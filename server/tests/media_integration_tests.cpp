#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "mi/shared/net/kcp_channel.hpp"
#include "mi/shared/proto/messages.hpp"
#include "server/auth_service.hpp"
#include "server/message_router.hpp"

namespace
{
constexpr std::uint8_t kAuthRequestType = 0x01;
constexpr std::uint8_t kAuthResponseType = 0x11;
constexpr std::uint8_t kMediaChunkType = 0x03;
constexpr std::uint8_t kMediaForwardType = 0x23;
constexpr std::uint8_t kMediaControlType = 0x04;
constexpr std::uint8_t kMediaControlForwardType = 0x24;

std::vector<std::uint8_t> BuildAuth(const std::wstring& user, const std::wstring& pass)
{
    mi::shared::proto::AuthRequest req{};
    req.username = user;
    req.password = pass;
    std::vector<std::uint8_t> buf;
    buf.push_back(kAuthRequestType);
    const auto body = mi::shared::proto::SerializeAuthRequest(req);
    buf.insert(buf.end(), body.begin(), body.end());
    return buf;
}

std::vector<std::uint8_t> BuildMediaChunk(std::uint32_t session,
                                          std::uint32_t target,
                                          std::uint64_t mediaId,
                                          const std::vector<std::uint8_t>& payload)
{
    mi::shared::proto::MediaChunk chunk{};
    chunk.sessionId = session;
    chunk.targetSessionId = target;
    chunk.mediaId = mediaId;
    chunk.chunkIndex = 0;
    chunk.totalChunks = 1;
    chunk.totalSize = static_cast<std::uint32_t>(payload.size());
    chunk.name = L"test.bin";
    chunk.payload = payload;

    std::vector<std::uint8_t> buf;
    buf.push_back(kMediaChunkType);
    const auto body = mi::shared::proto::SerializeMediaChunk(chunk);
    buf.insert(buf.end(), body.begin(), body.end());
    return buf;
}

std::vector<std::uint8_t> BuildMediaControl(std::uint32_t session,
                                            std::uint32_t target,
                                            std::uint64_t mediaId)
{
    mi::shared::proto::MediaControl ctl{};
    ctl.sessionId = session;
    ctl.targetSessionId = target;
    ctl.mediaId = mediaId;
    ctl.action = 1;
    std::vector<std::uint8_t> buf;
    buf.push_back(kMediaControlType);
    const auto body = mi::shared::proto::SerializeMediaControl(ctl);
    buf.insert(buf.end(), body.begin(), body.end());
    return buf;
}
}  // namespace

int main()
{
    using mi::shared::net::KcpChannel;
    using mi::shared::net::PeerEndpoint;

    mi::server::AuthService auth({mi::server::UserCredential{L"alice", L"pass"},
                                  mi::server::UserCredential{L"bob", L"pass"}});
    KcpChannel server;
    server.Configure({});
    if (!server.Start(L"127.0.0.1", 0))
    {
        printf("[media_test] server start failed\n");
        return 1;
    }
    const uint16_t serverPort = server.BoundPort();
    mi::server::MessageRouter router(auth, server);

    KcpChannel clientA;
    KcpChannel clientB;
    clientA.Configure({});
    clientB.Configure({});
    if (!clientA.Start(L"127.0.0.1", 0) || !clientB.Start(L"127.0.0.1", 0))
    {
        printf("[media_test] client start failed\n");
        return 1;
    }

    const PeerEndpoint serverPeer{L"127.0.0.1", serverPort};
    printf("[media_test] serverPort=%u\n", serverPort);
    const bool queuedAuthA = clientA.Send(serverPeer, BuildAuth(L"alice", L"pass"), 101);
    const bool queuedAuthB = clientB.Send(serverPeer, BuildAuth(L"bob", L"pass"), 202);
    if (!queuedAuthA || !queuedAuthB)
    {
        printf("[media_test] auth send failed queuedA=%d queuedB=%d\n", queuedAuthA ? 1 : 0, queuedAuthB ? 1 : 0);
        return 1;
    }

    std::uint32_t sessionA = 0;
    std::uint32_t sessionB = 0;
    std::size_t serverRecvCount = 0;
    std::size_t clientARecvCount = 0;
    std::size_t clientBRecvCount = 0;
    std::size_t serverAuthReqCount = 0;
    std::size_t serverMediaCount = 0;

    auto pumpServer = [&]() {
        server.Poll();
        mi::shared::net::ReceivedDatagram pkt{};
        while (server.TryReceive(pkt))
        {
            ++serverRecvCount;
            if (!pkt.payload.empty())
            {
                if (pkt.payload[0] == kAuthRequestType)
                {
                    ++serverAuthReqCount;
                }
                else if (pkt.payload[0] == kMediaChunkType)
                {
                    ++serverMediaCount;
                }
            }
            router.HandleIncoming(pkt);
        }
    };

    for (int i = 0; i < 400 && (sessionA == 0 || sessionB == 0); ++i)
    {
        pumpServer();
        mi::shared::net::ReceivedDatagram pkt{};
        clientA.Poll();
        while (clientA.TryReceive(pkt))
        {
            if (pkt.payload.empty())
            {
                continue;
            }
            ++clientARecvCount;
            const std::uint8_t type = pkt.payload[0];
            if (type == kAuthResponseType)
            {
                mi::shared::proto::AuthResponse resp{};
                std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
                if (mi::shared::proto::ParseAuthResponse(body, resp) && resp.success)
                {
                    sessionA = resp.sessionId;
                }
            }
        }

        clientB.Poll();
        while (clientB.TryReceive(pkt))
        {
            if (pkt.payload.empty())
            {
                continue;
            }
            ++clientBRecvCount;
            const std::uint8_t type = pkt.payload[0];
            if (type == kAuthResponseType)
            {
                mi::shared::proto::AuthResponse resp{};
                std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
                if (mi::shared::proto::ParseAuthResponse(body, resp) && resp.success)
                {
                    sessionB = resp.sessionId;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (sessionA == 0 || sessionB == 0)
    {
        std::wcerr << L"[media_test] session init failed A=" << sessionA << L" B=" << sessionB << std::endl;
        printf("[media_test] serverRecv=%zu authReq=%zu mediaRecv=%zu clientARecv=%zu clientBRecv=%zu\n",
               serverRecvCount,
               serverAuthReqCount,
               serverMediaCount,
               clientARecvCount,
               clientBRecvCount);
        return 1;
    }
    printf("[media_test] sessions A=%u B=%u\n", sessionA, sessionB);
    const auto peerB = server.FindPeer(sessionB);
    if (peerB.port == 0)
    {
        printf("[media_test] peerB not registered\n");
        return 1;
    }

    const std::vector<std::uint8_t> mediaPayload = {1, 2, 3, 4, 5};
    const std::uint64_t mediaId = 42;
    const bool mediaQueued =
        clientA.Send(serverPeer, BuildMediaChunk(sessionA, sessionB, mediaId, mediaPayload), sessionA);
    if (!mediaQueued)
    {
        printf("[media_test] media send failed\n");
        return 1;
    }

    bool mediaReceived = false;
    bool controlReceived = false;
    bool controlSent = false;
    std::size_t clientBMediaForwardCount = 0;
    std::size_t clientBControlForwardCount = 0;

    for (int i = 0; i < 1000 && (!mediaReceived || !controlReceived); ++i)
    {
        pumpServer();
        mi::shared::net::ReceivedDatagram pkt{};
        clientB.Poll();
        while (clientB.TryReceive(pkt))
        {
            if (pkt.payload.empty())
            {
                continue;
            }
            const std::uint8_t type = pkt.payload[0];
            std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
            if (type == kMediaForwardType)
            {
                ++clientBMediaForwardCount;
                mi::shared::proto::MediaChunk mc{};
                if (mi::shared::proto::ParseMediaChunk(body, mc))
                {
                    mediaReceived = (mc.mediaId == mediaId && mc.payload == mediaPayload &&
                                     mc.sessionId == sessionA && mc.targetSessionId == sessionB);
                    if (!mediaReceived)
                    {
                        std::wcerr << L"[media_test] media mismatch or parse fail" << std::endl;
                    }
                }
            }
            else if (type == kMediaControlForwardType)
            {
                ++clientBControlForwardCount;
                mi::shared::proto::MediaControl ctl{};
                if (mi::shared::proto::ParseMediaControl(body, ctl))
                {
                    controlReceived = (ctl.mediaId == mediaId && ctl.action == 1);
                    if (!controlReceived)
                    {
                        std::wcerr << L"[media_test] control parse mismatch" << std::endl;
                    }
                }
            }
            else
            {
                std::cout << "[media_test] clientB received type=" << static_cast<int>(type)
                          << " size=" << pkt.payload.size() << std::endl;
            }
        }

        if (mediaReceived && !controlReceived && !controlSent)
        {
            clientA.Send(serverPeer, BuildMediaControl(sessionA, sessionB, mediaId), sessionA);
            controlSent = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    printf("[media_test] result media=%d control=%d serverRecv=%zu authReq=%zu mediaRecv=%zu clientARecv=%zu clientBRecv=%zu "
           "clientBMedia=%zu clientBCtrl=%zu\n",
           mediaReceived ? 1 : 0,
           controlReceived ? 1 : 0,
           serverRecvCount,
           serverAuthReqCount,
           serverMediaCount,
           clientARecvCount,
           clientBRecvCount,
            clientBMediaForwardCount,
            clientBControlForwardCount);

    return (mediaReceived && controlReceived) ? 0 : 1;
}
