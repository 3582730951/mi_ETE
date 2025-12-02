#include <codecvt>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "client/client_runner.hpp"
#include "mi/shared/crypto/whitebox_aes.hpp"

namespace
{
std::wstring Trim(const std::wstring& text)
{
    const auto isSpace = [](wchar_t c) { return std::iswspace(c) != 0; };
    std::size_t start = 0;
    while (start < text.size() && isSpace(text[start]))
    {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && isSpace(text[end - 1]))
    {
        --end;
    }
    return text.substr(start, end - start);
}

bool ParseHostPort(const std::wstring& value, std::wstring& hostOut, uint16_t& portOut)
{
    const auto pos = value.find(L':');
    if (pos == std::wstring::npos)
    {
        return false;
    }
    hostOut = value.substr(0, pos);
    try
    {
        portOut = static_cast<uint16_t>(std::stoul(value.substr(pos + 1)));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ParseBool(const std::wstring& value)
{
    std::wstring tmp = Trim(value);
    for (wchar_t& c : tmp)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return (tmp == L"1" || tmp == L"true" || tmp == L"yes" || tmp == L"on");
}

mi::client::SendMode ParseMode(const std::wstring& value)
{
    std::wstring tmp = Trim(value);
    for (wchar_t& c : tmp)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    if (tmp == L"data")
    {
        return mi::client::SendMode::Data;
    }
    if (tmp == L"both")
    {
        return mi::client::SendMode::Both;
    }
    return mi::client::SendMode::Chat;
}

const wchar_t* ModeName(mi::client::SendMode mode)
{
    switch (mode)
    {
    case mi::client::SendMode::Data:
        return L"data";
    case mi::client::SendMode::Both:
        return L"both";
    default:
        return L"chat";
    }
}

bool TryGetEnv(const wchar_t* name, std::wstring& out)
{
#if defined(_WIN32) && defined(_MSC_VER)
    wchar_t* buffer = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&buffer, &len, name) != 0 || buffer == nullptr)
    {
        return false;
    }
    out.assign(buffer);
    free(buffer);
    return true;
#elif defined(_WIN32)
    const wchar_t* value = _wgetenv(name);
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

std::wstring DetectConfigPath(int argc, wchar_t* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::wstring(argv[i]) == L"--config" && i + 1 < argc)
        {
            return argv[i + 1];
        }
    }
    const std::filesystem::path defaultPath = std::filesystem::path(L"configs") / L"client.yaml";
    if (std::filesystem::exists(defaultPath))
    {
        return defaultPath.wstring();
    }
    return L"";
}

void ApplyEnv(mi::client::ClientOptions& opts)
{
    std::wstring value;
    if (TryGetEnv(L"MI_USER", value))
    {
        opts.username = value;
    }
    if (TryGetEnv(L"MI_PASS", value))
    {
        opts.password = value;
    }
    if (TryGetEnv(L"MI_MESSAGE", value))
    {
        opts.message = value;
    }
    if (TryGetEnv(L"MI_SERVER", value))
    {
        ParseHostPort(value, opts.serverHost, opts.serverPort);
    }
    if (TryGetEnv(L"MI_TARGET", value))
    {
        try
        {
            opts.targetSessionId = static_cast<std::uint32_t>(std::stoul(value));
        }
        catch (...)
        {
            opts.targetSessionId = 0;
        }
    }
    if (TryGetEnv(L"MI_MEDIA_PATH", value))
    {
        opts.mediaPath = value;
    }
    if (TryGetEnv(L"MI_MEDIA_CHUNK", value))
    {
        try
        {
            opts.mediaChunkSize = static_cast<std::uint32_t>(std::stoul(value));
        }
        catch (...)
        {
            opts.mediaChunkSize = 1200;
        }
    }
    if (TryGetEnv(L"MI_REVOKE_AFTER", value))
    {
        opts.revokeAfterReceive = ParseBool(value);
    }
    if (TryGetEnv(L"MI_RETRIES", value))
    {
        try
        {
            opts.retryCount = static_cast<std::uint32_t>(std::stoul(value));
        }
        catch (...)
        {
            opts.retryCount = 1;
        }
    }
    if (TryGetEnv(L"MI_RETRY_DELAY_MS", value))
    {
        try
        {
            opts.retryDelayMs = static_cast<std::uint32_t>(std::stoul(value));
        }
        catch (...)
        {
            opts.retryDelayMs = 500;
        }
    }
    if (TryGetEnv(L"MI_MODE", value))
    {
        opts.sendMode = ParseMode(value);
    }
}

bool LoadConfigFromFile(const std::wstring& path, mi::client::ClientOptions& opts)
{
    if (path.empty() || !std::filesystem::exists(path))
    {
        return false;
    }

    const std::filesystem::path fsPath(path);
    std::wifstream stream(fsPath);
    stream.imbue(std::locale(stream.getloc(), new std::codecvt_utf8<wchar_t>));
    if (!stream.is_open())
    {
        return false;
    }

    std::wstring line;
    while (std::getline(stream, line))
    {
        const auto commentPos = line.find(L'#');
        const std::wstring content = Trim(line.substr(0, commentPos));
        if (content.empty())
        {
            continue;
        }
        const auto sep = content.find_first_of(L":=");
        if (sep == std::wstring::npos)
        {
            continue;
        }
        const std::wstring key = Trim(content.substr(0, sep));
        const std::wstring value = Trim(content.substr(sep + 1));

        if (key == L"server")
        {
            ParseHostPort(value, opts.serverHost, opts.serverPort);
        }
        else if (key == L"user")
        {
            opts.username = value;
        }
        else if (key == L"password")
        {
            opts.password = value;
        }
        else if (key == L"message")
        {
            opts.message = value;
        }
        else if (key == L"target")
        {
            try
            {
                opts.targetSessionId = static_cast<std::uint32_t>(std::stoul(value));
            }
            catch (...)
            {
                opts.targetSessionId = 0;
            }
        }
        else if (key == L"timeout_ms")
        {
            try
            {
                opts.timeoutMs = static_cast<std::uint32_t>(std::stoul(value));
            }
            catch (...)
            {
                opts.timeoutMs = 2000;
            }
        }
        else if (key == L"media_path")
        {
            opts.mediaPath = value;
        }
        else if (key == L"media_chunk")
        {
            try
            {
                opts.mediaChunkSize = static_cast<std::uint32_t>(std::stoul(value));
            }
            catch (...)
            {
                opts.mediaChunkSize = 1200;
            }
        }
        else if (key == L"revoke_after")
        {
            opts.revokeAfterReceive = ParseBool(value);
        }
        else if (key == L"retries")
        {
            try
            {
                opts.retryCount = static_cast<std::uint32_t>(std::stoul(value));
            }
            catch (...)
            {
                opts.retryCount = 1;
            }
        }
        else if (key == L"retry_delay_ms")
        {
            try
            {
                opts.retryDelayMs = static_cast<std::uint32_t>(std::stoul(value));
            }
            catch (...)
            {
                opts.retryDelayMs = 500;
            }
        }
        else if (key == L"mode")
        {
            opts.sendMode = ParseMode(value);
        }
    }
    return true;
}

mi::client::ClientOptions ParseArgs(int argc, wchar_t* argv[], mi::client::ClientOptions opts)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--server" && i + 1 < argc)
        {
            const std::wstring value = argv[++i];
            ParseHostPort(value, opts.serverHost, opts.serverPort);
        }
        else if (arg == L"--user" && i + 1 < argc)
        {
            opts.username = argv[++i];
        }
        else if (arg == L"--password" && i + 1 < argc)
        {
            opts.password = argv[++i];
        }
        else if (arg == L"--message" && i + 1 < argc)
        {
            opts.message = argv[++i];
        }
        else if (arg == L"--target" && i + 1 < argc)
        {
            try
            {
                opts.targetSessionId = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            }
            catch (...)
            {
                opts.targetSessionId = 0;
            }
        }
        else if (arg == L"--timeout-ms" && i + 1 < argc)
        {
            try
            {
                opts.timeoutMs = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            }
            catch (...)
            {
                opts.timeoutMs = 2000;
            }
        }
        else if (arg == L"--media-path" && i + 1 < argc)
        {
            opts.mediaPath = argv[++i];
        }
        else if (arg == L"--media-chunk" && i + 1 < argc)
        {
            try
            {
                opts.mediaChunkSize = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            }
            catch (...)
            {
                opts.mediaChunkSize = 1200;
            }
        }
        else if (arg == L"--revoke-after")
        {
            opts.revokeAfterReceive = true;
        }
        else if (arg == L"--config" && i + 1 < argc)
        {
            // handled outside
            ++i;
        }
        else if (arg == L"--retries" && i + 1 < argc)
        {
            try
            {
                opts.retryCount = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            }
            catch (...)
            {
                opts.retryCount = 1;
            }
        }
        else if (arg == L"--retry-delay-ms" && i + 1 < argc)
        {
            try
            {
                opts.retryDelayMs = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            }
            catch (...)
            {
                opts.retryDelayMs = 500;
            }
        }
        else if (arg == L"--mode" && i + 1 < argc)
        {
            opts.sendMode = ParseMode(argv[++i]);
        }
    }
    return opts;
}
}  // namespace

int RunCli(int argc, wchar_t* argv[])
{
    mi::client::ClientOptions options{};
    const std::wstring configPath = DetectConfigPath(argc, argv);
    if (!configPath.empty())
    {
        options.configPath = configPath;
        if (LoadConfigFromFile(configPath, options))
        {
            std::wcout << L"[client] 已加载配置 " << configPath << L"\n";
        }
    }

    ApplyEnv(options);
    options = ParseArgs(argc, argv, options);

    mi::shared::crypto::WhiteboxKeyInfo keyInfo = mi::shared::crypto::BuildKeyFromEnv();
    std::atomic<bool> cancelled{false};
    bool success = false;
    const std::uint32_t attempts = options.retryCount + 1;
    for (std::uint32_t i = 0; i < attempts && !success; ++i)
    {
        if (i > 0)
        {
            std::wcout << L"[client] 重试第 " << (i + 1) << L" 次，等待 " << options.retryDelayMs << L"ms\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));
        }
        std::wcout << L"[client] 开始尝试 " << (i + 1) << L"/" << attempts << L" 目标 " << options.serverHost << L":"
                   << options.serverPort << L" 模式=" << ModeName(options.sendMode) << L"\n";
        mi::client::ClientCallbacks cbs{};
        cbs.onLog = [](const std::wstring& msg) { std::wcout << msg << L"\n"; };
        cbs.onFinished = [&](bool ok) { success = ok; };
        cbs.isCancelled = [&]() { return cancelled.load(); };
        success = mi::client::RunClient(options, keyInfo, cbs);
    }
    return success ? 0 : 1;
}

int wmain(int argc, wchar_t* argv[])
{
    bool forceCli = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--cli" || arg == L"--no-ui")
        {
            forceCli = true;
            break;
        }
    }

#ifdef _WIN32
    if (!forceCli)
    {
        HMODULE lib = ::LoadLibraryW(L"mi_client_qt_ui.dll");
        if (lib != nullptr)
        {
            using LaunchFn = int (*)(int, char**);
            LaunchFn launch = reinterpret_cast<LaunchFn>(::GetProcAddress(lib, "LaunchMiClientQt"));
            if (launch != nullptr)
            {
                std::vector<std::string> utf8Args;
                std::vector<char*> ptrs;
                utf8Args.reserve(static_cast<std::size_t>(argc));
                ptrs.reserve(static_cast<std::size_t>(argc));
                for (int i = 0; i < argc; ++i)
                {
                    const std::wstring warg = argv[i];
                    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
                    utf8Args.push_back(conv.to_bytes(warg));
                }
                for (auto& a : utf8Args)
                {
                    ptrs.push_back(a.data());
                }
                return launch(static_cast<int>(ptrs.size()), ptrs.data());
            }
        }
        std::wcout << L"[client] 未找到 mi_client_qt_ui.dll，回退到 CLI 模式\n";
    }
#endif
    return RunCli(argc, argv);
}

#if defined(_WIN32) && !defined(_MSC_VER)
int main(int argc, char* argv[])
{
    std::vector<std::wstring> converted;
    converted.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        std::wstring wideArg;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(argv[i]); *p != 0; ++p)
        {
            wideArg.push_back(static_cast<wchar_t>(*p));
        }
        converted.push_back(std::move(wideArg));
    }
    std::vector<wchar_t*> ptrs;
    ptrs.reserve(converted.size());
    for (auto& arg : converted)
    {
        ptrs.push_back(arg.data());
    }
    return wmain(static_cast<int>(ptrs.size()), ptrs.data());
}
#endif
