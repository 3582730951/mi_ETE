#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mi/shared/net/kcp_channel.hpp"

namespace mi::shared::net
{
// TCP 流量封装到 KCP 的简单隧道实现，类型取值不会与现有协议冲突（0x30+）。
enum class TunnelFrameType : std::uint8_t
{
    Open = 0x30,
    Data = 0x31,
    Close = 0x32
};

struct TcpTunnelClientConfig
{
    std::wstring listenHost = L"127.0.0.1";
    uint16_t listenPort = 0;  // 0 表示随机端口
    PeerEndpoint remotePeer{};
    PeerEndpoint routerPeer{};         // 当走中转服务器时使用
    std::uint32_t sessionId = 1;
    std::uint32_t targetSessionId = 0;  // 经路由转发的目标 session
    std::size_t maxFramePayload = 1024;
    bool viaRouter = false;             // true 时封装 DataPacket 发送到 routerPeer
};

struct TcpTunnelServerConfig
{
    std::wstring targetHost = L"127.0.0.1";
    uint16_t targetPort = 0;
    PeerEndpoint remotePeer{};
    PeerEndpoint routerPeer{};
    std::uint32_t sessionId = 1;
    std::uint32_t targetSessionId = 0;
    std::size_t maxFramePayload = 1024;
    bool viaRouter = false;
};

class TcpTunnelClient
{
public:
    TcpTunnelClient(KcpChannel& channel, TcpTunnelClientConfig config);
    ~TcpTunnelClient();

    bool Start();
    void Poll();
    void Stop();
    uint16_t ListenPort() const { return listenPort_; }
    bool IsRunning() const { return running_; }

private:
    struct Frame
    {
        TunnelFrameType type{TunnelFrameType::Data};
        std::uint32_t connId = 0;
        std::vector<std::uint8_t> payload;
    };

    bool CreateListener();
    void AcceptNew();
    void FlushTcpToKcp();
    void FlushKcpToTcp();
    bool SendFrame(const Frame& frame);
    bool ParseFrame(const std::vector<std::uint8_t>& buf, Frame& frame) const;
    void CloseConnection(std::uint32_t connId);

    KcpChannel& channel_;
    TcpTunnelClientConfig config_;
    bool running_;
    std::uintptr_t listenSock_;
    uint16_t listenPort_;
    std::uint32_t nextConnId_;
    std::unordered_map<std::uint32_t, std::uintptr_t> connections_;
};

class TcpTunnelServer
{
public:
    TcpTunnelServer(KcpChannel& channel, TcpTunnelServerConfig config);
    ~TcpTunnelServer();

    bool Start();
    void Poll();
    void Stop();
    bool IsRunning() const { return running_; }

private:
    struct Frame
    {
        TunnelFrameType type{TunnelFrameType::Data};
        std::uint32_t connId = 0;
        std::vector<std::uint8_t> payload;
    };

    bool ParseFrame(const std::vector<std::uint8_t>& buf, Frame& frame) const;
    bool SendFrame(const Frame& frame);
    void FlushKcpToTcp();
    void FlushTcpToKcp();
    void CloseConnection(std::uint32_t connId);

    KcpChannel& channel_;
    TcpTunnelServerConfig config_;
    bool running_;
    std::unordered_map<std::uint32_t, std::uintptr_t> connections_;
};
}  // namespace mi::shared::net
