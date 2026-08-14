#include "../inc/webRTCDebug.h"

#include <algorithm>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "commonDef.h"
#include "debugCommandServer.h"
#include "logger.h"

namespace rkmedia {
namespace webrtc {

static std::mutex g_web_rtc_debug_mutex; /* 保护调试命令注册状态和 server 列表。 */
static std::vector<WebRtcServer *> g_web_rtc_debug_servers; /* 已注册到 getWebRTC 的运行中 server。 */
static bool g_web_rtc_status_command_registered = false; /* getWebRTC shell 命令是否已注册。 */
static bool g_web_rtc_pli_test_command_registered = false; /* testWebRTCPLI shell 命令是否已注册。 */

/* 将 bool 值转换成调试命令里更直观的 yes/no 文本。 */
static const char *debug_bool_text(bool value)
{
    return value ? "yes" : "no";
}

/* 将最近事件距当前的间隔格式化为毫秒文本；未发生时显示 N/A。 */
static void debug_format_event_age(char *text, size_t textSize, bool valid, uint64_t valueMs)
{
    if (!text || textSize == 0) {
        return;
    }
    if (!valid) {
        snprintf(text, textSize, "%s", "N/A");
        return;
    }
    snprintf(text, textSize, "%llums", static_cast<unsigned long long>(valueMs));
}

/* 将同一条关键帧恢复链路的阶段耗时格式化；尚未形成完整链路时显示 N/A。 */
static void debug_format_keyframe_delay(char *text, size_t textSize, bool valid, uint64_t valueMs)
{
    if (!text || textSize == 0) {
        return;
    }
    if (!valid) {
        snprintf(text, textSize, "%s", "N/A");
        return;
    }
    snprintf(text, textSize, "%llums", static_cast<unsigned long long>(valueMs));
}

/* 将 WebRTC 音频编码类型转换成调试命令输出文本。 */
static const char *debug_audio_codec_text(WebRtcAudioCodec codec)
{
    switch (codec) {
    case WEBRTC_AUDIO_CODEC_NONE:
        return "none";
    case WEBRTC_AUDIO_CODEC_PCMA:
        return "PCMA";
    case WEBRTC_AUDIO_CODEC_PCMU:
        return "PCMU";
    case WEBRTC_AUDIO_CODEC_OPUS:
        return "OPUS";
    default:
        return "unknown";
    }
}

/*
 * 打印 WebRTC server 总览。

 */
static void debug_print_server_overview(char *reply,
                                        size_t *offset,
                                        size_t index,
                                        const WebRtcServerStats &stats)
{
    debug_command_reply_append(reply,
                               offset,
                               "server[%zu]\n"
                               "  name        : %s\n"
                               "  listen      : %s:%u\n"
                               "  running     : %s\n"
                               "  sessions    : %zu\n"
                               "  video_fps   : %u\n"
                               "  audio       : codec=%s rate=%u channels=%u\n",
                               index,
                               stats.name.c_str(),
                               stats.bindAddress.c_str(),
                               stats.port,
                               debug_bool_text(stats.running),
                               stats.sessions.size(),
                               stats.videoFps,
                               debug_audio_codec_text(stats.audioCodec),
                               stats.audioSampleRate,
                               stats.audioChannels);
}

/* 打印 WebRTC server 收到媒体帧和广播到 session 的统计。 */
static void debug_print_media_summary(char *reply,
                                      size_t *offset,
                                      const WebRtcServerStats &stats)
{
    char pliAge[32] = {0};
    char idrRequestAge[32] = {0};
    char idrInputAge[32] = {0};
    char pliToRequest[32] = {0};
    char requestToInput[32] = {0};

    debug_format_event_age(pliAge,
                           sizeof(pliAge),
                           stats.hasLastVideoPliTime,
                           stats.lastVideoPliAgeMs);
    debug_format_event_age(idrRequestAge,
                           sizeof(idrRequestAge),
                           stats.hasLastVideoIdrRequestTime,
                           stats.lastVideoIdrRequestAgeMs);
    debug_format_event_age(idrInputAge,
                           sizeof(idrInputAge),
                           stats.hasLastVideoIdrInputTime,
                           stats.lastVideoIdrInputAgeMs);
    debug_format_keyframe_delay(pliToRequest,
                                sizeof(pliToRequest),
                                stats.hasLastPliToIdrRequestDelay,
                                stats.lastPliToIdrRequestMs);
    debug_format_keyframe_delay(requestToInput,
                                sizeof(requestToInput),
                                stats.hasLastIdrRequestToInputDelay,
                                stats.lastIdrRequestToInputMs);

    debug_command_reply_append(reply,
                               offset,
                               "\n  media summary\n"
                               "    video input     : frames=%llu bytes=%llu\n"
                               "    video broadcast : targets=%llu no_ready=%llu\n"
                               "    video keyframe : pending=%s pli_pending=%s requests=%llu\n"
                               "    video pli      : rx=%llu accepted=%llu deferred=%llu\n"
                               "    video timing   : last_pli=%s last_idr_request=%s last_idr_input=%s\n"
                               "                     pli_to_request=%s request_to_idr_input=%s\n"
                               "    audio input     : frames=%llu bytes=%llu\n"
                               "    audio broadcast : targets=%llu no_ready=%llu\n",
                               static_cast<unsigned long long>(stats.inputVideoFrames),
                               static_cast<unsigned long long>(stats.inputVideoBytes),
                               static_cast<unsigned long long>(stats.videoBroadcastTargets),
                               static_cast<unsigned long long>(stats.videoNoReadySession),
                               debug_bool_text(stats.pendingVideoKeyframeRequest),
                               debug_bool_text(stats.pendingPliKeyframeRequest),
                               static_cast<unsigned long long>(stats.videoKeyframeRequests),
                               static_cast<unsigned long long>(stats.videoPliReceived),
                               static_cast<unsigned long long>(stats.videoPliAccepted),
                               static_cast<unsigned long long>(stats.videoPliDeferred),
                               pliAge,
                               idrRequestAge,
                               idrInputAge,
                               pliToRequest,
                               requestToInput,
                               static_cast<unsigned long long>(stats.inputAudioFrames),
                               static_cast<unsigned long long>(stats.inputAudioBytes),
                               static_cast<unsigned long long>(stats.audioBroadcastTargets),
                               static_cast<unsigned long long>(stats.audioNoReadySession));
}

/* 打印每个浏览器 session 的连接状态表格。 */
static void debug_print_session_state_table(char *reply,
                                            size_t *offset,
                                            const std::vector<WebRtcSessionStats> &sessions)
{
    size_t i;

    if (sessions.empty()) {
        debug_command_reply_append(reply, offset, "\n  sessions: none\n");
        return;
    }

    debug_command_reply_append(reply,
                               offset,
                               "\n  session states\n"
                               "    %-4s %-22s %-12s %-12s %-12s %-3s %-3s %-3s %-3s %-4s %-20s\n"
                               "    %-4s %-22s %-12s %-12s %-12s %-3s %-3s %-3s %-3s %-4s %-20s\n",
                               "id",
                               "remote",
                               "pc",
                               "ice",
                               "gathering",
                               "ws",
                               "dc",
                               "vid",
                               "aud",
                               "vkey",
                               "path",
                               "----",
                               "----------------------",
                               "------------",
                               "------------",
                               "------------",
                               "---",
                               "---",
                               "---",
                               "---",
                               "----",
                               "--------------------");

    for (i = 0; i < sessions.size(); ++i) {
        debug_command_reply_append(reply,
                                   offset,
                                   "    %-4d %-22.22s %-12.12s %-12.12s %-12.12s %-3s %-3s %-3s %-3s %-4s %-20.20s\n",
                                   sessions[i].id,
                                   sessions[i].remoteAddress.c_str(),
                                   sessions[i].peerState.c_str(),
                                   sessions[i].iceState.c_str(),
                                   sessions[i].gatheringState.c_str(),
                                   debug_bool_text(sessions[i].websocketOpen),
                                   debug_bool_text(sessions[i].dataChannelOpen),
                                   debug_bool_text(sessions[i].videoTrackReady),
                                   debug_bool_text(sessions[i].audioTrackReady),
                                   sessions[i].waitingForVideoKeyframe ? "wait" : "send",
                                   sessions[i].path.c_str());
    }
}

/* 打印每个浏览器 session 的收发计数表格。 */
static void debug_print_session_counter_table(char *reply,
                                              size_t *offset,
                                              const std::vector<WebRtcSessionStats> &sessions)
{
    size_t i;

    if (sessions.empty()) {
        return;
    }

    debug_command_reply_append(reply,
                               offset,
                               "\n  session counters\n"
                               "    %-4s %12s %12s %10s %10s %10s %8s %12s %12s %10s %10s %8s %8s %8s %8s %8s %8s\n"
                               "    %-4s %12s %12s %10s %10s %10s %8s %12s %12s %10s %10s %8s %8s %8s %8s %8s %8s\n",
                               "id",
                               "v_frames",
                               "v_bytes",
                               "v_drop",
                               "v_wait",
                               "v_fail",
                               "pli_rx",
                               "a_frames",
                               "a_bytes",
                               "a_drop",
                               "a_fail",
                               "sig_rx",
                               "sig_tx",
                               "cand_l",
                               "cand_r",
                               "dc_rx",
                               "dc_tx",
                               "----",
                               "------------",
                               "------------",
                               "----------",
                               "----------",
                               "----------",
                               "--------",
                               "------------",
                               "------------",
                               "----------",
                               "----------",
                               "--------",
                               "--------",
                               "--------",
                               "--------",
                               "--------",
                               "--------");

    for (i = 0; i < sessions.size(); ++i) {
        debug_command_reply_append(reply,
                                   offset,
                                   "    %-4d %12llu %12llu %10llu %10llu %10llu %8llu"
                                   " %12llu %12llu %10llu %10llu"
                                   " %8llu %8llu %8llu %8llu %8llu %8llu\n",
                                   sessions[i].id,
                                   static_cast<unsigned long long>(sessions[i].videoFrames),
                                   static_cast<unsigned long long>(sessions[i].videoBytes),
                                   static_cast<unsigned long long>(sessions[i].videoNotReady),
                                   static_cast<unsigned long long>(sessions[i].videoWaitingKeyframeDrops),
                                   static_cast<unsigned long long>(sessions[i].videoSendFail),
                                   static_cast<unsigned long long>(sessions[i].videoPliReceived),
                                   static_cast<unsigned long long>(sessions[i].audioFrames),
                                   static_cast<unsigned long long>(sessions[i].audioBytes),
                                   static_cast<unsigned long long>(sessions[i].audioNotReady),
                                   static_cast<unsigned long long>(sessions[i].audioSendFail),
                                   static_cast<unsigned long long>(sessions[i].signalingRxMessages),
                                   static_cast<unsigned long long>(sessions[i].signalingTxMessages),
                                   static_cast<unsigned long long>(sessions[i].localCandidates),
                                   static_cast<unsigned long long>(sessions[i].remoteCandidates),
                                   static_cast<unsigned long long>(sessions[i].dataChannelRxMessages),
                                   static_cast<unsigned long long>(sessions[i].dataChannelTxMessages));
    }
}

/* 打印 PLI 测试的 session 级状态，便于确认 IDR 是否已抑制、是否由 PLI 或超时恢复。 */
static void debug_print_pli_test_table(char *reply,
                                       size_t *offset,
                                       const std::vector<WebRtcSessionStats> &sessions)
{
    size_t i;
    bool hasTestRecord;

    hasTestRecord = false;
    for (i = 0; i < sessions.size(); ++i) {
        if (sessions[i].pliTestSuppressingVideo || sessions[i].pliTestStarts > 0) {
            hasTestRecord = true;
            break;
        }
    }
    if (!hasTestRecord) {
        return;
    }

    debug_command_reply_append(reply,
                               offset,
                               "\n  PLI test\n"
                               "    %-4s %-12s %12s %16s %14s %12s\n"
                               "    %-4s %-12s %12s %16s %14s %12s\n",
                               "id",
                               "state",
                               "starts",
                               "video_suppressed",
                               "pli_recovered",
                               "timeouts",
                               "----",
                               "------------",
                               "------------",
                               "----------------",
                               "--------------",
                               "------------");
    for (i = 0; i < sessions.size(); ++i) {
        if (!sessions[i].pliTestSuppressingVideo && sessions[i].pliTestStarts == 0) {
            continue;
        }
        debug_command_reply_append(reply,
                                   offset,
                                   "    %-4d %-12s %12llu %16llu %14llu %12llu\n",
                                   sessions[i].id,
                                   sessions[i].pliTestSuppressingVideo ? "WAIT_PLI" : "IDLE",
                                   static_cast<unsigned long long>(sessions[i].pliTestStarts),
                                   static_cast<unsigned long long>(sessions[i].pliTestSuppressedVideoFrames),
                                   static_cast<unsigned long long>(sessions[i].pliTestPliRecovered),
                                   static_cast<unsigned long long>(sessions[i].pliTestTimeouts));
    }
}

/*
 * getWebRTC shell 命令处理函数。
 * 调用时机：用户执行 rkmgw getWebRTC 时由 debugCommandServer 调用，函数只读取快照并格式化文本。
 */
static int debug_handle_get_webrtc(void *user_data, const char *input, char *output)
{
    WebRtcServerStats stats;
    std::unique_lock<std::mutex> lock(g_web_rtc_debug_mutex, std::defer_lock);
    size_t offset;
    size_t i;

    (void)user_data;
    (void)input;

    offset = 0;
    lock.lock();

    debug_command_reply_append(output,
                               &offset,
                               "WebRTC Debug\n"
                               "============\n"
                               "server_count : %zu\n",
                               g_web_rtc_debug_servers.size());

    if (g_web_rtc_debug_servers.empty()) {
        debug_command_reply_append(output, &offset, "\nno WebRTC server registered\n");
        return MEDIA_OK;
    }

    /*
     * 查询期间持有调试模块全局锁，保证 stop/unregister 不会在命令输出过程中释放 server 对象。
     * getStats() 只读取快照，不回调 webRTCDebug，因此不会形成反向锁依赖。
     */
    for (i = 0; i < g_web_rtc_debug_servers.size(); ++i) {
        if (!g_web_rtc_debug_servers[i]) {
            continue;
        }

        stats = WebRtcServerStats();
        g_web_rtc_debug_servers[i]->getStats(stats);
        debug_command_reply_append(output, &offset, "\n");
        debug_print_server_overview(output, &offset, i, stats);
        debug_print_media_summary(output, &offset, stats);
        debug_print_session_state_table(output, &offset, stats.sessions);
        debug_print_session_counter_table(output, &offset, stats.sessions);
        debug_print_pli_test_table(output, &offset, stats.sessions);
    }

    return MEDIA_OK;
}

/*
 * testWebRTCPLI <server_id> <session_id> <on|off>
 * on 会持续抑制指定 session 的全部视频帧，直到收到该 session 的 PLI/FIR 或 15 秒超时。
 */
static int debug_handle_test_webrtc_pli(void *user_data, const char *input, char *output)
{
    WebRtcServer *server;
    std::unique_lock<std::mutex> lock(g_web_rtc_debug_mutex, std::defer_lock);
    char action[8] = {0};
    size_t offset;
    long serverIndex;
    long sessionId;
    int enabled;
    int parseCount;

    (void)user_data;
    offset = 0;
    server = NULL;
    serverIndex = -1;
    sessionId = -1;
    enabled = 0;
    parseCount = 0;
    output[0] = '\0';
    debug_command_reply_append(output, &offset, "cmd=testWebRTCPLI\n");

    if (!input) {
        debug_command_reply_append(output,
                                   &offset,
                                   "ret=-1\nusage=testWebRTCPLI <server_id> <session_id> <on|off>\n");
        return MEDIA_ERR_INVALID_PARAM;
    }
    parseCount = sscanf(input, "%ld %ld %7s", &serverIndex, &sessionId, action);
    if (parseCount != 3 || serverIndex < 0 || sessionId <= 0 ||
        (strcmp(action, "on") != 0 && strcmp(action, "off") != 0)) {
        debug_command_reply_append(output,
                                   &offset,
                                   "ret=-1\nusage=testWebRTCPLI <server_id> <session_id> <on|off>\n");
        return MEDIA_ERR_INVALID_PARAM;
    }
    enabled = strcmp(action, "on") == 0;

    lock.lock();
    if (static_cast<size_t>(serverIndex) >= g_web_rtc_debug_servers.size() ||
        !g_web_rtc_debug_servers[serverIndex]) {
        debug_command_reply_append(output,
                                   &offset,
                                   "ret=-1\nerror=server_id %ld not found\n",
                                   serverIndex);
        return MEDIA_ERR_INVALID_PARAM;
    }
    server = g_web_rtc_debug_servers[serverIndex];
    if (!server->setSessionPliTestVideoSuppression(static_cast<int>(sessionId), enabled)) {
        debug_command_reply_append(output,
                                   &offset,
                                   "ret=-1\nerror=session %ld is unavailable or video track is not ready\n",
                                   sessionId);
        return MEDIA_ERR_NOT_READY;
    }

    debug_command_reply_append(output,
                               &offset,
                               "ret=0\nserver_id=%ld\nsession_id=%ld\nstate=%s\n",
                               serverIndex,
                               sessionId,
                               enabled ? "WAIT_PLI" : "IDLE");
    return MEDIA_OK;
}

/* 按需注册 getWebRTC 命令，避免多个 WebRTC 输出通道重复注册同名命令。 */
static int debug_ensure_command_registered()
{
    int ret;

    if (!g_web_rtc_status_command_registered) {
        ret = regDebugCmd("getWebRTC", debug_handle_get_webrtc, NULL);
        if (ret != MEDIA_OK) {
            LOG_WARN("[WEBRTC] register shell command getWebRTC failed ret=%d", ret);
            return ret;
        }
        g_web_rtc_status_command_registered = true;
    }

    if (!g_web_rtc_pli_test_command_registered) {
        ret = regDebugCmd("testWebRTCPLI", debug_handle_test_webrtc_pli, NULL);
        if (ret != MEDIA_OK) {
            LOG_WARN("[WEBRTC] register shell command testWebRTCPLI failed ret=%d", ret);
            return ret;
        }
        g_web_rtc_pli_test_command_registered = true;
    }
    return MEDIA_OK;
}

/*
 * 注册 WebRtcServer 到调试命令模块。
 * 调用时机：WebRTC 输出通道 start 成功后调用；同一个 server 重复注册会被忽略。
 */
int web_rtc_debug_register_server(WebRtcServer *server)
{
    std::vector<WebRtcServer *>::iterator iter;
    std::unique_lock<std::mutex> lock(g_web_rtc_debug_mutex, std::defer_lock);
    int ret;

    if (!server) {
        LOG_ERROR("[WEBRTC] debug register failed: server is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    lock.lock();

    iter = std::find(g_web_rtc_debug_servers.begin(), g_web_rtc_debug_servers.end(), server);
    if (iter == g_web_rtc_debug_servers.end()) {
        g_web_rtc_debug_servers.push_back(server);
    }

    ret = debug_ensure_command_registered();
    if (ret != MEDIA_OK) {
        return ret;
    }

    return MEDIA_OK;
}

/*
 * 从调试命令模块注销 WebRtcServer。
 * 调用时机：WebRTC 输出通道 stop 释放 server 前调用，保证 shell 查询不会访问悬空指针。
 */
void web_rtc_debug_unregister_server(WebRtcServer *server)
{
    std::vector<WebRtcServer *>::iterator iter;
    std::unique_lock<std::mutex> lock(g_web_rtc_debug_mutex, std::defer_lock);

    if (!server) {
        return;
    }

    lock.lock();

    iter = std::remove(g_web_rtc_debug_servers.begin(), g_web_rtc_debug_servers.end(), server);
    g_web_rtc_debug_servers.erase(iter, g_web_rtc_debug_servers.end());
}

} // namespace webrtc
} // namespace rkmedia
