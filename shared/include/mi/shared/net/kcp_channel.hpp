#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

struct IKCPCB;
typedef struct IKCPCB ikcpcb;

namespace mi::shared::net
{

struct KcpSettings
{
    uint16_t mtu = 1400;
    uint32_t intervalMs = 10;
    uint32_t sendWindow = 128;
    uint32_t receiveWindow = 128;
    bool noDelay = true;
    uint32_t idleTimeoutMs = 15000;      // 会话超时时间，0 表示不回收
    uint32_t peerRebindCooldownMs = 500; // 允许重绑到新端点的最小时间间隔
    bool enableCrc32 = false;            // 出站/入站增加 CRC32 校验
    bool crcDropLog = false;             // 是否打印 CRC 失败日志
    std::uint32_t maxFrameSize = 4096;   // CRC 包裹后最大帧长，超过则丢弃
};

struct PeerEndpoint
{
    std::wstring host;
    uint16_t port = 0;
};

struct Session
{
    std::uint32_t id = 0;
    PeerEndpoint peer;
};

struct SessionState
{
    PeerEndpoint peer;
    ikcpcb* kcp = nullptr;
    std::uint32_t lastActiveMs = 0;
    std::uint32_t lastSendMs = 0;
    std::uint32_t crcOk = 0;
    std::uint32_t crcFail = 0;
};

struct ReceivedDatagram
{
    std::vector<std::uint8_t> payload;
    PeerEndpoint sender;
    std::uint32_t sessionId = 0;  // KCP conv，便于 TLS/会话校验
};

struct KcpChannelStats
{
    std::uint32_t sessionCount = 0;
    std::uint32_t crcOk = 0;
    std::uint32_t crcFail = 0;
    std::uint32_t idleReclaimed = 0;
};

class KcpChannel
{
public:
    KcpChannel();
    ~KcpChannel();

    void Configure(const KcpSettings& settings);
    bool Start(const std::wstring& host, uint16_t port);
    bool Send(const PeerEndpoint& peer, const std::vector<std::uint8_t>& payload, std::uint32_t sessionId = 0);
    void Poll();
    bool TryReceive(ReceivedDatagram& packet);
    void Stop();
    bool IsRunning() const;
    KcpSettings Settings() const;
    std::vector<std::uint8_t> LastReceived() const;  // 返回最近消费的数据副本，兼容旧接口
    PeerEndpoint LastSender() const;
    void RegisterSession(const Session& session);
    PeerEndpoint FindPeer(std::uint32_t sessionId) const;
    std::uint32_t FindSessionId(const PeerEndpoint& peer) const;
    uint16_t BoundPort() const;
    KcpChannelStats CollectStats() const;
    std::vector<std::uint32_t> ActiveSessionIds() const;

private:
    struct PendingPacket
    {
        std::vector<std::uint8_t> frame;
        PeerEndpoint peer;
        std::uint32_t sequence = 0;
        std::uint8_t attempts = 0;
        std::chrono::steady_clock::time_point lastSend;
        std::uint32_t sessionId = 0;
    };

    struct FrameHeader
    {
        std::uint8_t magic = 0x5Au;
        std::uint8_t flags = 0;
        std::uint16_t length = 0;
        std::uint32_t sessionId = 0;
        std::uint32_t sequence = 0;
        std::uint32_t ack = 0;
        std::uint32_t crc = 0;
    };

    void ProcessIncoming();
    void HandleDatagram(const std::vector<std::uint8_t>& buffer, const PeerEndpoint& sender);
    void UpdateSessions();
    SessionState& EnsureSession(std::uint32_t sessionId, const PeerEndpoint& peer);
    bool SendRaw(const PeerEndpoint& peer, const std::vector<std::uint8_t>& frame);
    std::wstring BuildPeerKey(const PeerEndpoint& peer) const;
    void Reset();
    static int KcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    static std::uint32_t NowMs();
    void DisposeSessions();
    void CleanupStaleSessions(std::uint32_t now);
    void UpdatePeer(std::uint32_t sessionId, SessionState& state, const PeerEndpoint& peer, std::uint32_t now);

    KcpSettings settings_;
    bool running_;
    std::uintptr_t socketHandle_;
    bool winsockReady_;
    uint16_t boundPort_;
    std::deque<ReceivedDatagram> received_;
    std::vector<std::uint8_t> lastReceived_;
    PeerEndpoint lastSender_;
    std::unordered_map<std::uint32_t, SessionState> sessions_;
    std::unordered_map<std::wstring, std::uint32_t> peerToSession_;
    std::uint32_t reclaimedCount_;
};
}  // namespace mi::shared::net
