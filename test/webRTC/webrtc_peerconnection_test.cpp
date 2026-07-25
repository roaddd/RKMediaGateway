#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <rtc/datachannel.hpp>
#include <rtc/global.hpp>
#include <rtc/peerconnection.hpp>

namespace {

/* 保持测试日志输出稳定，便于快速判断 ICE gathering 状态。 */
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

/* libdatachannel 的 SDP 和 ICE 结果通过异步回调返回。 */
bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

} // namespace

int main()
{
    /* 这些标志位对应本测试需要验证的关键检查点。 */
    std::atomic<bool> gotOffer(false);
    std::atomic<bool> gotCandidate(false);
    std::atomic<bool> gatheringComplete(false);

    try {
        /*
         * Preload 用于初始化 libdatachannel 全局状态。
         * 如果共享库或运行时依赖无法加载，应在创建 PeerConnection 前失败。
         */
        rtc::InitLogger(rtc::LogLevel::Info);
        rtc::Preload();
        std::cout << "[OK] libdatachannel loaded" << std::endl;

        rtc::Configuration config;
        /* 关闭自动协商，测试流程中显式触发 Offer 生成。 */
        config.disableAutoNegotiation = true;

        rtc::PeerConnection pc(config);
        std::cout << "[OK] PeerConnection created" << std::endl;

        /* setLocalDescription(Offer) 后会通过该回调返回本地 Offer SDP。 */
        pc.onLocalDescription([&gotOffer](rtc::Description description) {
            const std::string sdp = description.generateSdp();
            gotOffer = (description.type() == rtc::Description::Type::Offer) && !sdp.empty();

            std::cout << "[OK] local description type=" << description.typeString()
                      << ", sdp bytes=" << sdp.size() << std::endl;
            std::cout << "----- BEGIN OFFER SDP -----" << std::endl;
            std::cout << sdp;
            std::cout << "----- END OFFER SDP -----" << std::endl;
        });

        /* 收到任意本地 candidate 即可证明 ICE candidate 回调链路正常。 */
        pc.onLocalCandidate([&gotCandidate](rtc::Candidate candidate) {
            gotCandidate = true;
            std::cout << "[OK] ICE candidate: " << candidate.candidate()
                      << " mid=" << candidate.mid() << std::endl;
        });

        pc.onGatheringStateChange([&gatheringComplete](rtc::PeerConnection::GatheringState state) {
            std::cout << "[INFO] ICE gathering state=" << gatheringStateName(state) << std::endl;
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                gatheringComplete = true;
            }
        });

        /*
         * 不接视频、浏览器或 WebSocket。
         * 这里创建 DataChannel 只是为了生成最小 WebRTC Offer 所需的 application m-line。
         */
        auto dc = pc.createDataChannel("minimal-test");
        if (!dc) {
            std::cerr << "[FAIL] DataChannel creation returned null" << std::endl;
            return 1;
        }

        pc.setLocalDescription(rtc::Description::Type::Offer);

        if (!waitUntil([&gotOffer] { return gotOffer.load(); }, 3000)) {
            std::cerr << "[FAIL] Offer SDP was not generated before timeout" << std::endl;
            return 1;
        }

        /* 等待一个本地 candidate；若 gathering 结束仍无 candidate，则测试失败。 */
        waitUntil([&gotCandidate, &gatheringComplete] {
            return gotCandidate.load() || gatheringComplete.load();
        }, 5000);

        if (!gotCandidate) {
            std::cerr << "[FAIL] ICE candidate callback did not produce a candidate before timeout"
                      << std::endl;
            return 1;
        }

        pc.close();
        rtc::Cleanup().wait();
        std::cout << "[OK] minimal PeerConnection test passed" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[FAIL] unknown exception" << std::endl;
    }

    return 1;
}
