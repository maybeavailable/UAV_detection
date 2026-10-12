#pragma once

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uav::gateway {

// 网页 / 自定义 HTTP/WS API 入口。GB28181、ONVIF 由应用独立启动原生协议服务，
// 通过内部设备/媒体接口协作，不依赖 Gateway 的运行状态、Token 或播放 API。
struct Config
{
    std::string bindAddress{"127.0.0.1"};
    std::uint16_t port{8080}; // 0 可用于测试，由系统分配端口
    std::string adminToken;
    std::string viewerToken;
    std::vector<std::string> allowedOrigins; // 留空时仅允许同源浏览器请求
    std::size_t maxConnections{64};
    std::size_t maxPendingCommands{32};
    std::size_t maxDevices{128};
    std::chrono::seconds ticketLifetime{30};
    std::chrono::seconds accessLifetime{900};
    std::chrono::seconds refreshLifetime{28800};
    std::chrono::seconds playbackLifetime{120};
    std::string stateFile; // 空值禁用持久化；独立程序默认 data/gateway-state.json
    std::string webRoot;   // 构建后的网页目录，可为空
    std::string tlsCertificate;
    std::string tlsPrivateKey;
    std::string mediaServiceToken; // 可选：独立媒体进程调用授权校验 API 的凭据
};

struct Device
{
    std::string id;
    std::string type; // camera / radar / spectrum / ptz / laser
    std::string name;
    bool enabled{true};
    boost::json::object config;
    boost::json::object status{{"state", "not_implemented"}};
};

struct PlaybackEndpoint
{
    std::string url;
    bool ready{false};
    boost::json::object parameters; // WebRTC 信令等下层服务参数，不含 Gateway 密钥
};

struct Stream
{
    std::string id;
    std::string deviceId;
    std::string codec{"h264"};
    std::vector<std::string> protocols;
};

enum class CommandCompletion { completed, dispatched };

struct CommandResult
{
    bool success{false};
    std::string code{"backend_error"};
    std::string message;
    boost::json::object data;
    // 仅有发送成功、缺少设备完成确认时必须使用 dispatched。
    CommandCompletion completion{CommandCompletion::completed};
};

// handler 在独立、串行的命令线程执行，须有界返回；不与设备生命周期并发。
// completed 表示适配器能确认操作完成；只写入传输层必须返回 dispatched。
using CommandHandler = std::function<CommandResult(const std::string &, const boost::json::object &)>;
// resolver 必须快速、只读且线程安全。网页/API 播放顺序：解析候选入口 → Gateway 鉴权 → 返回。
// 下层媒体服务仍须校验自己的会话授权；Gateway 的 Bearer Token 不会被放入媒体 URL。
using StreamResolver = std::function<std::optional<PlaybackEndpoint>(const std::string &protocol)>;

class Gateway
{
public:
    explicit Gateway(Config config);
    ~Gateway();
    Gateway(const Gateway &) = delete;
    Gateway &operator=(const Gateway &) = delete;

    // 启动前登记硬件/媒体适配器。运行期间用 updateDeviceStatus/publish 更新数据。
    void registerDevice(Device device, std::vector<std::string> commands = {}, CommandHandler handler = {});
    void registerStream(Stream stream, StreamResolver resolver);
    void updateDeviceStatus(const std::string &id, boost::json::object status);
    // 有界异步投递：队列已满或尚未启动时返回 false。不能在此传递硬件帧指针。
    bool publish(std::string type, boost::json::value data);
    // 媒体适配器在真正接受播放请求时调用；凭据限流 ID/协议且可过期、撤销。
    bool validatePlaybackToken(const std::string &token, const std::string &streamId,
                               const std::string &protocol) const;

    void start(); // 由所有者线程调用；绑定失败/配置错误抛异常；一个实例只启动一次
    void stop();  // 由所有者线程调用，不能在 handler 内调用；取消排队命令并等待在途 handler
    std::uint16_t port() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

// 启动文件只描述设备配置及流目录，不连接或模拟尚未实现的设备。
void loadRegistry(Gateway &gateway, const std::string &path);
Config configFromEnvironment();
int runStandalone(int argc, char **argv);

} // namespace uav::gateway
