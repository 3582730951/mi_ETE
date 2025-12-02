#include <cassert>
#include <string>
#include <vector>

#include "mi/shared/proto/messages.hpp"

int main()
{
    mi::shared::proto::AuthRequest req{};
    req.username = L"user";
    req.password = L"pass";
    const auto reqBuf = mi::shared::proto::SerializeAuthRequest(req);
    mi::shared::proto::AuthRequest parsedReq{};
    assert(mi::shared::proto::ParseAuthRequest(reqBuf, parsedReq));
    assert(parsedReq.username == L"user");
    assert(parsedReq.password == L"pass");

    mi::shared::proto::AuthResponse resp{};
    resp.success = true;
    resp.sessionId = 1234;
    const auto respBuf = mi::shared::proto::SerializeAuthResponse(resp);
    mi::shared::proto::AuthResponse parsedResp{};
    assert(mi::shared::proto::ParseAuthResponse(respBuf, parsedResp));
    assert(parsedResp.success);
    assert(parsedResp.sessionId == 1234);

    mi::shared::proto::DataPacket pkt{};
    pkt.sessionId = 99;
    pkt.targetSessionId = 1001;
    pkt.payload = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    const auto pktBuf = mi::shared::proto::SerializeDataPacket(pkt);
    mi::shared::proto::DataPacket parsedPkt{};
    assert(mi::shared::proto::ParseDataPacket(pktBuf, parsedPkt));
    assert(parsedPkt.sessionId == 99);
    assert(parsedPkt.targetSessionId == 1001);
    assert(parsedPkt.payload == pkt.payload);

    mi::shared::proto::ErrorResponse error{};
    error.code = 0x02;
    error.severity = 1;
    error.retryAfterMs = 1234;
    error.message = L"session not found";
    const auto errBuf = mi::shared::proto::SerializeErrorResponse(error);
    mi::shared::proto::ErrorResponse parsedErr{};
    assert(mi::shared::proto::ParseErrorResponse(errBuf, parsedErr));
    assert(parsedErr.code == error.code);
    assert(parsedErr.severity == error.severity);
    assert(parsedErr.retryAfterMs == error.retryAfterMs);
    assert(parsedErr.message == error.message);

    mi::shared::proto::MediaChunk media{};
    media.sessionId = 10;
    media.targetSessionId = 20;
    media.mediaId = 123456789ULL;
    media.chunkIndex = 1;
    media.totalChunks = 3;
    media.totalSize = 999;
    media.name = L"photo.png";
    media.payload = {0xAA, 0xBB, 0xCC};
    const auto mediaBuf = mi::shared::proto::SerializeMediaChunk(media);
    mi::shared::proto::MediaChunk mediaParsed{};
    assert(mi::shared::proto::ParseMediaChunk(mediaBuf, mediaParsed));
    assert(mediaParsed.sessionId == media.sessionId);
    assert(mediaParsed.targetSessionId == media.targetSessionId);
    assert(mediaParsed.mediaId == media.mediaId);
    assert(mediaParsed.chunkIndex == media.chunkIndex);
    assert(mediaParsed.totalChunks == media.totalChunks);
    assert(mediaParsed.totalSize == media.totalSize);
    assert(mediaParsed.name == media.name);
    assert(mediaParsed.payload == media.payload);

    mi::shared::proto::MediaControl ctl{};
    ctl.sessionId = 5;
    ctl.targetSessionId = 6;
    ctl.mediaId = 777;
    ctl.action = 1;
    const auto ctlBuf = mi::shared::proto::SerializeMediaControl(ctl);
    mi::shared::proto::MediaControl ctlParsed{};
    assert(mi::shared::proto::ParseMediaControl(ctlBuf, ctlParsed));
    assert(ctlParsed.sessionId == ctl.sessionId);
    assert(ctlParsed.targetSessionId == ctl.targetSessionId);
    assert(ctlParsed.mediaId == ctl.mediaId);
    assert(ctlParsed.action == ctl.action);

    mi::shared::proto::ChatMessage chat{};
    chat.sessionId = 1;
    chat.targetSessionId = 2;
    chat.messageId = 999;
    chat.format = 1;
    chat.attachments = {L"fileA.txt", L"image.png"};
    chat.payload = {1, 2, 3};
    const auto chatBuf = mi::shared::proto::SerializeChatMessage(chat);
    mi::shared::proto::ChatMessage chatParsed{};
    assert(mi::shared::proto::ParseChatMessage(chatBuf, chatParsed));
    assert(chatParsed.sessionId == chat.sessionId);
    assert(chatParsed.targetSessionId == chat.targetSessionId);
    assert(chatParsed.messageId == chat.messageId);
    assert(chatParsed.format == chat.format);
    assert(chatParsed.attachments == chat.attachments);
    assert(chatParsed.payload == chat.payload);

    mi::shared::proto::StatsReport rpt{};
    rpt.sessionId = 1;
    rpt.bytesSent = 100;
    rpt.bytesReceived = 200;
    rpt.chatFailures = 1;
    rpt.dataFailures = 0;
    rpt.mediaFailures = 2;
    rpt.durationMs = 1500;
    const auto rptBuf = mi::shared::proto::SerializeStatsReport(rpt);
    mi::shared::proto::StatsReport rptParsed{};
    assert(mi::shared::proto::ParseStatsReport(rptBuf, rptParsed));
    assert(rptParsed.sessionId == rpt.sessionId);
    assert(rptParsed.bytesSent == rpt.bytesSent);
    assert(rptParsed.mediaFailures == rpt.mediaFailures);
    assert(rptParsed.durationMs == rpt.durationMs);

    mi::shared::proto::StatsHistoryRequest histReq{};
    histReq.sessionId = 42;
    const auto histReqBuf = mi::shared::proto::SerializeStatsHistoryRequest(histReq);
    mi::shared::proto::StatsHistoryRequest histReqParsed{};
    assert(mi::shared::proto::ParseStatsHistoryRequest(histReqBuf, histReqParsed));
    assert(histReqParsed.sessionId == histReq.sessionId);

    mi::shared::proto::StatsHistoryResponse histResp{};
    histResp.sessionId = 42;
    mi::shared::proto::StatsSample sample{};
    sample.sessionId = 42;
    sample.timestampSec = 123456;
    sample.stats = rpt;
    histResp.samples.push_back(sample);
    const auto histRespBuf = mi::shared::proto::SerializeStatsHistoryResponse(histResp);
    mi::shared::proto::StatsHistoryResponse histRespParsed{};
    assert(mi::shared::proto::ParseStatsHistoryResponse(histRespBuf, histRespParsed));
    assert(histRespParsed.sessionId == histResp.sessionId);
    assert(histRespParsed.samples.size() == 1);
    assert(histRespParsed.samples[0].timestampSec == sample.timestampSec);
    assert(histRespParsed.samples[0].stats.bytesSent == sample.stats.bytesSent);

    mi::shared::proto::ChatControl chatCtl{};
    chatCtl.sessionId = 3;
    chatCtl.targetSessionId = 4;
    chatCtl.messageId = 555;
    chatCtl.action = 1;
    const auto chatCtlBuf = mi::shared::proto::SerializeChatControl(chatCtl);
    mi::shared::proto::ChatControl chatCtlParsed{};
    assert(mi::shared::proto::ParseChatControl(chatCtlBuf, chatCtlParsed));
    assert(chatCtlParsed.sessionId == chatCtl.sessionId);
    assert(chatCtlParsed.targetSessionId == chatCtl.targetSessionId);
    assert(chatCtlParsed.messageId == chatCtl.messageId);
    assert(chatCtlParsed.action == chatCtl.action);

    return 0;
}
