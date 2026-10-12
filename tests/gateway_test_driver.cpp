#include "gateway/gateway.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void stop(int) { stopRequested = 1; }
}

int main()
{
    using namespace uav::gateway;
    Config config;
    config.port = 0;
    config.adminToken = "integration-admin-token";
    config.viewerToken = "integration-viewer-token";
    config.ticketLifetime = std::chrono::seconds(1);
    config.maxPendingCommands = 2;
    config.accessLifetime = std::chrono::seconds(2);
    config.refreshLifetime = std::chrono::seconds(8);
    config.playbackLifetime = std::chrono::seconds(2);
    config.mediaServiceToken = "integration-media-token";
    if (const char *state = std::getenv("UAV_GATEWAY_STATE")) config.stateFile = state;
    Gateway gateway(config);
    auto resolves = std::make_shared<std::atomic<int>>(0);
    gateway.registerDevice({"camera", "camera", "Test camera", true, {}, {{"state", "ready"}}},
        {"echo", "slow", "fail", "throw", "large", "flood", "dispatch"}, [&gateway](const std::string &action, const boost::json::object &parameters) {
            if (action == "slow") std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (action == "throw") throw std::runtime_error("backend-specific internal detail");
            if (action == "large") return CommandResult{true, "ok", "", {{"text", std::string(70000, 'x')}}};
            if (action == "dispatch") return CommandResult{true, "command_sent", "", {}, CommandCompletion::dispatched};
            if (action == "flood")
                for (int i = 0; i < 256; ++i) gateway.publish("test.feed", boost::json::object{{"text", std::string(60000, 'x')}});
            return CommandResult{action != "fail", action == "fail" ? "device_rejected" : "ok", "", parameters};
        });
    gateway.registerDevice({"radar", "radar", "Pending radar", true, {}, {{"state", "not_implemented"}}});
    gateway.registerDevice({"disabled", "camera", "Disabled camera", false, {}, {}});
    gateway.registerStream({"disabled.main", "disabled", "h264", {"webrtc"}},
        [](const std::string &) -> std::optional<PlaybackEndpoint> {
            return PlaybackEndpoint{"https://media.invalid/disabled", true, {}};
        });
    gateway.registerStream({"camera.main", "camera", "h264", {"webrtc", "rtsp"}},
        [resolves](const std::string &protocol) -> std::optional<PlaybackEndpoint> {
            const int count = ++*resolves;
            if (protocol == "rtsp") return PlaybackEndpoint{"", false, {}};
            return PlaybackEndpoint{"https://media.invalid/camera/main/whep", true, {{"resolve_count", count}}};
        });
    gateway.registerStream({"unsafe.main", "camera", "h264", {"webrtc"}},
        [](const std::string &) -> std::optional<PlaybackEndpoint> {
            return PlaybackEndpoint{"javascript:alert(1)", true, {}};
        });
    gateway.registerStream({"error.main", "camera", "h264", {"webrtc"}},
        [](const std::string &) -> std::optional<PlaybackEndpoint> { throw std::runtime_error("secret resolver detail"); });
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    gateway.start();
    std::cout << gateway.port() << std::endl;
    while (!stopRequested) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gateway.stop();
    gateway.stop(); // 幂等退出
}
