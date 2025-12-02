#include "server/config.hpp"

#include <algorithm>
#include <codecvt>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
#include <vector>

namespace
{
std::wstring Trim(const std::wstring& text)
{
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
    {
        return L"";
    }

    const auto last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool TryParseUint(const std::wstring& text, uint64_t& value)
{
    try
    {
        value = std::stoull(text);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void ApplyValue(mi::server::ServerConfig& config, const std::wstring& key, const std::wstring& value)
{
    if (key == L"listen_host")
    {
        config.listenHost = value;
        return;
    }

    if (key == L"listen_port")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.listenPort = static_cast<uint16_t>(parsed);
        }
        return;
    }

    if (key == L"panel_host")
    {
        config.panelHost = value;
        return;
    }

    if (key == L"panel_port")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.panelPort = static_cast<uint16_t>(parsed);
        }
        return;
    }

    if (key == L"panel_token")
    {
        config.panelToken = value;
        return;
    }

    if (key == L"cert_base64")
    {
        config.certBase64 = value;
        return;
    }

    if (key == L"cert_password")
    {
        config.certPassword = value;
        return;
    }

    if (key == L"cert_sha256")
    {
        config.certSha256 = value;
        return;
    }

    if (key == L"cert_allow_self_signed")
    {
        const auto lower = value == L"1" || value == L"true" || value == L"TRUE" || value == L"on";
        const auto upper = value == L"0" || value == L"false" || value == L"FALSE" || value == L"off";
        if (lower)
        {
            config.certAllowSelfSigned = true;
        }
        else if (upper)
        {
            config.certAllowSelfSigned = false;
        }
        return;
    }

    if (key == L"kcp_interval_ms")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpIntervalMs = static_cast<uint32_t>(parsed);
        }
        return;
    }

    if (key == L"kcp_mtu")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpMtu = static_cast<uint16_t>(parsed);
        }
        return;
    }

    if (key == L"kcp_send_window")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpSendWindow = static_cast<uint32_t>(parsed);
        }
        return;
    }

    if (key == L"kcp_recv_window")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpRecvWindow = static_cast<uint32_t>(parsed);
        }
        return;
    }

    if (key == L"kcp_idle_timeout_ms")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpIdleTimeoutMs = static_cast<uint32_t>(parsed);
        }
        return;
    }

    if (key == L"kcp_peer_rebind_ms")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpPeerRebindMs = static_cast<uint32_t>(parsed);
        }
        return;
    }

    if (key == L"kcp_crc_enable")
    {
        config.kcpCrcEnable = (value == L"1" || value == L"true" || value == L"on");
        return;
    }

    if (key == L"kcp_crc_drop_log")
    {
        config.kcpCrcDropLog = (value == L"1" || value == L"true" || value == L"on");
        return;
    }

    if (key == L"kcp_crc_max_frame")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpMaxFrameSize = static_cast<uint32_t>(parsed);
        }
        return;
    }

    if (key == L"poll_sleep_ms")
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.pollSleepMs = static_cast<uint32_t>(parsed);
        }
        return;
    }
}

std::vector<mi::server::UserCredential> ParseUsers(const std::wstring& value)
{
    std::vector<mi::server::UserCredential> users;
    size_t start = 0;
    while (start < value.size())
    {
        const size_t comma = value.find(L',', start);
        const size_t end = (comma == std::wstring::npos) ? value.size() : comma;
        const std::wstring pair = Trim(value.substr(start, end - start));
        const size_t colon = pair.find(L':');
        if (colon != std::wstring::npos)
        {
            mi::server::UserCredential cred{};
            cred.username = Trim(pair.substr(0, colon));
            cred.password = Trim(pair.substr(colon + 1));
            if (!cred.username.empty() && !cred.password.empty())
            {
                users.push_back(cred);
            }
        }

        if (comma == std::wstring::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return users;
}

bool TryGetEnv(const std::wstring& name, std::wstring& out)
{
#if defined(_WIN32) && defined(_MSC_VER)
    wchar_t* buffer = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&buffer, &len, name.c_str()) != 0 || buffer == nullptr)
    {
        return false;
    }
    out.assign(buffer);
    free(buffer);
    return true;
#elif defined(_WIN32)
    const wchar_t* value = _wgetenv(name.c_str());
    if (value == nullptr)
    {
        return false;
    }
    out.assign(value);
    return true;
#else
    (void)name;
    (void)out;
    return false;
#endif
}

void ApplyEnvOverrides(mi::server::ServerConfig& config)
{
    std::wstring value;
    if (TryGetEnv(L"MI_USERS", value))
    {
        const auto parsed = ParseUsers(value);
        if (!parsed.empty())
        {
            config.allowedUsers = parsed;
        }
    }

    if (TryGetEnv(L"MI_KCP_MTU", value))
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpMtu = static_cast<uint16_t>(parsed);
        }
    }

    if (TryGetEnv(L"MI_KCP_INTERVAL_MS", value))
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpIntervalMs = static_cast<uint32_t>(parsed);
        }
    }

    if (TryGetEnv(L"MI_KCP_SEND_WINDOW", value))
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpSendWindow = static_cast<uint32_t>(parsed);
        }
    }

    if (TryGetEnv(L"MI_KCP_RECV_WINDOW", value))
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpRecvWindow = static_cast<uint32_t>(parsed);
        }
    }

    if (TryGetEnv(L"MI_KCP_IDLE_TIMEOUT_MS", value))
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpIdleTimeoutMs = static_cast<uint32_t>(parsed);
        }
    }

    if (TryGetEnv(L"MI_KCP_PEER_REBIND_MS", value))
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpPeerRebindMs = static_cast<uint32_t>(parsed);
        }
    }

    if (TryGetEnv(L"MI_PANEL_TOKEN", value))
    {
        config.panelToken = value;
    }

    if (TryGetEnv(L"MI_KCP_CRC_ENABLE", value))
    {
        config.kcpCrcEnable = (value == L"1" || value == L"true" || value == L"on");
    }

    if (TryGetEnv(L"MI_KCP_CRC_DROP_LOG", value))
    {
        config.kcpCrcDropLog = (value == L"1" || value == L"true" || value == L"on");
    }

    if (TryGetEnv(L"MI_KCP_CRC_MAX_FRAME", value))
    {
        uint64_t parsed = 0;
        if (TryParseUint(value, parsed))
        {
            config.kcpMaxFrameSize = static_cast<uint32_t>(parsed);
        }
    }

    if (TryGetEnv(L"MI_CERT_ALLOW_SELF_SIGNED", value))
    {
        const auto lower = value == L"1" || value == L"true" || value == L"TRUE" || value == L"on" || value == L"ON";
        const auto upper = value == L"0" || value == L"false" || value == L"FALSE" || value == L"off" || value == L"OFF";
        if (lower)
        {
            config.certAllowSelfSigned = true;
        }
        else if (upper)
        {
            config.certAllowSelfSigned = false;
        }
    }
}
}  // namespace

namespace mi::server
{
ServerConfig LoadServerConfig(const std::wstring& path)
{
    ServerConfig config{};
    config.listenHost = L"0.0.0.0";
    config.listenPort = 7845;
    config.panelHost = L"127.0.0.1";
    config.panelPort = 9000;
    config.panelToken.clear();
    config.kcpIntervalMs = 10;
    config.kcpMtu = 1400;
    config.kcpSendWindow = 128;
    config.kcpRecvWindow = 128;
    config.kcpIdleTimeoutMs = 15000;
    config.kcpPeerRebindMs = 500;
    config.kcpCrcEnable = false;
    config.kcpCrcDropLog = false;
    config.kcpMaxFrameSize = 4096;
    config.pollSleepMs = 5;
    config.allowedUsers.clear();
    config.certAllowSelfSigned = true;

    const std::filesystem::path filePath(path);
    if (!std::filesystem::exists(filePath))
    {
        std::wcerr << L"[server] 配置文件不存在，使用默认值: " << filePath.wstring() << L"\n";
        return config;
    }

    std::wifstream file(filePath);
    file.imbue(std::locale(file.getloc(), new std::codecvt_utf8<wchar_t>()));
    if (!file.is_open())
    {
        std::wcerr << L"[server] 无法打开配置文件，使用默认值\n";
        return config;
    }

    std::wstring line;
    while (std::getline(file, line))
    {
        const auto commentPos = line.find(L'#');
        if (commentPos != std::wstring::npos)
        {
            line = line.substr(0, commentPos);
        }

        const std::wstring trimmed = Trim(line);
        if (trimmed.empty())
        {
            continue;
        }

        size_t delimiter = trimmed.find(L':');
        if (delimiter == std::wstring::npos)
        {
            delimiter = trimmed.find(L'=');
        }

        if (delimiter == std::wstring::npos)
        {
            continue;
        }

        const std::wstring key = Trim(trimmed.substr(0, delimiter));
        const std::wstring value = Trim(trimmed.substr(delimiter + 1));
        if (!key.empty() && !value.empty())
        {
            if (key == L"users")
            {
                const auto parsed = ParseUsers(value);
                if (!parsed.empty())
                {
                    config.allowedUsers = parsed;
                }
            }
            else
            {
                ApplyValue(config, key, value);
            }
        }
    }

    ApplyEnvOverrides(config);

    return config;
}
}  // namespace mi::server
