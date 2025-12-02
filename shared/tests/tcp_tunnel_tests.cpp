#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "mi/shared/net/kcp_channel.hpp"
#include "mi/shared/net/tcp_tunnel.hpp"

#ifdef _WIN32
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace
{
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

struct EchoServer
{
    SOCKET listenSock = INVALID_SOCKET;
    uint16_t port = 0;
    std::thread worker;
    bool running = false;
};

bool StartEcho(EchoServer& server)
{
    if (!EnsureWinsock())
    {
        return false;
    }
    SOCKET sock = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
    if (sock == INVALID_SOCKET)
    {
        return false;
    }

    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    InetPtonW(AF_INET, L"127.0.0.1", &addr.sin_addr);
    if (::bind(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) != 0)
    {
        ::closesocket(sock);
        return false;
    }
    if (::listen(sock, SOMAXCONN) != 0)
    {
        ::closesocket(sock);
        return false;
    }
    sockaddr_in bound{};
    int len = sizeof(bound);
    if (::getsockname(sock, reinterpret_cast<SOCKADDR*>(&bound), &len) != 0)
    {
        ::closesocket(sock);
        return false;
    }
    server.listenSock = sock;
    server.port = ntohs(bound.sin_port);
    server.running = true;
    server.worker = std::thread([&server]() {
        SOCKET conn = INVALID_SOCKET;
        while (server.running)
        {
            if (conn == INVALID_SOCKET)
            {
                sockaddr_in remote{};
                int rlen = sizeof(remote);
                conn = ::accept(server.listenSock, reinterpret_cast<SOCKADDR*>(&remote), &rlen);
                if (conn == INVALID_SOCKET)
                {
                    const int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                u_long nbConn = 1;
                ioctlsocket(conn, FIONBIO, &nbConn);
            }
            else
            {
                char buffer[1024] = {0};
                int received = ::recv(conn, buffer, sizeof(buffer), 0);
                if (received > 0)
                {
                    ::send(conn, buffer, received, 0);
                }
                else if (received == 0)
                {
                    ::closesocket(conn);
                    conn = INVALID_SOCKET;
                }
                else
                {
                    const int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
                    {
                        ::closesocket(conn);
                        conn = INVALID_SOCKET;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (conn != INVALID_SOCKET)
        {
            ::closesocket(conn);
        }
    });
    return true;
}

void StopEcho(EchoServer& server)
{
    server.running = false;
    if (server.worker.joinable())
    {
        server.worker.join();
    }
    if (server.listenSock != INVALID_SOCKET)
    {
        ::closesocket(server.listenSock);
        server.listenSock = INVALID_SOCKET;
    }
}
#endif
}  // namespace

int main()
{
#ifndef _WIN32
    std::cout << "[tcp_tunnel_test] skipped (Windows only)" << std::endl;
    return 0;
#else
    assert(EnsureWinsock());

    EchoServer echo{};
    assert(StartEcho(echo));

    mi::shared::net::KcpSettings settings{};
    settings.intervalMs = 5;

    mi::shared::net::KcpChannel kcpClient;
    mi::shared::net::KcpChannel kcpServer;
    kcpClient.Configure(settings);
    kcpServer.Configure(settings);

    assert(kcpClient.Start(L"127.0.0.1", 0));
    assert(kcpServer.Start(L"127.0.0.1", 0));

    const mi::shared::net::PeerEndpoint peerClient{L"127.0.0.1", kcpClient.BoundPort()};
    const mi::shared::net::PeerEndpoint peerServer{L"127.0.0.1", kcpServer.BoundPort()};

    mi::shared::net::TcpTunnelServerConfig serverCfg{};
    serverCfg.remotePeer = peerClient;
    serverCfg.sessionId = 9001;
    serverCfg.targetHost = L"127.0.0.1";
    serverCfg.targetPort = echo.port;

    mi::shared::net::TcpTunnelClientConfig clientCfg{};
    clientCfg.remotePeer = peerServer;
    clientCfg.sessionId = 9001;
    clientCfg.listenHost = L"127.0.0.1";
    clientCfg.listenPort = 0;

    mi::shared::net::TcpTunnelServer tunnelServer(kcpServer, serverCfg);
    mi::shared::net::TcpTunnelClient tunnelClient(kcpClient, clientCfg);
    assert(tunnelServer.Start());
    assert(tunnelClient.Start());

    // 使用本地 socket 连接到隧道入口
    SOCKET local = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
    assert(local != INVALID_SOCKET);
    sockaddr_in connectAddr{};
    connectAddr.sin_family = AF_INET;
    connectAddr.sin_port = htons(tunnelClient.ListenPort());
    InetPtonW(AF_INET, L"127.0.0.1", &connectAddr.sin_addr);
    assert(::connect(local, reinterpret_cast<SOCKADDR*>(&connectAddr), sizeof(connectAddr)) == 0);

    const std::string payload = "tcp_kcp_tunnel_payload";
    assert(::send(local, payload.data(), static_cast<int>(payload.size()), 0) ==
           static_cast<int>(payload.size()));

    bool received = false;
    std::string echoed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !received)
    {
        kcpClient.Poll();
        kcpServer.Poll();
        tunnelClient.Poll();
        tunnelServer.Poll();

        char buf[1024] = {0};
        const int r = ::recv(local, buf, sizeof(buf), 0);
        if (r > 0)
        {
            echoed.append(buf, buf + r);
            if (echoed == payload)
            {
                received = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ::closesocket(local);
    tunnelClient.Stop();
    tunnelServer.Stop();
    kcpClient.Stop();
    kcpServer.Stop();
    StopEcho(echo);

    assert(received);
    return 0;
#endif
}
