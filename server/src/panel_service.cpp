#include "server/panel_service.hpp"

#include <chrono>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace mi::server
{
namespace
{
#ifdef _WIN32
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
#endif
}  // namespace

PanelService::PanelService() : running_(false), responder_(), worker_(), socketHandle_(0), token_()
{
}

PanelService::~PanelService()
{
    Stop();
}

bool PanelService::Start(const std::wstring& host, uint16_t port, PanelResponder responder, std::wstring token)
{
    if (running_.load())
    {
        return true;
    }
    responder_ = std::move(responder);
    token_ = std::move(token);
    running_.store(true);
    worker_ = std::thread([this, host, port]() { Serve(host, port); });
    std::wcout << L"[panel] 面板监听 " << host << L":" << port << L"\n";
    return true;
}

void PanelService::Serve(const std::wstring& host, uint16_t port)
{
#ifndef _WIN32
    std::wcerr << L"[panel] 仅支持 Windows 占位\n";
    return;
#else
    SOCKET sock = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
    if (sock == INVALID_SOCKET)
    {
        std::wcerr << L"[panel] 创建 socket 失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        running_.store(false);
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::InetPtonW(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        std::wcerr << L"[panel] 地址解析失败: " << host << L"\n";
        ::closesocket(sock);
        running_.store(false);
        return;
    }

    if (::bind(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) != 0)
    {
        std::wcerr << L"[panel] 绑定失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        ::closesocket(sock);
        running_.store(false);
        return;
    }
    if (::listen(sock, SOMAXCONN) != 0)
    {
        std::wcerr << L"[panel] listen 失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        ::closesocket(sock);
        running_.store(false);
        return;
    }

    socketHandle_ = static_cast<std::uintptr_t>(sock);
    while (running_.load())
    {
        sockaddr_in remote{};
        int rlen = sizeof(remote);
        SOCKET conn = ::accept(sock, reinterpret_cast<SOCKADDR*>(&remote), &rlen);
        if (conn == INVALID_SOCKET)
        {
            const int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && running_.load())
            {
                std::wcerr << L"[panel] accept 失败: " << ToWideMessage(err) << L"\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        char buf[512] = {0};
        const int received = ::recv(conn, buf, sizeof(buf) - 1, 0);
        std::string req;
        if (received > 0)
        {
            req.assign(buf, buf + received);
        }
        std::string path = "/";
        const auto lineEnd = req.find("\r\n");
        if (lineEnd != std::string::npos)
        {
            const std::string line = req.substr(0, lineEnd);
            const auto firstSpace = line.find(' ');
            const auto secondSpace = (firstSpace != std::string::npos) ? line.find(' ', firstSpace + 1) : std::string::npos;
            if (firstSpace != std::string::npos && secondSpace != std::string::npos && secondSpace > firstSpace + 1)
            {
                path = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            }
        }

        bool authorized = true;
        if (!token_.empty())
        {
            const std::string tokenHeader = "x-panel-token:";
            const auto pos = req.find(tokenHeader);
            if (pos == std::string::npos)
            {
                authorized = false;
            }
            else
            {
                const auto endLine = req.find("\r\n", pos);
                const std::string line = req.substr(pos + tokenHeader.size(), endLine - (pos + tokenHeader.size()));
                std::string trimmed = line;
                trimmed.erase(0, trimmed.find_first_not_of(" \t"));
                if (trimmed.empty())
                {
                    authorized = false;
                }
                else
                {
                    std::wstring wline(trimmed.begin(), trimmed.end());
                    if (wline != token_)
                    {
                        authorized = false;
                    }
                }
            }
        }

        std::string body = responder_ ? responder_(path) : "{}";
        int statusCode = 200;
        if (body.empty())
        {
            body = "{\"error\":\"not_found\"}";
            statusCode = 404;
        }
        std::ostringstream oss;
        if (!authorized)
        {
            const std::string deny = "{\"error\":\"unauthorized\"}";
            oss << "HTTP/1.1 401 Unauthorized\r\n"
                << "Content-Type: application/json; charset=utf-8\r\n"
                << "Content-Length: " << deny.size() << "\r\n"
                << "Connection: close\r\n\r\n"
                << deny;
        }
        else
        {
            oss << "HTTP/1.1 " << statusCode << (statusCode == 200 ? " OK" : " Not Found") << "\r\n"
                << "Content-Type: application/json; charset=utf-8\r\n"
                << "Content-Length: " << body.size() << "\r\n"
                << "Connection: close\r\n\r\n"
                << body;
        }
        const std::string resp = oss.str();
        ::send(conn, resp.data(), static_cast<int>(resp.size()), 0);
        ::closesocket(conn);
    }

    ::closesocket(sock);
    socketHandle_ = 0;
#endif
}

void PanelService::Stop()
{
    if (!running_.load())
    {
        return;
    }

    running_.store(false);
#ifdef _WIN32
    if (socketHandle_ != 0)
    {
        ::closesocket(static_cast<SOCKET>(socketHandle_));
        socketHandle_ = 0;
    }
#endif
    if (worker_.joinable())
    {
        worker_.join();
    }
    std::wcout << L"[panel] 已停止\n";
}

bool PanelService::IsRunning() const
{
    return running_.load();
}
}  // namespace mi::server
