/**
 * @file webrtc_signaling_protocol_test.cpp
 * @brief 验证云端信令 JSON、会话路由封装和 DataChannel 命令解析边界。
 */
#include "cloudSignalingProtocol.h"
#include "webrtcDataChannelMessage.h"
#include "webrtcJson.h"

#include <iostream>
#include <string>
#include <vector>

using rkmedia::webrtc::CloudSignalingMessage;
using rkmedia::webrtc::cloud_signaling_make_close;
using rkmedia::webrtc::cloud_signaling_make_registration;
using rkmedia::webrtc::cloud_signaling_parse_message;
using rkmedia::webrtc::cloud_signaling_wrap_session_message;
using rkmedia::webrtc::data_channel_parse_command;
using rkmedia::webrtc::webrtc_json_escape;

#define CHECK_TRUE(expression)                                                     \
    do {                                                                           \
        if (!(expression)) {                                                       \
            std::cerr << "check failed: " #expression << " line=" << __LINE__     \
                      << std::endl;                                                \
            return false;                                                          \
        }                                                                          \
    } while (0)

/** @brief 验证 JSON 字符串转义不会破坏 SDP 和凭据中的控制字符。 */
static bool testJsonEscape()
{
    std::string escaped;

    escaped = webrtc_json_escape("a\"b\\c\r\n");
    CHECK_TRUE(escaped == "a\\\"b\\\\c\\r\\n");
    return true;
}

/** @brief 验证云端入站消息必须是 version=1、无重复键的扁平对象。 */
static bool testCloudMessageParsing()
{
    CloudSignalingMessage message;
    bool parsed = false;

    parsed = cloud_signaling_parse_message(
        "{\"version\":1,\"type\":\"opened\",\"session_id\":\"s-1\","
        "\"stream_id\":\"main\",\"device_id\":\"ipc-1\"}",
        message);
    CHECK_TRUE(parsed);
    CHECK_TRUE(message.get("type") == "opened");
    CHECK_TRUE(message.get("session_id") == "s-1");
    CHECK_TRUE(message.get("stream_id") == "main");

    CHECK_TRUE(!cloud_signaling_parse_message(
        "{\"version\":2,\"type\":\"registered\"}", message));
    CHECK_TRUE(!cloud_signaling_parse_message(
        "{\"version\":1,\"type\":\"close\",\"type\":\"offer\"}", message));
    CHECK_TRUE(!cloud_signaling_parse_message(
        "{\"version\":1,\"session_id\":\"s-1\"}", message));
    return true;
}

/** @brief 验证设备登记、会话关闭和本地信令到云端信封的构造。 */
static bool testCloudMessageBuilding()
{
    std::vector<std::string> streamIds;
    std::string registration;
    std::string closeMessage;
    std::string envelope;
    bool wrapped = false;

    streamIds.push_back("main");
    streamIds.push_back("sub\"stream");
    registration = cloud_signaling_make_registration(
        "ipc-1", "secret\"token", streamIds);
    CHECK_TRUE(registration.find("\"type\":\"register\"") != std::string::npos);
    CHECK_TRUE(registration.find("sub\\\"stream") != std::string::npos);
    CHECK_TRUE(registration.find("secret\\\"token") != std::string::npos);

    closeMessage = cloud_signaling_make_close("s-1", "client_closed");
    CHECK_TRUE(closeMessage ==
               "{\"version\":1,\"type\":\"close\",\"session_id\":\"s-1\","
               "\"reason\":\"client_closed\"}");

    wrapped = cloud_signaling_wrap_session_message(
        "s-1", "{\"type\":\"answer\",\"sdp\":\"v=0\\r\\n\"}", envelope);
    CHECK_TRUE(wrapped);
    CHECK_TRUE(envelope.find("\"type\":\"answer\"") != std::string::npos);
    CHECK_TRUE(envelope.find("\"version\":1") != std::string::npos);
    CHECK_TRUE(envelope.find("\"session_id\":\"s-1\"") != std::string::npos);
    CHECK_TRUE(!cloud_signaling_wrap_session_message("", "{}", envelope));
    return true;
}

/** @brief 验证 DataChannel 命令不会套用 SDP/ICE 信令的 type 字段规则。 */
static bool testDataChannelCommandParsing()
{
    std::string command;
    bool parsed = false;

    parsed = data_channel_parse_command("{\"cmd\":\"getStatus\"}", command);
    CHECK_TRUE(parsed);
    CHECK_TRUE(command == "getStatus");
    CHECK_TRUE(!data_channel_parse_command("{\"type\":\"offer\"}", command));
    CHECK_TRUE(command.empty());
    CHECK_TRUE(!data_channel_parse_command("{\"cmd\":12}", command));
    return true;
}

int main()
{
    if (!testJsonEscape() || !testCloudMessageParsing() ||
        !testCloudMessageBuilding() || !testDataChannelCommandParsing()) {
        std::cerr << "webrtc signaling protocol tests failed" << std::endl;
        return 1;
    }
    std::cout << "webrtc signaling protocol tests passed" << std::endl;
    return 0;
}
