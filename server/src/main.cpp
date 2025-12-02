#include <iostream>
#include <string>
#include <vector>

#include "server/config.hpp"
#include "server/server_app.hpp"

namespace
{
struct LaunchOptions
{
    std::wstring configPath = L"configs/server.yaml";
    bool runOnce = false;
    uint32_t ticks = 0;
};

LaunchOptions ParseArgs(int argc, wchar_t* argv[])
{
    LaunchOptions options{};
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--config" && i + 1 < argc)
        {
            options.configPath = argv[++i];
        }
        else if (arg == L"--once")
        {
            options.runOnce = true;
        }
        else if (arg == L"--ticks" && i + 1 < argc)
        {
            try
            {
                options.ticks = static_cast<uint32_t>(std::stoul(argv[++i]));
            }
            catch (...)
            {
                options.ticks = 0;
            }
        }
    }
    return options;
}
}  // namespace

int wmain(int argc, wchar_t* argv[])
{
    const LaunchOptions options = ParseArgs(argc, argv);
    const mi::server::ServerConfig config = mi::server::LoadServerConfig(options.configPath);
    mi::server::ServerApplication app(config);

    if (!app.Start())
    {
        return 1;
    }

    if (options.runOnce)
    {
        app.RunOnce();
        app.Stop();
        return 0;
    }

    if (options.ticks > 0)
    {
        for (uint32_t i = 0; i < options.ticks && app.IsRunning(); ++i)
        {
            app.RunOnce();
        }
        app.Stop();
        return 0;
    }

    std::wcout << L"[server] 进入运行循环，按 Ctrl+C 退出\n";
    app.Run();
    app.Stop();
    return 0;
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
