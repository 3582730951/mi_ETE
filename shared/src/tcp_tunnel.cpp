#include "mi/shared/net/tcp_tunnel.hpp"

#include <algorithm>
#include <iostream>

#include "mi/shared/proto/messages.hpp"

#ifdef _WIN32
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace
{
constexpr std::size_t kHeaderSize = 1 + sizeof(std::uint32_t) + sizeof(std::uint16_t);  // type + connId + length
constexpr std::uint8_t kDataPacketType = 0x02;
constexpr std::uint8_t kDataForwardType = 0x12;

#ifdef _WIN32
bool EnsureWinsock()
{
    static bool ready = false;
    static bool attempted = false;
    if (ready)
    {
        return true;
    }
    if (attempted)
    {
        return false;
    }
    attempted = true;
    WSADATA wsaData{};
    ready = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    return ready;
}

std::wstring ToWideMessage(DWORD errorCode)
{
    wchar_t* buffer = nullptr;
    const DWORD size = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        errorCode,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer),
                                        0,
                                        nullptr);
    if (size == 0 || buffer == nullptr)
    {
        return L"unknown";
    }

    std::wstring msg(buffer, size);
    ::LocalFree(buffer);
    return msg;
}

bool SetNonBlocking(SOCKET sock)
{
    u_long nb = 1;
    return ::ioctlsocket(sock, FIONBIO, &nb) == 0;
}
#endif
}  // namespace

namespace mi::shared::net
{
TcpTunnelClient::TcpTunnelClient(KcpChannel& channel, TcpTunnelClientConfig config)
    : channel_(channel),
      config_(std::move(config)),
      running_(false),
      listenSock_(0),
      listenPort_(0),
      nextConnId_(1),
      connections_()
{
}

TcpTunnelClient::~TcpTunnelClient()
{
    Stop();
}

bool TcpTunnelClient::Start()
{
#ifndef _WIN32
    std::wcerr << L"[tcp_tunnel] 仅在 Windows 下实现\n";
    return false;
#else
    if (!EnsureWinsock())
    {
        std::wcerr << L"[tcp_tunnel] 初始化 Winsock 失败\n";
        return false;
    }
    if (!CreateListener())
    {
        return false;
    }
    running_ = true;
    std::wcout << L"[tcp_tunnel] client 监听 " << config_.listenHost << L":" << listenPort_ << L"\n";
    return true;
#endif
}

void TcpTunnelClient::Poll()
{
    if (!running_)
    {
        return;
    }
    AcceptNew();
    FlushTcpToKcp();
    FlushKcpToTcp();
}

void TcpTunnelClient::Stop()
{
    if (!running_)
    {
        return;
    }
#ifdef _WIN32
    if (listenSock_ != 0)
    {
        ::closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = 0;
    }
    for (auto& kv : connections_)
    {
        ::closesocket(static_cast<SOCKET>(kv.second));
    }
#endif
    connections_.clear();
    running_ = false;
    listenPort_ = 0;
}

bool TcpTunnelClient::CreateListener()
{
#ifndef _WIN32
    return false;
#else
    SOCKET sock = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
    if (sock == INVALID_SOCKET)
    {
        std::wcerr << L"[tcp_tunnel] 创建监听 socket 失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        return false;
    }

    if (!SetNonBlocking(sock))
    {
        std::wcerr << L"[tcp_tunnel] 设置非阻塞失败\n";
        ::closesocket(sock);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.listenPort);
    if (::InetPtonW(AF_INET, config_.listenHost.c_str(), &addr.sin_addr) != 1)
    {
        std::wcerr << L"[tcp_tunnel] 监听地址解析失败: " << config_.listenHost << L"\n";
        ::closesocket(sock);
        return false;
    }

    if (::bind(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) != 0)
    {
        std::wcerr << L"[tcp_tunnel] 绑定失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        ::closesocket(sock);
        return false;
    }
    if (::listen(sock, SOMAXCONN) != 0)
    {
        std::wcerr << L"[tcp_tunnel] listen 失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        ::closesocket(sock);
        return false;
    }

    sockaddr_in bound{};
    int len = sizeof(bound);
    if (::getsockname(sock, reinterpret_cast<SOCKADDR*>(&bound), &len) == 0)
    {
        listenPort_ = ntohs(bound.sin_port);
    }
    else
    {
        listenPort_ = config_.listenPort;
    }

    listenSock_ = static_cast<std::uintptr_t>(sock);
    return true;
#endif
}

void TcpTunnelClient::AcceptNew()
{
#ifndef _WIN32
    return;
#else
    SOCKET listener = static_cast<SOCKET>(listenSock_);
    while (true)
    {
        sockaddr_in remote{};
        int remoteLen = sizeof(remote);
        SOCKET conn = ::accept(listener, reinterpret_cast<SOCKADDR*>(&remote), &remoteLen);
        if (conn == INVALID_SOCKET)
        {
            const int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
            {
                std::wcerr << L"[tcp_tunnel] accept 失败: " << ToWideMessage(err) << L"\n";
            }
            break;
        }
        if (!SetNonBlocking(conn))
        {
            std::wcerr << L"[tcp_tunnel] 设置非阻塞失败\n";
            ::closesocket(conn);
            continue;
        }

        const std::uint32_t connId = nextConnId_++;
        connections_[connId] = static_cast<std::uintptr_t>(conn);

        Frame open{};
        open.type = TunnelFrameType::Open;
        open.connId = connId;
        SendFrame(open);
        std::wcout << L"[tcp_tunnel] 新连接 " << connId << L" 已建立并通知远端\n";
    }
#endif
}

bool TcpTunnelClient::SendFrame(const Frame& frame)
{
    std::vector<std::uint8_t> buf;
    buf.reserve(kHeaderSize + frame.payload.size());
    buf.push_back(static_cast<std::uint8_t>(frame.type));
    const std::uint32_t cid = frame.connId;
    const std::uint16_t length = static_cast<std::uint16_t>(frame.payload.size());
    buf.push_back(static_cast<std::uint8_t>(cid & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((cid >> 8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((cid >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((cid >> 24) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>(length & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFu));
    buf.insert(buf.end(), frame.payload.begin(), frame.payload.end());
    if (config_.viaRouter)
    {
        mi::shared::proto::DataPacket pkt{};
        pkt.sessionId = config_.sessionId;
        pkt.targetSessionId = (config_.targetSessionId != 0) ? config_.targetSessionId : config_.sessionId;
        pkt.payload = buf;

        std::vector<std::uint8_t> out;
        out.push_back(kDataPacketType);
        const auto body = mi::shared::proto::SerializeDataPacket(pkt);
        out.insert(out.end(), body.begin(), body.end());
        return channel_.Send(config_.routerPeer, out, config_.sessionId);
    }
    return channel_.Send(config_.remotePeer, buf, config_.sessionId);
}

bool TcpTunnelClient::ParseFrame(const std::vector<std::uint8_t>& buf, Frame& frame) const
{
    if (buf.size() < kHeaderSize)
    {
        return false;
    }

    const auto type = buf[0];
    if (type != static_cast<std::uint8_t>(TunnelFrameType::Open) && type != static_cast<std::uint8_t>(TunnelFrameType::Data) &&
        type != static_cast<std::uint8_t>(TunnelFrameType::Close))
    {
        return false;
    }
    frame.type = static_cast<TunnelFrameType>(type);
    frame.connId = static_cast<std::uint32_t>(buf[1]) | (static_cast<std::uint32_t>(buf[2]) << 8) |
                   (static_cast<std::uint32_t>(buf[3]) << 16) | (static_cast<std::uint32_t>(buf[4]) << 24);
    const std::uint16_t length = static_cast<std::uint16_t>(buf[5]) | (static_cast<std::uint16_t>(buf[6]) << 8);
    if (buf.size() < kHeaderSize + length)
    {
        return false;
    }
    frame.payload.assign(buf.begin() + static_cast<long long>(kHeaderSize),
                         buf.begin() + static_cast<long long>(kHeaderSize + length));
    return true;
}

void TcpTunnelClient::FlushTcpToKcp()
{
#ifndef _WIN32
    return;
#else
    std::vector<std::uint32_t> toClose;
    for (auto& kv : connections_)
    {
        SOCKET sock = static_cast<SOCKET>(kv.second);
        char buffer[2048] = {0};
        int received = ::recv(sock, buffer, static_cast<int>(std::min<std::size_t>(sizeof(buffer), config_.maxFramePayload)), 0);
        while (received > 0)
        {
            Frame frame{};
            frame.type = TunnelFrameType::Data;
            frame.connId = kv.first;
            frame.payload.assign(buffer, buffer + received);
            SendFrame(frame);
            received = ::recv(sock, buffer, static_cast<int>(std::min<std::size_t>(sizeof(buffer), config_.maxFramePayload)), 0);
        }
        if (received == 0)
        {
            Frame close{};
            close.type = TunnelFrameType::Close;
            close.connId = kv.first;
            SendFrame(close);
            toClose.push_back(kv.first);
        }
        else if (received < 0)
        {
            const int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
            {
                Frame close{};
                close.type = TunnelFrameType::Close;
                close.connId = kv.first;
                SendFrame(close);
                toClose.push_back(kv.first);
            }
        }
    }
    for (std::uint32_t cid : toClose)
    {
        CloseConnection(cid);
    }
#endif
}

void TcpTunnelClient::FlushKcpToTcp()
{
    mi::shared::net::ReceivedDatagram pkt{};
    while (channel_.TryReceive(pkt))
    {
        const std::vector<std::uint8_t>* payload = &pkt.payload;
        std::vector<std::uint8_t> unpacked;
        if (config_.viaRouter && !pkt.payload.empty() && pkt.payload[0] == kDataForwardType)
        {
            mi::shared::proto::DataPacket dp{};
            std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
            if (mi::shared::proto::ParseDataPacket(body, dp))
            {
                unpacked = std::move(dp.payload);
                payload = &unpacked;
            }
            else
            {
                continue;
            }
        }

        Frame frame{};
        if (!ParseFrame(*payload, frame))
        {
            continue;
        }

        auto it = connections_.find(frame.connId);
        if (it == connections_.end())
        {
            continue;
        }
#ifdef _WIN32
        SOCKET sock = static_cast<SOCKET>(it->second);
        if (frame.type == TunnelFrameType::Data && !frame.payload.empty())
        {
            const int sent = ::send(sock,
                                    reinterpret_cast<const char*>(frame.payload.data()),
                                    static_cast<int>(frame.payload.size()),
                                    0);
            if (sent == SOCKET_ERROR)
            {
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
                {
                    CloseConnection(frame.connId);
                }
            }
        }
        else if (frame.type == TunnelFrameType::Close)
        {
            CloseConnection(frame.connId);
        }
#endif
    }
}

void TcpTunnelClient::CloseConnection(std::uint32_t connId)
{
#ifdef _WIN32
    auto it = connections_.find(connId);
    if (it != connections_.end())
    {
        SOCKET sock = static_cast<SOCKET>(it->second);
        ::closesocket(sock);
        connections_.erase(it);
        std::wcout << L"[tcp_tunnel] 连接 " << connId << L" 已关闭\n";
    }
#else
    (void)connId;
#endif
}

TcpTunnelServer::TcpTunnelServer(KcpChannel& channel, TcpTunnelServerConfig config)
    : channel_(channel), config_(std::move(config)), running_(false), connections_()
{
}

TcpTunnelServer::~TcpTunnelServer()
{
    Stop();
}

bool TcpTunnelServer::Start()
{
#ifndef _WIN32
    std::wcerr << L"[tcp_tunnel] 仅在 Windows 下实现\n";
    return false;
#else
    if (!EnsureWinsock())
    {
        std::wcerr << L"[tcp_tunnel] 初始化 Winsock 失败\n";
        return false;
    }
    running_ = true;
    std::wcout << L"[tcp_tunnel] server 已启动，目标 " << config_.targetHost << L":" << config_.targetPort << L"\n";
    return true;
#endif
}

void TcpTunnelServer::Poll()
{
    if (!running_)
    {
        return;
    }
    FlushKcpToTcp();
    FlushTcpToKcp();
}

void TcpTunnelServer::Stop()
{
    if (!running_)
    {
        return;
    }
#ifdef _WIN32
    for (auto& kv : connections_)
    {
        ::closesocket(static_cast<SOCKET>(kv.second));
    }
#endif
    connections_.clear();
    running_ = false;
}

bool TcpTunnelServer::ParseFrame(const std::vector<std::uint8_t>& buf, Frame& frame) const
{
    if (buf.size() < kHeaderSize)
    {
        return false;
    }

    const auto type = buf[0];
    if (type != static_cast<std::uint8_t>(TunnelFrameType::Open) && type != static_cast<std::uint8_t>(TunnelFrameType::Data) &&
        type != static_cast<std::uint8_t>(TunnelFrameType::Close))
    {
        return false;
    }
    frame.type = static_cast<TunnelFrameType>(type);
    frame.connId = static_cast<std::uint32_t>(buf[1]) | (static_cast<std::uint32_t>(buf[2]) << 8) |
                   (static_cast<std::uint32_t>(buf[3]) << 16) | (static_cast<std::uint32_t>(buf[4]) << 24);
    const std::uint16_t length = static_cast<std::uint16_t>(buf[5]) | (static_cast<std::uint16_t>(buf[6]) << 8);
    if (buf.size() < kHeaderSize + length)
    {
        return false;
    }
    frame.payload.assign(buf.begin() + static_cast<long long>(kHeaderSize),
                         buf.begin() + static_cast<long long>(kHeaderSize + length));
    return true;
}

bool TcpTunnelServer::SendFrame(const Frame& frame)
{
    std::vector<std::uint8_t> buf;
    buf.reserve(kHeaderSize + frame.payload.size());
    buf.push_back(static_cast<std::uint8_t>(frame.type));
    const std::uint32_t cid = frame.connId;
    const std::uint16_t length = static_cast<std::uint16_t>(frame.payload.size());
    buf.push_back(static_cast<std::uint8_t>(cid & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((cid >> 8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((cid >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((cid >> 24) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>(length & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFu));
    buf.insert(buf.end(), frame.payload.begin(), frame.payload.end());
    if (config_.viaRouter)
    {
        mi::shared::proto::DataPacket pkt{};
        pkt.sessionId = config_.sessionId;
        pkt.targetSessionId = (config_.targetSessionId != 0) ? config_.targetSessionId : config_.sessionId;
        pkt.payload = buf;

        std::vector<std::uint8_t> out;
        out.push_back(kDataPacketType);
        const auto body = mi::shared::proto::SerializeDataPacket(pkt);
        out.insert(out.end(), body.begin(), body.end());
        return channel_.Send(config_.routerPeer, out, config_.sessionId);
    }
    return channel_.Send(config_.remotePeer, buf, config_.sessionId);
}

void TcpTunnelServer::FlushKcpToTcp()
{
    mi::shared::net::ReceivedDatagram pkt{};
    while (channel_.TryReceive(pkt))
    {
        const std::vector<std::uint8_t>* payload = &pkt.payload;
        std::vector<std::uint8_t> unpacked;
        if (config_.viaRouter && !pkt.payload.empty() && pkt.payload[0] == kDataForwardType)
        {
            mi::shared::proto::DataPacket dp{};
            std::vector<std::uint8_t> body(pkt.payload.begin() + 1, pkt.payload.end());
            if (mi::shared::proto::ParseDataPacket(body, dp))
            {
                unpacked = std::move(dp.payload);
                payload = &unpacked;
            }
            else
            {
                continue;
            }
        }

        Frame frame{};
        if (!ParseFrame(*payload, frame))
        {
            continue;
        }
#ifdef _WIN32
        if (frame.type == TunnelFrameType::Open)
        {
            SOCKET sock = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
            if (sock == INVALID_SOCKET)
            {
                continue;
            }
            if (!SetNonBlocking(sock))
            {
                ::closesocket(sock);
                continue;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(config_.targetPort);
            if (::InetPtonW(AF_INET, config_.targetHost.c_str(), &addr.sin_addr) != 1)
            {
                ::closesocket(sock);
                continue;
            }
            const int rc = ::connect(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr));
            if (rc == SOCKET_ERROR)
            {
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
                {
                    ::closesocket(sock);
                    continue;
                }
            }
            connections_[frame.connId] = static_cast<std::uintptr_t>(sock);
            std::wcout << L"[tcp_tunnel] server 连接目标建立 connId=" << frame.connId << L"\n";
        }
        else if (frame.type == TunnelFrameType::Data)
        {
            auto it = connections_.find(frame.connId);
            if (it == connections_.end())
            {
                continue;
            }
            SOCKET sock = static_cast<SOCKET>(it->second);
            const int sent = ::send(sock,
                                    reinterpret_cast<const char*>(frame.payload.data()),
                                    static_cast<int>(frame.payload.size()),
                                    0);
            if (sent == SOCKET_ERROR)
            {
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
                {
                    CloseConnection(frame.connId);
                }
            }
        }
        else if (frame.type == TunnelFrameType::Close)
        {
            CloseConnection(frame.connId);
        }
#endif
    }
}

void TcpTunnelServer::FlushTcpToKcp()
{
#ifndef _WIN32
    return;
#else
    std::vector<std::uint32_t> toClose;
    for (auto& kv : connections_)
    {
        SOCKET sock = static_cast<SOCKET>(kv.second);
        char buffer[2048] = {0};
        int received = ::recv(sock, buffer, static_cast<int>(std::min<std::size_t>(sizeof(buffer), config_.maxFramePayload)), 0);
        while (received > 0)
        {
            Frame frame{};
            frame.type = TunnelFrameType::Data;
            frame.connId = kv.first;
            frame.payload.assign(buffer, buffer + received);
            SendFrame(frame);
            received = ::recv(sock, buffer, static_cast<int>(std::min<std::size_t>(sizeof(buffer), config_.maxFramePayload)), 0);
        }
        if (received == 0)
        {
            Frame close{};
            close.type = TunnelFrameType::Close;
            close.connId = kv.first;
            SendFrame(close);
            toClose.push_back(kv.first);
        }
        else if (received < 0)
        {
            const int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
            {
                Frame close{};
                close.type = TunnelFrameType::Close;
                close.connId = kv.first;
                SendFrame(close);
                toClose.push_back(kv.first);
            }
        }
    }

    for (std::uint32_t cid : toClose)
    {
        CloseConnection(cid);
    }
#endif
}

void TcpTunnelServer::CloseConnection(std::uint32_t connId)
{
#ifdef _WIN32
    auto it = connections_.find(connId);
    if (it != connections_.end())
    {
        ::closesocket(static_cast<SOCKET>(it->second));
        connections_.erase(it);
        std::wcout << L"[tcp_tunnel] server 连接 " << connId << L" 已关闭\n";
    }
#else
    (void)connId;
#endif
}
}  // namespace mi::shared::net
