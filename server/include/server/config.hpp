#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mi::server
{
struct UserCredential
{
    std::wstring username;
    std::wstring password;
};

struct ServerConfig
{
    std::wstring listenHost;
    uint16_t listenPort;
    std::wstring panelHost;
    uint16_t panelPort;
    std::wstring panelToken;
    uint32_t kcpIntervalMs;
    uint16_t kcpMtu;
    uint32_t kcpSendWindow;
    uint32_t kcpRecvWindow;
    uint32_t kcpIdleTimeoutMs;
    uint32_t kcpPeerRebindMs;
    bool kcpCrcEnable;
    bool kcpCrcDropLog;
    uint32_t kcpMaxFrameSize;
    uint32_t pollSleepMs;
    std::wstring certBase64;   // 服务端证书（可选）Base64，未配置则使用默认/自签
    std::wstring certPassword; // 可选密码
    std::wstring certSha256;   // 可选指纹校验（hex）
    bool certAllowSelfSigned = true;  // 是否允许自签证书
    std::vector<UserCredential> allowedUsers;
};

ServerConfig LoadServerConfig(const std::wstring& path);
}  // namespace mi::server
