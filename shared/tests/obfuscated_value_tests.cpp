#include <chrono>
#include <thread>
#include <vector>

#include "mi/shared/secure/obfuscated_value.hpp"
#include "mi/shared/net/kcp_channel.hpp"

int main()
{
    using mi::shared::secure::ObfuscatedInt32;
    using mi::shared::secure::ObfuscatedUint32;

    ObfuscatedInt32 a(42);
    ObfuscatedUint32 b(0);
    if (a.Value() != 42)
    {
        return 1;
    }

    b.Set(100);
    if (b.Value() != 100)
    {
        return 2;
    }
    const auto next = b.Increment();
    if (next != 101 || b.Value() != 101)
    {
        return 3;
    }

    // 嵌入 KcpChannel 场景验证：统计与超时回收。
    mi::shared::net::KcpChannel channel;
    mi::shared::net::KcpSettings settings{};
    settings.idleTimeoutMs = 10;
    channel.Configure(settings);
    if (!channel.Start(L"127.0.0.1", 0))
    {
        return 4;
    }

    const mi::shared::net::PeerEndpoint dummyPeer{L"127.0.0.1", 9000};
    mi::shared::net::Session session{};
    session.id = 123;
    session.peer = dummyPeer;
    channel.RegisterSession(session);

    auto stats = channel.CollectStats();
    if (stats.sessionCount != 1)
    {
        channel.Stop();
        return 5;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    channel.Poll();  // 触发超时回收
    stats = channel.CollectStats();
    channel.Stop();
    if (stats.sessionCount != 0 || stats.idleReclaimed == 0)
    {
        return 6;
    }

    return 0;
}
