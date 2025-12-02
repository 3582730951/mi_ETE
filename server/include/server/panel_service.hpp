#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace mi::server
{
using PanelResponder = std::function<std::string(const std::string& path)>;

class PanelService
{
public:
    PanelService();
    ~PanelService();

    bool Start(const std::wstring& host, uint16_t port, PanelResponder responder, std::wstring token = L"");
    void Stop();
    bool IsRunning() const;

private:
    void Serve(const std::wstring& host, uint16_t port);

    std::atomic<bool> running_;
    PanelResponder responder_;
    std::thread worker_;
    std::uintptr_t socketHandle_;
    std::wstring token_;
};
}  // namespace mi::server
