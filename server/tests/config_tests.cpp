#include <cstdio>
#include <fstream>
#include <string>

#include "server/config.hpp"

namespace
{
std::wstring WriteTempConfig()
{
    const std::wstring path = L"tmp_server_config.yaml";
    const std::string narrow(path.begin(), path.end());
    std::ofstream file(narrow);
    file << "listen_host: 127.0.0.1\n";
    file << "listen_port: 9001\n";
    file << "panel_host: 0.0.0.0\n";
    file << "panel_port: 9100\n";
    file << "kcp_crc_enable: true\n";
    file << "kcp_crc_drop_log: false\n";
    file << "kcp_crc_max_frame: 2048\n";
    file.close();
    return path;
}
}  // namespace

int main()
{
    const std::wstring path = WriteTempConfig();
    const mi::server::ServerConfig cfg = mi::server::LoadServerConfig(path);

    std::remove(std::string(path.begin(), path.end()).c_str());

    if (cfg.listenHost != L"127.0.0.1" || cfg.listenPort != 9001)
    {
        return 1;
    }
    if (cfg.panelHost != L"0.0.0.0" || cfg.panelPort != 9100)
    {
        return 2;
    }
    if (!cfg.kcpCrcEnable || cfg.kcpCrcDropLog)
    {
        return 3;
    }
    if (cfg.kcpMaxFrameSize != 2048)
    {
        return 4;
    }
    return 0;
}
