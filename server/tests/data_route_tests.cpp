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
constexpr std::uint8_t kDataPacketType = 0x02;
constexpr std::uint8_t kDataForwardType = 0x12;
constexpr std::uint8_t kErrorType = 0x13;
constexpr std::uint8_t kChatMessageType = 0x05;
constexpr std::uint8_t kChatMessageForwardType = 0x25;
constexpr std::uint8_t kChatControlType = 0x06;
constexpr std::uint8_t kChatControlForwardType = 0x26;

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

std::vector<std::uint8_t> BuildData(std::uint32_t sessionId,
                                    std::uint32_t targetSession,
                                    const std::vector<std::uint8_t>& payload)
{
    mi::shared::proto::DataPacket pkt{};
    pkt.sessionId = sessionId;
    pkt.targetSessionId = targetSession;
    pkt.payload = payload;

    std::vector<std::uint8_t> buf;
    buf.push_back(kDataPacketType);
    const auto body = mi::shared::proto::SerializeDataPacket(pkt);
    buf.insert(buf.end(), body.begin(), body.end());
    return buf;
}

std::vector<std::uint8_t> BuildChat(std::uint32_t session,
                                    std::uint32_t target,
                                    std::uint64_t messageId,
                                    const std::vector<std::uint8_t>& payload)
{
    mi::shared::proto::ChatMessage msg{};
    msg.sessionId = session;
    msg.targetSessionId = target;
    msg.messageId = messageId;
    msg.payload = payload;
    std::vector<std::uint8_t> buf;
    buf.push_back(kChatMessageType);
    const auto body = mi::shared::proto::SerializeChatMessage(msg);
    buf.insert(buf.end(), body.begin(), body.end());
    return buf;
}

std::vector<std::uint8_t> BuildChatControl(std::uint32_t session,
                                           std::uint32_t target,
                                           std::uint64_t messageId)
{
    mi::shared::proto::ChatControl ctl{};
    ctl.sessionId = session;
    ctl.targetSessionId = target;
    ctl.messageId = messageId;
    ctl.action = 1;
    std::vector<std::uint8_t> buf;
    buf.push_back(kChatControlType);
    const auto body = mi::shared::proto::SerializeChatControl(ctl);
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
        std::wcerr << L"[data_test] server start failed\n";
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
        std::wcerr << L"[data_test] client start failed\n";
        return 1;
    }

    const PeerEndpoint serverPeer{L"127.0.0.1", serverPort};
    clientA.Send(serverPeer, BuildAuth(L"alice", L"pass"), 101);
    clientB.Send(serverPeer, BuildAuth(L"bob", L"pass"), 202);

    std::uint32_t sessionA = 0;
    std::uint32_t sessionB = 0;
    auto pumpServer = [&]() {
        server.Poll();
        mi::shared::net::ReceivedDatagram pkt{};
        while (server.TryReceive(pkt))
        {
            router.HandleIncoming(pkt);
        }
    };

    const auto authDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < authDeadline && (sessionA == 0 || sessionB == 0))
    {
        pumpServer();
        mi::shared::net::ReceivedDatagram pkt{};
        clientA.Poll();
        while (clientA.TryReceive(pkt))
        {
            if (pkt.payload.empty() || pkt.payload[0] != kAuthResponseType)
            {
                continue;
            }
            mi::shared::proto::AuthResponse resp{};
            std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
            if (mi::shared::proto::ParseAuthResponse(body, resp) && resp.success)
            {
                sessionA = resp.sessionId;
            }
        }

        clientB.Poll();
        while (clientB.TryReceive(pkt))
        {
            if (pkt.payload.empty() || pkt.payload[0] != kAuthResponseType)
            {
                continue;
            }
            mi::shared::proto::AuthResponse resp{};
            std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
            if (mi::shared::proto::ParseAuthResponse(body, resp) && resp.success)
            {
                sessionB = resp.sessionId;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (sessionA == 0 || sessionB == 0)
    {
        std::wcerr << L"[data_test] auth failed sessions A=" << sessionA << L" B=" << sessionB << L"\n";
        return 1;
    }

    const std::vector<std::uint8_t> payload{'h', 'e', 'l', 'l', 'o'};
    clientA.Send(serverPeer, BuildData(sessionA, sessionB, payload), sessionA);

    bool dataForwarded = false;
    bool missingTargetError = false;
    bool unauthorizedError = false;
    bool chatReceived = false;
    bool chatControlReceived = false;
    const auto forwardDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < forwardDeadline && (!dataForwarded || !chatReceived))
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
            if (pkt.payload[0] == kDataForwardType)
            {
                mi::shared::proto::DataPacket dp{};
                std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
                if (mi::shared::proto::ParseDataPacket(body, dp))
                {
                    dataForwarded = (dp.sessionId == sessionA && dp.targetSessionId == sessionB && dp.payload == payload);
                }
            }
            else if (pkt.payload[0] == kChatMessageForwardType)
            {
                mi::shared::proto::ChatMessage msg{};
                std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
                if (mi::shared::proto::ParseChatMessage(body, msg))
                {
                    chatReceived = (msg.payload == payload && msg.sessionId == sessionA && msg.targetSessionId == sessionB);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 触发不存在的目标错误
    clientA.Send(serverPeer, BuildData(sessionA, 999999u, payload), sessionA);
    const auto errorDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < errorDeadline && !missingTargetError)
    {
        pumpServer();
        mi::shared::net::ReceivedDatagram pkt{};
        clientA.Poll();
        while (clientA.TryReceive(pkt))
        {
            if (pkt.payload.empty() || pkt.payload[0] != kErrorType)
            {
                continue;
            }
            mi::shared::proto::ErrorResponse err{};
            std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
            if (mi::shared::proto::ParseErrorResponse(body, err))
            {
                missingTargetError = (err.code == 0x06);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 触发未注册会话错误
    clientB.Send(serverPeer, BuildData(424242u, sessionA, payload), 424242u);
    const auto unauthorizedDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < unauthorizedDeadline && !unauthorizedError)
    {
        pumpServer();
        mi::shared::net::ReceivedDatagram pkt{};
        clientB.Poll();
        while (clientB.TryReceive(pkt))
        {
            if (pkt.payload.empty() || pkt.payload[0] != kErrorType)
            {
                continue;
            }
            mi::shared::proto::ErrorResponse err{};
            std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
            if (mi::shared::proto::ParseErrorResponse(body, err))
            {
                unauthorizedError = (err.code == 0x05);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 聊天撤回
    const std::uint64_t messageId = 888;
    clientA.Send(serverPeer, BuildChat(sessionA, sessionB, messageId, payload), sessionA);
    const auto chatDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool chatControlSent = false;
    while (std::chrono::steady_clock::now() < chatDeadline && (!chatControlReceived))
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
            if (type == kChatMessageForwardType)
            {
                mi::shared::proto::ChatMessage msg{};
                if (mi::shared::proto::ParseChatMessage(body, msg) && msg.messageId == messageId)
                {
                    chatReceived = true;
                }
            }
            else if (type == kChatControlForwardType)
            {
                mi::shared::proto::ChatControl ctl{};
                if (mi::shared::proto::ParseChatControl(body, ctl) && ctl.messageId == messageId && ctl.action == 1)
                {
                    chatControlReceived = true;
                }
            }
        }
        if (chatReceived && !chatControlSent)
        {
            clientA.Send(serverPeer, BuildChatControl(sessionA, sessionB, messageId), sessionA);
            chatControlSent = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    clientA.Stop();
    clientB.Stop();
    server.Stop();

    const bool ok = dataForwarded && missingTargetError && unauthorizedError && chatReceived && chatControlReceived;
    if (!ok)
    {
        std::wcerr << L"[data_test] result forward=" << (dataForwarded ? 1 : 0) << L" missingTarget=" << (missingTargetError ? 1 : 0)
                   << L" unauthorized=" << (unauthorizedError ? 1 : 0) << L" chat=" << (chatReceived ? 1 : 0)
                   << L" chatControl=" << (chatControlReceived ? 1 : 0) << L"\n";
    }
    return ok ? 0 : 1;
}
