#pragma once

#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "server/auth_service.hpp"
#include "server/config.hpp"
#include "mi/shared/net/kcp_channel.hpp"
#include "mi/shared/proto/messages.hpp"
#include "mi/shared/crypto/whitebox_aes.hpp"
#include "mi/shared/crypto/tls_support.hpp"
#include "mi/shared/secure/obfuscated_value.hpp"

namespace mi::server
{
class MessageRouter
{
public:
    MessageRouter(AuthService& auth,
                  mi::shared::net::KcpChannel& channel,
                  std::vector<std::uint8_t> certBytes = {},
                  std::wstring certPassword = L"",
                  std::string certFingerprint = {},
                  bool allowSelfSigned = true);

    void HandleIncoming(const mi::shared::net::ReceivedDatagram& packet);
    std::uint32_t ActiveSessions() const;
    std::vector<std::pair<std::uint32_t, mi::shared::net::PeerEndpoint>> ListSessions() const;
    std::vector<mi::shared::proto::SessionInfo> GetSessionInfos() const;
    std::vector<mi::shared::proto::StatsSample> GetStatsHistory(std::uint32_t sessionId) const;
    void DeliverOffline(std::uint32_t sessionId);
    void Tick();

private:
    void HandleAuth(const std::vector<std::uint8_t>& buffer, const mi::shared::net::PeerEndpoint& sender);
    void HandleData(const std::vector<std::uint8_t>& buffer, const mi::shared::net::PeerEndpoint& sender);
    void HandleMediaChunk(const std::vector<std::uint8_t>& buffer, const mi::shared::net::PeerEndpoint& sender);
    void HandleMediaControl(const std::vector<std::uint8_t>& buffer, const mi::shared::net::PeerEndpoint& sender);
    void HandleSessionListRequest(const std::vector<std::uint8_t>& buffer, const mi::shared::net::PeerEndpoint& sender);
    void SendError(const mi::shared::net::PeerEndpoint& target,
                   std::uint8_t code,
                   const std::wstring& message,
                   std::uint32_t sessionIdHint = 0);
    void BroadcastSessionList();
    void SendSessionList(const mi::shared::net::PeerEndpoint& target, std::uint32_t sessionId, bool subscribed);
    bool IsSenderAuthorized(std::uint32_t sessionId, const mi::shared::net::PeerEndpoint& sender);
    void LoadState();
    void SaveState() const;
    void HandleTlsClientHello(const std::vector<std::uint8_t>& buffer,
                              const mi::shared::net::PeerEndpoint& sender,
                              std::uint32_t sessionIdHint);
    bool DecryptEnvelope(std::uint32_t sessionId,
                         const std::vector<std::uint8_t>& cipher,
                         std::uint8_t& innerType,
                         std::vector<std::uint8_t>& innerPayload);
    void SendSecure(std::uint32_t sessionId,
                    const mi::shared::net::PeerEndpoint& peer,
                    const std::vector<std::uint8_t>& plain);
    mi::shared::crypto::WhiteboxKeyInfo BuildTlsKey(const std::vector<std::uint8_t>& secret) const;

    AuthService& auth_;
    mi::shared::net::KcpChannel& channel_;
    mi::shared::secure::ObfuscatedUint32 nextSessionId_;
    std::unordered_map<std::uint32_t, mi::shared::net::PeerEndpoint> sessions_;
    std::unordered_set<std::uint32_t> sessionSubscribers_;
    std::unordered_map<std::uint32_t, std::uint32_t> unreadCounts_;
    std::unordered_map<std::uint32_t, mi::shared::proto::StatsReport> stats_;
    std::unordered_map<std::uint32_t, std::vector<mi::shared::proto::StatsSample>> statsHistory_;
    std::wstring statePath_;
    std::unordered_map<std::uint32_t, std::vector<mi::shared::proto::ChatMessage>> offlineChats_;
    std::vector<std::uint8_t> certBytes_;
    std::wstring certPassword_;
    std::string certFingerprint_;
    bool allowSelfSigned_;
    bool tlsReady_;
    std::unordered_map<std::uint32_t, mi::shared::crypto::WhiteboxKeyInfo> tlsKeys_;
};
}  // namespace mi::server
