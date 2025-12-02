#include <cassert>
#include <string>
#include <vector>

#include "mi/shared/proto/messages.hpp"

int main()
{
    mi::shared::proto::SessionListRequest req{};
    req.sessionId = 42;
    req.subscribe = true;
    const auto reqBuf = mi::shared::proto::SerializeSessionListRequest(req);
    mi::shared::proto::SessionListRequest parsedReq{};
    assert(mi::shared::proto::ParseSessionListRequest(reqBuf, parsedReq));
    assert(parsedReq.sessionId == 42);
    assert(parsedReq.subscribe);

    mi::shared::proto::SessionListResponse resp{};
    resp.subscribed = true;
    resp.serverTimeSec = 123456u;
    resp.sessions = {
        {1001u, L"127.0.0.1:9000"},
        {1002u, L"10.0.0.1:9001"},
    };
    const auto respBuf = mi::shared::proto::SerializeSessionListResponse(resp);
    mi::shared::proto::SessionListResponse parsedResp{};
    assert(mi::shared::proto::ParseSessionListResponse(respBuf, parsedResp));
    assert(parsedResp.subscribed);
    assert(parsedResp.serverTimeSec == resp.serverTimeSec);
    assert(parsedResp.sessions.size() == resp.sessions.size());
    for (size_t i = 0; i < resp.sessions.size(); ++i)
    {
        assert(parsedResp.sessions[i].sessionId == resp.sessions[i].sessionId);
        assert(parsedResp.sessions[i].peer == resp.sessions[i].peer);
    }

    return 0;
}
