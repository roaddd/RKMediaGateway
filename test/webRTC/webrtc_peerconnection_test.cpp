#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rtc/datachannel.hpp>
#include <rtc/global.hpp>
#include <rtc/peerconnection.hpp>

namespace {

/** @description: 本测试要验证的 ICE 候选类型。 */
enum class TestMode {
    Basic,
    Stun,
    Turn
};

/** @description: 命令行解析后的测试参数；TURN 密码只从环境变量读取。 */
struct TestOptions {
    TestMode mode = TestMode::Basic;
    std::string server;
    uint16_t port = 3478;
    std::string username;
};

/** @description: 打印设备端 ICE 候选收集测试的用法。 */
void printUsage(const char *program)
{
    std::cerr << "Usage:\n"
              << "  " << program << "\n"
              << "  " << program << " stun <server> [port]\n"
              << "  " << program << " turn <server> <port> <username>\n"
              << "TURN mode requires RKMGW_TURN_PASSWORD in the environment.\n";
}

/** @description: 将端口字符串严格解析为 1～65535。 */
bool parsePort(const char *text, uint16_t *port)
{
    char *end = nullptr;
    long value = 0;

    if (!text || !port || text[0] == '\0') {
        std::cerr << "[FAIL] ICE server port is empty" << std::endl;
        return false;
    }

    errno = 0;
    value = std::strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > 65535) {
        std::cerr << "[FAIL] invalid ICE server port: " << text << std::endl;
        return false;
    }

    *port = static_cast<uint16_t>(value);
    return true;
}

/** @description: 校验命令行并选择基础、STUN 或 TURN 测试模式。 */
bool parseOptions(int argc, char **argv, TestOptions *options)
{
    if (!argv || !options) {
        std::cerr << "[FAIL] invalid test options" << std::endl;
        return false;
    }

    if (argc == 1) {
        return true;
    }

    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "stun") {
        if (argv[2][0] == '\0') {
            std::cerr << "[FAIL] STUN server is empty" << std::endl;
            return false;
        }
        options->mode = TestMode::Stun;
        options->server = argv[2];
        return argc == 3 || parsePort(argv[3], &options->port);
    }

    if (argc == 5 && std::string(argv[1]) == "turn") {
        if (argv[2][0] == '\0' || argv[4][0] == '\0') {
            std::cerr << "[FAIL] TURN server and username must be nonempty" << std::endl;
            return false;
        }
        if (!parsePort(argv[3], &options->port)) {
            return false;
        }
        options->mode = TestMode::Turn;
        options->server = argv[2];
        options->username = argv[4];
        return true;
    }

    std::cerr << "[FAIL] invalid ICE test arguments" << std::endl;
    return false;
}

/** @description: 将 libdatachannel 的 gathering 状态转换为稳定的日志文本。 */
const char *gatheringStateName(rtc::PeerConnection::GatheringState state)
{
    switch (state) {
    case rtc::PeerConnection::GatheringState::New:
        return "new";
    case rtc::PeerConnection::GatheringState::InProgress:
        return "in-progress";
    case rtc::PeerConnection::GatheringState::Complete:
        return "complete";
    }
    return "unknown";
}

/** @description: 将 ICE 候选类型转换为与 SDP 一致的日志文本。 */
const char *candidateTypeName(rtc::Candidate::Type type)
{
    switch (type) {
    case rtc::Candidate::Type::Host:
        return "host";
    case rtc::Candidate::Type::ServerReflexive:
        return "srflx";
    case rtc::Candidate::Type::PeerReflexive:
        return "prflx";
    case rtc::Candidate::Type::Relayed:
        return "relay";
    case rtc::Candidate::Type::Unknown:
        return "unknown";
    }
    return "unknown";
}

/** @description: 在限定时间内等待异步 ICE 回调满足条件。 */
bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

} // namespace

/**
 * @description: 在设备本机收集 ICE 候选，验证基础、STUN 或 TURN 路径。
 * @return: 0 表示收集到了所选模式要求的候选；1 表示参数或 ICE 测试失败。
 */
int main(int argc, char **argv)
{
    std::atomic<bool> gotOffer(false);
    std::atomic<bool> gatheringComplete(false);
    std::atomic<int> candidateCount(0);
    std::atomic<int> hostCount(0);
    std::atomic<int> srflxCount(0);
    std::atomic<int> relayCount(0);
    TestOptions options;
    rtc::Configuration config;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    const char *turnPassword = nullptr;
    bool completed = false;
    bool passed = false;

    if (argc == 2 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }
    if (!parseOptions(argc, argv, &options)) {
        printUsage(argv[0]);
        return 1;
    }

    try {
        /*
         * TURN 密码只从环境变量读取，并使用 IceServer 的独立参数构造函数，
         * 避免把密码写入命令行、ICE URI、测试输出或异常日志。
         */
        if (options.mode == TestMode::Turn) {
            turnPassword = std::getenv("RKMGW_TURN_PASSWORD");
            if (!turnPassword || turnPassword[0] == '\0') {
                std::cerr << "[FAIL] RKMGW_TURN_PASSWORD is not set" << std::endl;
                return 1;
            }
            config.iceServers.emplace_back(options.server, options.port,
                                           options.username, turnPassword,
                                           rtc::IceServer::RelayType::TurnUdp);
            config.iceTransportPolicy = rtc::TransportPolicy::Relay;
        } else if (options.mode == TestMode::Stun) {
            config.iceServers.emplace_back(options.server, options.port);
        }
        config.disableAutoNegotiation = true;

        /* 网络模式等待 ICE gathering 完成，不能把最先出现的 host 误判为通过。 */
        rtc::InitLogger(options.mode == TestMode::Turn ? rtc::LogLevel::None
                                                       : rtc::LogLevel::Warning);
        rtc::Preload();
        std::cout << "[INFO] mode="
                  << (options.mode == TestMode::Turn ? "turn" :
                      options.mode == TestMode::Stun ? "stun" : "basic");
        if (options.mode != TestMode::Basic) {
            std::cout << " server=" << options.server << ":" << options.port;
        }
        std::cout << std::endl;

        pc = std::make_shared<rtc::PeerConnection>(config);
        if (!pc) {
            std::cerr << "[FAIL] PeerConnection creation returned null" << std::endl;
            return 1;
        }

        pc->onLocalDescription([&gotOffer](rtc::Description description) {
            gotOffer = description.type() == rtc::Description::Type::Offer &&
                       !description.generateSdp().empty();
            std::cout << "[INFO] local description type=" << description.typeString()
                      << std::endl;
        });

        /* 按 libdatachannel 解析后的候选类型计数，输出地址供核对公网映射。 */
        pc->onLocalCandidate([&candidateCount, &hostCount, &srflxCount,
                              &relayCount](rtc::Candidate candidate) {
            rtc::Candidate::Type type = candidate.type();
            ++candidateCount;
            if (type == rtc::Candidate::Type::Host) {
                ++hostCount;
            } else if (type == rtc::Candidate::Type::ServerReflexive) {
                ++srflxCount;
            } else if (type == rtc::Candidate::Type::Relayed) {
                ++relayCount;
            }
            std::cout << "[INFO] ICE candidate type=" << candidateTypeName(type)
                      << " value=" << candidate.candidate()
                      << " mid=" << candidate.mid() << std::endl;
        });

        pc->onGatheringStateChange([&gatheringComplete](rtc::PeerConnection::GatheringState state) {
            std::cout << "[INFO] ICE gathering state=" << gatheringStateName(state) << std::endl;
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                gatheringComplete = true;
            }
        });

        /* 创建 DataChannel 以产生 application m-line，无需远端信令或媒体采集。 */
        dc = pc->createDataChannel("ice-gathering-test");
        if (!dc) {
            std::cerr << "[FAIL] DataChannel creation returned null" << std::endl;
            return 1;
        }
        pc->setLocalDescription(rtc::Description::Type::Offer);

        if (options.mode == TestMode::Basic) {
            completed = waitUntil([&gotOffer, &candidateCount] {
                return gotOffer.load() && candidateCount.load() > 0;
            }, 5000);
        } else {
            completed = waitUntil([&gatheringComplete] { return gatheringComplete.load(); }, 45000);
        }
        std::cout << "[INFO] candidate summary: total=" << candidateCount.load()
                  << " host=" << hostCount.load()
                  << " srflx=" << srflxCount.load()
                  << " relay=" << relayCount.load()
                  << " gathering_complete=" << (gatheringComplete.load() ? "yes" : "no")
                  << std::endl;

        if (!gotOffer.load()) {
            std::cerr << "[FAIL] Offer SDP was not generated" << std::endl;
        } else if (options.mode == TestMode::Basic && candidateCount.load() == 0) {
            std::cerr << "[FAIL] no ICE candidate was gathered within 5 seconds" << std::endl;
        } else if (options.mode != TestMode::Basic && !completed) {
            std::cerr << "[FAIL] ICE gathering did not complete within 45 seconds" << std::endl;
        } else if (options.mode == TestMode::Stun && srflxCount.load() == 0) {
            std::cerr << "[FAIL] no srflx candidate from STUN server" << std::endl;
        } else if (options.mode == TestMode::Turn && relayCount.load() == 0) {
            std::cerr << "[FAIL] no relay candidate from TURN server" << std::endl;
        } else {
            passed = true;
        }

        pc->close();
        dc.reset();
        pc.reset();
        rtc::Cleanup().wait();
        if (!passed) {
            return 1;
        }
        std::cout << "[OK] ICE "
                  << (options.mode == TestMode::Turn ? "relay" :
                      options.mode == TestMode::Stun ? "srflx" : "basic")
                  << " candidate test passed" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        if (options.mode == TestMode::Turn) {
            std::cerr << "[FAIL] TURN ICE test raised an exception; details suppressed to protect credentials"
                      << std::endl;
        } else {
            std::cerr << "[FAIL] exception: " << e.what() << std::endl;
        }
    } catch (...) {
        std::cerr << "[FAIL] unknown ICE test exception" << std::endl;
    }

    return 1;
}
