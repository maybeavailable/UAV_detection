#include "gateway/gateway.h"
#include "gateway/files.h"

#include <boost/json.hpp>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace uav::gateway {
namespace {
volatile std::sig_atomic_t stopped = 0;
void onSignal(int) { stopped = 1; }

std::string env(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

std::uint16_t parsePort(const std::string &text)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("port must be an integer in 0..65535");
    const auto value = std::stoul(text);
    if (value > 65535) throw std::invalid_argument("port out of range");
    return static_cast<std::uint16_t>(value);
}

std::string textOf(const boost::json::value &value)
{
    const auto &text = value.as_string();
    return std::string(text.data(), text.size());
}

std::size_t setting(const char *name, std::size_t fallback, std::size_t maximum)
{
    auto text = env(name);
    if (text.empty()) return fallback;
    if (text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(std::string("invalid integer: ") + name);
    auto number = std::stoul(text);
    if (number == 0 || number > maximum) throw std::invalid_argument(std::string("out of range: ") + name);
    return number;
}
} // namespace

Config configFromEnvironment()
{
    Config config;
    config.adminToken = env("UAV_GATEWAY_TOKEN");
    config.viewerToken = env("UAV_GATEWAY_VIEW_TOKEN");
    if (auto bind = env("UAV_GATEWAY_BIND"); !bind.empty()) config.bindAddress = bind;
    if (auto port = env("UAV_GATEWAY_PORT"); !port.empty()) config.port = parsePort(port);
    if (auto origin = env("UAV_GATEWAY_ORIGIN"); !origin.empty()) config.allowedOrigins.push_back(origin);
    config.stateFile = std::getenv("UAV_GATEWAY_STATE") ? env("UAV_GATEWAY_STATE") : "data/gateway-state.json";
    config.webRoot = env("UAV_GATEWAY_WEB_ROOT");
    config.tlsCertificate = env("UAV_GATEWAY_TLS_CERT");
    config.tlsPrivateKey = env("UAV_GATEWAY_TLS_KEY");
    config.mediaServiceToken = env("UAV_GATEWAY_MEDIA_TOKEN");
    config.maxConnections = setting("UAV_GATEWAY_MAX_CONNECTIONS", 64, 1024);
    config.maxPendingCommands = setting("UAV_GATEWAY_MAX_COMMANDS", 32, 256);
    config.accessLifetime = std::chrono::seconds(setting("UAV_GATEWAY_ACCESS_TTL", 900, 86400));
    config.refreshLifetime = std::chrono::seconds(setting("UAV_GATEWAY_REFRESH_TTL", 28800, 604800));
    config.playbackLifetime = std::chrono::seconds(setting("UAV_GATEWAY_PLAYBACK_TTL", 120, 3600));
    config.ticketLifetime = std::chrono::seconds(setting("UAV_GATEWAY_TICKET_TTL", 30, 300));
    return config;
}

void loadRegistry(Gateway &gateway, const std::string &path)
{
    if (path.empty()) return;
    auto root = detail::readRegistryFile(path);
    if (auto devices = root.if_contains("devices"))
    {
        for (const auto &value : devices->as_array())
        {
            const auto &object = value.as_object();
            Device device;
            device.id = textOf(object.at("id"));
            device.type = textOf(object.at("type"));
            if (auto name = object.if_contains("name")) device.name = textOf(*name);
            if (auto enabled = object.if_contains("enabled")) device.enabled = enabled->as_bool();
            if (auto config = object.if_contains("config")) device.config = config->as_object();
            gateway.registerDevice(std::move(device));
        }
    }
    if (auto streams = root.if_contains("streams"))
    {
        for (const auto &value : streams->as_array())
        {
            const auto &object = value.as_object();
            Stream stream;
            stream.id = textOf(object.at("id"));
            stream.deviceId = textOf(object.at("device_id"));
            if (auto codec = object.if_contains("codec")) stream.codec = textOf(*codec);
            for (const auto &protocol : object.at("protocols").as_array())
                stream.protocols.emplace_back(textOf(protocol));
            // 配置目录不能证明媒体服务可用。真实服务通过 registerStream 注入 resolver。
            gateway.registerStream(std::move(stream), {});
        }
    }
}

int runStandalone(int argc, char **argv)
{
    try
    {
        Config config = configFromEnvironment();
        std::string registry = env("UAV_GATEWAY_CONFIG");
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--help")
            {
                std::cout << "Usage: uav_gateway [--bind ADDRESS] [--port PORT] [--config FILE]\n"
                             "  [--state-file FILE] [--web-root DIRECTORY] [--tls-cert FILE --tls-key FILE]\n"
                             "Set UAV_GATEWAY_TOKEN; optional UAV_GATEWAY_VIEW_TOKEN, UAV_GATEWAY_ORIGIN.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing option value");
            if (arg == "--bind") config.bindAddress = argv[++i];
            else if (arg == "--port") config.port = parsePort(argv[++i]);
            else if (arg == "--config") registry = argv[++i];
            else if (arg == "--state-file") config.stateFile = argv[++i];
            else if (arg == "--web-root") config.webRoot = argv[++i];
            else if (arg == "--tls-cert") config.tlsCertificate = argv[++i];
            else if (arg == "--tls-key") config.tlsPrivateKey = argv[++i];
            else throw std::invalid_argument("unknown gateway option");
        }
        Gateway gateway(std::move(config));
        loadRegistry(gateway, registry);
        stopped = 0;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        gateway.start();
        std::cout << "Gateway listening on port " << gateway.port() << std::endl;
        while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        gateway.stop();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Gateway: " << error.what() << '\n';
        return 1;
    }
}
} // namespace uav::gateway
