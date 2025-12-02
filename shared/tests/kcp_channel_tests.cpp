#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "mi/shared/net/kcp_channel.hpp"

int main()
{
    mi::shared::net::KcpSettings settings{};
    settings.intervalMs = 5;

    mi::shared::net::KcpChannel channelA;
    mi::shared::net::KcpChannel channelB;
    channelA.Configure(settings);
    channelB.Configure(settings);

    assert(channelA.Start(L"127.0.0.1", 0));
    assert(channelB.Start(L"127.0.0.1", 0));
    assert(channelA.IsRunning() && channelB.IsRunning());

    const uint16_t portA = channelA.BoundPort();
    const uint16_t portB = channelB.BoundPort();
    assert(portA != 0 && portB != 0);

    mi::shared::net::PeerEndpoint peerA{L"127.0.0.1", portA};
    mi::shared::net::PeerEndpoint peerB{L"127.0.0.1", portB};

    const std::vector<std::uint8_t> payload{'k', 'c', 'p', '_', 't', 'e', 's', 't'};
    assert(channelA.Send(peerB, payload, 1));

    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!received && std::chrono::steady_clock::now() < deadline)
    {
        channelA.Poll();
        channelB.Poll();

        mi::shared::net::ReceivedDatagram packet{};
        while (channelB.TryReceive(packet))
        {
            received = (packet.payload == payload);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    channelA.Stop();
    channelB.Stop();
    assert(received);
    return 0;
}
