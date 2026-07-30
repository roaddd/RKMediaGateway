#include <atomic>
#include <cctype>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <rtc/datachannel.hpp>
#include <rtc/global.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/websocket.hpp>
#include <rtc/websocketserver.hpp>

namespace {

struct WsSession {
    int id;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::mutex mutex;
};

std::atomic<int> g_next_session_id(1);
std::mutex g_sessions_mutex;
std::map<int, std::shared_ptr<WsSession>> g_sessions;

/*
 * 将字符串安全放进 JSON 字符串字段。
 * SDP 和 candidate 中包含换行、反斜杠等字符，直接拼 JSON 会破坏格式。
 */
std::string jsonEscape(const std::string &value)
{
    std::ostringstream out;
    char ch;
    size_t i;

    for (i = 0; i < value.size(); ++i) {
        ch = value[i];
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u00";
                out << "0123456789abcdef"[(ch >> 4) & 0x0f];
                out << "0123456789abcdef"[ch & 0x0f];
            } else {
                out << ch;
            }
            break;
        }
    }

    return out.str();
}

/*
 * 从固定格式的扁平 JSON 中读取字符串字段。
 * 当前测试只处理浏览器页面发来的 type/sdp/candidate/mid/cmd，不引入额外 JSON 库。
 */
bool jsonGetString(const std::string &json, const std::string &key, std::string &value)
{
    std::string pattern;
    size_t pos;
    size_t i;
    char ch;
    char esc;
    int code;
    int digit;

    pattern = "\"" + key + "\"";
    pos = json.find(pattern);
    if (pos == std::string::npos) {
        return false;
    }

    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) {
        return false;
    }

    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }

    value.clear();
    for (i = pos + 1; i < json.size(); ++i) {
        ch = json[i];
        if (ch == '"') {
            return true;
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }

        ++i;
        if (i >= json.size()) {
            return false;
        }

        esc = json[i];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            value.push_back(esc);
            break;
        case 'b':
            value.push_back('\b');
            break;
        case 'f':
            value.push_back('\f');
            break;
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'u':
            code = 0;
            if (i + 4 >= json.size()) {
                return false;
            }
            for (digit = 0; digit < 4; ++digit) {
                ++i;
                ch = json[i];
                code <<= 4;
                if (ch >= '0' && ch <= '9') {
                    code += ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    code += ch - 'a' + 10;
                } else if (ch >= 'A' && ch <= 'F') {
                    code += ch - 'A' + 10;
                } else {
                    return false;
                }
            }
            if (code < 0x80) {
                value.push_back(static_cast<char>(code));
            } else {
                value.push_back('?');
            }
            break;
        default:
            value.push_back(esc);
            break;
        }
    }

    return false;
}

/* 通过 WebSocket 发送信令消息。 */
void wsSend(const std::shared_ptr<rtc::WebSocket> &ws, const std::string &message)
{
    if (ws && ws->isOpen()) {
        ws->send(message);
        std::cout << "[WS] send " << message << std::endl;
    }
}

/* 发送 SDP 描述，type 为 answer 或 offer。 */
void wsSendDescription(const std::shared_ptr<rtc::WebSocket> &ws,
                       const std::string &type,
                       const std::string &sdp)
{
    std::string message;

    message = "{\"type\":\"" + jsonEscape(type) + "\",\"sdp\":\"" + jsonEscape(sdp) + "\"}";
    wsSend(ws, message);
}

/* 将 libdatachannel 生成的本地 ICE candidate 转成浏览器可识别的信令 JSON。 */
void wsSendCandidate(const std::shared_ptr<rtc::WebSocket> &ws, const rtc::Candidate &candidate)
{
    std::string message;

    message = "{\"type\":\"candidate\",\"candidate\":\"" + jsonEscape(candidate.candidate()) +
              "\",\"mid\":\"" + jsonEscape(candidate.mid()) + "\"}";
    wsSend(ws, message);
}

/* 通过 DataChannel 发送 IPC JSON 消息。 */
void dcSendJson(const std::shared_ptr<rtc::DataChannel> &dc, const std::string &message)
{
    if (dc && dc->isOpen()) {
        dc->send(message);
        std::cout << "[DC] send " << message << std::endl;
    }
}

/*
 * 处理浏览器 DataChannel 发来的 IPC 消息。
 * 第一阶段只支持 ping 和 get_status，用于验证 DataChannel 双向收发。
 */
void handleIpcMessage(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::shared_ptr<rtc::DataChannel> dc;
    std::string cmd;
    std::string response;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        dc = session->dc;
    }

    std::cout << "[DC] recv " << message << std::endl;
    jsonGetString(message, "cmd", cmd);

    /* ping/pong 用于验证 IPC 通路最小闭环。 */
    if (cmd == "ping") {
        response = "{\"cmd\":\"pong\",\"code\":0}";
    } else if (cmd == "get_status") {
        response = "{\"cmd\":\"status\",\"code\":0,\"data\":{\"webrtc\":\"connected\"}}";
    } else {
        response = "{\"cmd\":\"unknown\",\"code\":400,\"message\":\"unsupported command\"}";
    }

    dcSendJson(dc, response);
}

/*
 * 绑定浏览器创建的 DataChannel。
 * 浏览器页面作为 offer 方会先 createDataChannel("ipc")，设备端通过 onDataChannel 收到。
 */
void bindDataChannel(const std::shared_ptr<WsSession> &session,
                     const std::shared_ptr<rtc::DataChannel> &dc)
{
    std::weak_ptr<WsSession> weakSession;
    int id;

    weakSession = session;
    id = session->id;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->dc = dc;
    }

    std::cout << "[DC] session " << id << " datachannel label=" << dc->label() << std::endl;

    dc->onOpen([id]() {
        std::cout << "[DC] session " << id << " open" << std::endl;
    });

    dc->onClosed([id]() {
        std::cout << "[DC] session " << id << " closed" << std::endl;
    });

    dc->onError([id](std::string error) {
        std::cout << "[DC] session " << id << " error: " << error << std::endl;
    });

    dc->onMessage(nullptr, [weakSession](std::string message) {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        if (!session) {
            return;
        }
        handleIpcMessage(session, message);
    });
}

/*
 * 为一个浏览器 WebSocket 会话创建 PeerConnection。
 * 这里集中注册 SDP、ICE、DataChannel 和状态回调。
 */
std::shared_ptr<rtc::PeerConnection> createPeerConnection(const std::shared_ptr<WsSession> &session)
{
    rtc::Configuration config;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::weak_ptr<WsSession> weakSession;
    int id;

    config.disableAutoNegotiation = true;
    pc = std::make_shared<rtc::PeerConnection>(config);
    weakSession = session;
    id = session->id;

    /*
     * 设备端生成 Answer 后会进入该回调。
     * 回调内只负责把 SDP 通过 WebSocket 发回浏览器。
     */
    pc->onLocalDescription([weakSession](rtc::Description description) {
        std::shared_ptr<WsSession> session;
        std::shared_ptr<rtc::WebSocket> ws;
        std::string type;
        std::string sdp;

        session = weakSession.lock();
        if (!session) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            ws = session->ws;
        }

        type = description.typeString();
        sdp = description.generateSdp();
        std::cout << "[PC] local description type=" << type << " bytes=" << sdp.size() << std::endl;
        wsSendDescription(ws, type, sdp);
    });

    /*
     * ICE candidate 使用 trickle 方式发送。
     * 浏览器收到后调用 addIceCandidate。
     */
    pc->onLocalCandidate([weakSession](rtc::Candidate candidate) {
        std::shared_ptr<WsSession> session;
        std::shared_ptr<rtc::WebSocket> ws;

        session = weakSession.lock();
        if (!session) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            ws = session->ws;
        }

        std::cout << "[PC] local candidate " << candidate.candidate()
                  << " mid=" << candidate.mid() << std::endl;
        wsSendCandidate(ws, candidate);
    });

    /* 浏览器端创建 DataChannel 后，设备端会在这里收到并绑定 IPC 处理。 */
    pc->onDataChannel([weakSession](std::shared_ptr<rtc::DataChannel> dc) {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        if (!session) {
            return;
        }
        bindDataChannel(session, dc);
    });

    pc->onStateChange([id](rtc::PeerConnection::State state) {
        std::cout << "[PC] session " << id << " state=" << state << std::endl;
    });

    pc->onIceStateChange([id](rtc::PeerConnection::IceState state) {
        std::cout << "[PC] session " << id << " ice=" << state << std::endl;
    });

    pc->onGatheringStateChange([id](rtc::PeerConnection::GatheringState state) {
        std::cout << "[PC] session " << id << " gathering=" << state << std::endl;
    });

    return pc;
}

/*
 * 处理浏览器发来的 offer。
 * 流程：创建 PeerConnection -> setRemoteDescription(offer) -> setLocalDescription(answer)。
 */
void handleOffer(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string sdp;
    std::unique_ptr<rtc::Description> offer;

    if (!jsonGetString(message, "sdp", sdp) && !jsonGetString(message, "description", sdp)) {
        std::cout << "[WS] offer missing sdp" << std::endl;
        return;
    }

    pc = createPeerConnection(session);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->pc = pc;
    }

    offer.reset(new rtc::Description(sdp, "offer"));
    pc->setRemoteDescription(*offer);
    pc->setLocalDescription(rtc::Description::Type::Answer);
}

/* 处理浏览器通过 WebSocket 发来的远端 ICE candidate。 */
void handleCandidate(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string candidateText;
    std::string mid;
    rtc::Candidate candidate;

    if (!jsonGetString(message, "candidate", candidateText)) {
        return;
    }
    if (!jsonGetString(message, "mid", mid) && !jsonGetString(message, "sdpMid", mid)) {
        mid = "0";
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        pc = session->pc;
    }

    if (!pc) {
        std::cout << "[WS] candidate ignored before offer" << std::endl;
        return;
    }

    candidate = rtc::Candidate(candidateText, mid);
    pc->addRemoteCandidate(candidate);
    std::cout << "[PC] remote candidate " << candidateText << " mid=" << mid << std::endl;
}

/* WebSocket 文本消息入口，根据 type 分发到 offer/candidate/close 处理。 */
void handleWsText(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::string type;

    std::cout << "[WS] recv " << message << std::endl;
    if (!jsonGetString(message, "type", type)) {
        std::cout << "[WS] message missing type" << std::endl;
        return;
    }

    if (type == "offer") {
        handleOffer(session, message);
    } else if (type == "candidate") {
        handleCandidate(session, message);
    } else if (type == "close") {
        std::cout << "[WS] close requested" << std::endl;
        session->ws->close();
    } else {
        std::cout << "[WS] unsupported type=" << type << std::endl;
    }
}

/*
 * 绑定一个浏览器 WebSocket 连接。
 * WebSocket 断开时同步关闭并释放对应 PeerConnection 和 DataChannel。
 */
void bindWebSocket(const std::shared_ptr<WsSession> &session)
{
    std::weak_ptr<WsSession> weakSession;
    std::shared_ptr<rtc::WebSocket> ws;
    int id;

    weakSession = session;
    ws = session->ws;
    id = session->id;

    ws->onOpen([id]() {
        std::cout << "[WS] session " << id << " open" << std::endl;
    });

    ws->onClosed([weakSession, id]() {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        std::cout << "[WS] session " << id << " closed" << std::endl;
        if (!session) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->pc) {
                session->pc->close();
            }
            session->dc.reset();
            session->pc.reset();
        }

        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            g_sessions.erase(id);
        }
    });

    ws->onError([id](std::string error) {
        std::cout << "[WS] session " << id << " error: " << error << std::endl;
    });

    ws->onMessage(nullptr, [weakSession](std::string message) {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        if (!session) {
            return;
        }
        handleWsText(session, message);
    });
}

/* 打印命令行用法。 */
void printUsage(const char *program)
{
    std::cout << "Usage: " << program << " [port] [bind_address]" << std::endl;
    std::cout << "Default: " << program << " 8000 0.0.0.0" << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
    rtc::WebSocketServer::Configuration wsConfig;
    std::shared_ptr<rtc::WebSocketServer> server;
    uint16_t port;
    std::string bindAddress;

    port = 8000;
    bindAddress = "0.0.0.0";

    if (argc > 1) {
        if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    if (argc > 2) {
        bindAddress = argv[2];
    }

    try {
        rtc::InitLogger(rtc::LogLevel::Info);
        rtc::Preload();

        /* WebSocketServer 监听浏览器页面配置的 ws://设备IP:端口/browser。 */
        wsConfig.port = port;
        wsConfig.bindAddress = bindAddress;
        server = std::make_shared<rtc::WebSocketServer>(wsConfig);

        /*
         * 每个浏览器 WebSocket 连接创建一个 WsSession。
         * session 放入全局表，保证异步回调期间对象生命周期有效。
         */
        server->onClient([](std::shared_ptr<rtc::WebSocket> ws) {
            std::shared_ptr<WsSession> session;
            int id;

            id = g_next_session_id.fetch_add(1);
            session = std::make_shared<WsSession>();
            session->id = id;
            session->ws = ws;

            {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                g_sessions[id] = session;
            }

            std::cout << "[WS] session " << id << " connected";
            if (ws->remoteAddress()) {
                std::cout << " remote=" << *ws->remoteAddress();
            }
            if (ws->path()) {
                std::cout << " path=" << *ws->path();
            }
            std::cout << std::endl;

            bindWebSocket(session);
        });

        std::cout << "[OK] WebSocket signaling server listening on "
                  << bindAddress << ":" << server->port() << std::endl;
        std::cout << "[INFO] browser url example: ws://" << bindAddress
                  << ":" << server->port() << "/browser" << std::endl;
        std::cout << "[INFO] press ENTER to exit" << std::endl;
        std::cin.get();

        /* 退出时先停止监听，再清理所有仍在线的测试会话。 */
        server->stop();
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            g_sessions.clear();
        }
        rtc::Cleanup().wait();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[FAIL] unknown exception" << std::endl;
    }

    return 1;
}
