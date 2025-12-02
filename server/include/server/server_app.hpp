#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "server/config.hpp"
#include "mi/shared/net/kcp_channel.hpp"
#include "server/auth_service.hpp"
#include "server/panel_service.hpp"
#include "server/message_router.hpp"

namespace mi::server
{
class ServerApplication
{
public:
    explicit ServerApplication(ServerConfig config);

    bool Start();
    void Run();
    void RunOnce();
    void Stop();
    bool IsRunning() const;

private:
    void InitializeChannel();
    void RefreshPanelCache();
    std::string GetPanelCache();
    std::string HandlePanelPath(const std::string& path);
    std::string GetCertBase64() const;
    std::string GetCertPassword() const;

    ServerConfig config_;
    mi::shared::net::KcpChannel channel_;
    std::atomic<bool> running_;
    std::chrono::steady_clock::time_point startTime_;
    AuthService auth_;
    PanelService panel_;
    std::unique_ptr<MessageRouter> router_;
    std::chrono::steady_clock::time_point lastPanelRefresh_;
    std::string panelCache_;
    mutable std::mutex panelMutex_;
};
}  // namespace mi::server
