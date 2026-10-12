#include "gateway/gateway.h"
#include "gateway/security.h"
#include "gateway/files.h"
#include "gateway_openapi.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <filesystem>
#include <thread>
#include <type_traits>
#include <unordered_set>

namespace uav::gateway {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
namespace json = boost::json;
using tcp = net::ip::tcp;
using Clock = std::chrono::steady_clock;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;
using detail::Role;
using detail::Principal;
using detail::randomId;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
constexpr std::size_t kBodyLimit = 64 * 1024;
constexpr std::size_t kEventLimit = 64 * 1024;
constexpr std::size_t kCommandHistory = 256;

bool identifier(const std::string &value)
{
    return !value.empty() && value.size() <= 64 &&
           value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string::npos;
}

bool deviceType(const std::string &value)
{
    return value == "camera" || value == "radar" || value == "spectrum" || value == "ptz" || value == "laser";
}

bool protocolType(const std::string &value)
{
    return value == "rtsp" || value == "gb28181" || value == "webrtc" || value == "moq";
}

bool playbackUrl(const std::string &url, const std::string &protocol)
{
    if (url.empty() || url.find_first_of("\\\r\n") != std::string::npos || url.find('\0') != std::string::npos) return false;
    if (url[0] == '/') return url.size() == 1 || url[1] != '/';
    const auto separator = url.find("://");
    if (separator == std::string::npos) return false;
    const auto scheme = url.substr(0, separator);
    const auto authority = url.substr(separator + 3, url.find_first_of("/?#", separator + 3) - (separator + 3));
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    if (protocol == "rtsp") return scheme == "rtsp" || scheme == "rtsps";
    if (protocol == "webrtc") return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss";
    return scheme == "https";
}

std::string stringField(const json::object &object, const char *key)
{
    const auto *value = object.if_contains(key);
    if (!value || !value->is_string()) throw std::invalid_argument(std::string("expected string: ") + key);
    const auto &text = value->as_string();
    return std::string(text.data(), text.size());
}

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Reply
{
    unsigned status;
    json::object body;
};

Reply success(json::value value, unsigned status = 200)
{
    return {status, {{"ok", true}, {"data", std::move(value)}}};
}

Reply failure(unsigned status, const std::string &code, const std::string &message)
{
    return {status, {{"ok", false}, {"error", {{"code", code}, {"message", message}}}}};
}

Reply securityReply(detail::SecurityReply reply)
{
    return reply.error.empty() ? success(std::move(reply.data), reply.status) :
        failure(reply.status, reply.error, "authentication or session operation rejected");
}

Response response(const Request &request, Reply reply, bool close = false)
{
    Response result{static_cast<http::status>(reply.status), request.version() ? request.version() : 11};
    result.set(http::field::server, "UAV-Gateway/1");
    result.set(http::field::content_type, "application/json; charset=utf-8");
    result.set(http::field::cache_control, "no-store");
    result.set("X-Content-Type-Options", "nosniff");
    if (reply.status == 401) result.set(http::field::www_authenticate, "Bearer realm=\"uav\"");
    if (reply.status == 405) result.set(http::field::allow, "GET, POST, PUT, DELETE");
    result.keep_alive(!close && request.keep_alive());
    result.body() = json::serialize(reply.body);
    result.prepare_payload();
    return result;
}

json::object parseBody(const Request &request)
{
    auto contentType = request[http::field::content_type];
    auto separator = contentType.find(';');
    if (contentType.substr(0, separator) != "application/json")
        throw std::invalid_argument("Content-Type must be application/json");
    beast::error_code error;
    json::parse_options options;
    options.max_depth = 16;
    auto value = json::parse(request.body(), error, {}, options);
    if (error || !value.is_object()) throw std::invalid_argument("expected a JSON object");
    return std::move(value.as_object());
}

Device parseDevice(const std::string &id, const json::object &body)
{
    Device device;
    device.id = id;
    device.type = stringField(body, "type");
    if (body.if_contains("name")) device.name = stringField(body, "name");
    if (auto value = body.if_contains("enabled"))
    {
        if (!value->is_bool()) throw std::invalid_argument("enabled must be boolean");
        device.enabled = value->as_bool();
    }
    if (auto value = body.if_contains("config"))
    {
        if (!value->is_object()) throw std::invalid_argument("config must be an object");
        device.config = value->as_object();
    }
    if (!identifier(id) || !deviceType(device.type) || device.name.size() > 128 || json::serialize(device.config).size() > 16 * 1024)
        throw std::invalid_argument("invalid device descriptor");
    return device;
}
Stream parseStream(const std::string &id, const json::object &body)
{
    Stream stream;
    stream.id = id;
    stream.deviceId = stringField(body, "device_id");
    if (body.if_contains("codec")) stream.codec = stringField(body, "codec");
    const auto *protocols = body.if_contains("protocols");
    if (!protocols || !protocols->is_array() || protocols->as_array().empty() || protocols->as_array().size() > 4)
        throw std::invalid_argument("protocols must contain 1..4 entries");
    for (const auto &value : protocols->as_array())
    {
        if (!value.is_string()) throw std::invalid_argument("protocol must be a string");
        const std::string protocol(value.as_string().data(), value.as_string().size());
        if (!protocolType(protocol) || std::find(stream.protocols.begin(), stream.protocols.end(), protocol) != stream.protocols.end())
            throw std::invalid_argument("invalid or duplicate protocol");
        stream.protocols.push_back(protocol);
    }
    if (!identifier(id) || !identifier(stream.deviceId) || (stream.codec != "h264" && stream.codec != "h265"))
        throw std::invalid_argument("invalid stream descriptor");
    return stream;
}
} // namespace

struct Gateway::Impl : std::enable_shared_from_this<Gateway::Impl>
{
    struct Connection
    {
        virtual ~Connection() = default;
        virtual void close() = 0;
        virtual void sendEvent(const std::shared_ptr<const std::string> &) {}
    };
    template<class StreamType> class HttpSession;
    template<class StreamType> class WsSession;
    struct DeviceEntry
    {
        Device device;
        std::vector<std::string> commands;
        CommandHandler handler;
    };
    struct StreamEntry { Stream stream; StreamResolver resolver; };
    struct Ticket { Principal principal; Clock::time_point expires; };
    struct Job
    {
        std::string id, deviceId, action;
        json::object parameters;
        CommandHandler handler;
        Clock::time_point deadline;
    };

    explicit Impl(Config value) : config(std::move(value)), security(config), acceptor(io), timer(io) {}
    Config config;
    detail::Security security;
    std::unique_ptr<detail::StateLock> stateLock;
    net::ssl::context tls{net::ssl::context::tls_server};
    net::io_context io;
    tcp::acceptor acceptor;
    net::steady_timer timer;
    std::optional<net::executor_work_guard<net::io_context::executor_type>> guard;
    std::thread networkThread, commandThread, fileThread;
    std::atomic<bool> running{false};
    bool started{false};
    std::uint16_t boundPort{0};
    std::atomic<std::size_t> pendingEvents{0};
    std::atomic<std::uint64_t> droppedEvents{0};
    std::unordered_set<std::shared_ptr<Connection>> connections; // 仅网络线程访问
    std::map<std::string, Ticket> tickets;
    mutable std::mutex mutex;
    std::condition_variable commandReady;
    std::map<std::string, DeviceEntry> devices;
    std::map<std::string, StreamEntry> streams;
    std::map<std::string, json::object> commands;
    std::deque<std::string> commandOrder;
    std::deque<Job> jobs;
    std::size_t pendingCommands{0};
    bool commandsStopping{false};
    std::mutex fileMutex;
    std::condition_variable fileReady;
    std::deque<std::function<void()>> fileJobs;
    bool filesStopping{false};
    std::atomic<std::uint64_t> requests{0}, errors{0};
    std::deque<json::object> audit;

    Principal authenticate(const Request &request) const
    {
        return security.authenticate(std::string(request[http::field::authorization]));
    }
    Role authorize(const Request &request) const { return authenticate(request).role; }

    void recordAudit(const Principal &principal, const std::string &operation, const std::string &target, unsigned status)
    {
        // 仅记录身份、操作和结果，不记录凭据、请求正文或完整 URL 查询参数。
        std::lock_guard<std::mutex> lock(mutex);
        if (audit.size() >= 256) audit.pop_front();
        audit.push_back({{"time_ms", nowMs()}, {"actor", principal.owner}, {"operation", operation},
            {"target", target}, {"status", status}});
    }

    bool postFile(std::function<void()> job)
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (filesStopping || fileJobs.size() >= 16) return false;
        fileJobs.push_back(std::move(job));
        fileReady.notify_one();
        return true;
    }
    void fileLoop()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(fileMutex);
                fileReady.wait(lock, [&] { return filesStopping || !fileJobs.empty(); });
                if (fileJobs.empty() && filesStopping) return;
                job = std::move(fileJobs.front()); fileJobs.pop_front();
            }
            job(); // 工作函数自行转换异常为 HTTP 错误
        }
    }

    bool originAllowed(const Request &request) const
    {
        const auto origin = request[http::field::origin];
        if (origin.empty()) return true; // 非浏览器客户端仍须通过 Bearer 鉴权
        const std::string text(origin);
        if (!config.allowedOrigins.empty())
            return std::find(config.allowedOrigins.begin(), config.allowedOrigins.end(), text) != config.allowedOrigins.end();
        const std::string host(request[http::field::host]);
        return !host.empty() && (text == "http://" + host || text == "https://" + host);
    }

    json::object deviceJson(const DeviceEntry &entry, bool includeConfig) const
    {
        const auto &device = entry.device;
        json::array actions;
        for (const auto &action : entry.commands) actions.emplace_back(action);
        json::object value{{"id", device.id}, {"type", device.type}, {"name", device.name},
                           {"enabled", device.enabled}, {"status", device.status},
                           {"commands", std::move(actions)}, {"managed", bool(entry.handler)}};
        if (includeConfig) value["config"] = device.config;
        return value;
    }

    json::object snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        json::array values;
        for (const auto &item : devices) values.push_back(deviceJson(item.second, false));
        return {{"time_ms", nowMs()}, {"devices", std::move(values)},
                {"pending_commands", pendingCommands}, {"dropped_events", droppedEvents.load()},
                {"requests", requests.load()}, {"errors", errors.load()}, {"persistence", !config.stateFile.empty()},
                {"tls", !config.tlsCertificate.empty()}};
    }

    bool publish(std::string type, json::value data)
    {
        // 与 stop 的投递屏障共用锁，禁止关闭网络循环后再投递持有 Impl 的任务。
        std::lock_guard<std::mutex> lock(mutex);
        if (!running.load() || !identifier(type)) return false;
        if (pendingEvents.fetch_add(1) >= 128)
        {
            --pendingEvents;
            ++droppedEvents;
            return false;
        }
        try
        {
            auto text = std::make_shared<const std::string>(json::serialize(
                json::object{{"type", std::move(type)}, {"time_ms", nowMs()}, {"data", std::move(data)}}));
            if (text->size() > kEventLimit)
            {
                --pendingEvents;
                ++droppedEvents;
                return false;
            }
            net::post(io, [self = this->shared_from_this(), text] {
                --self->pendingEvents;
                if (self->running.load()) self->broadcast(text);
            });
            return true;
        }
        catch (...)
        {
            --pendingEvents;
            throw;
        }
    }

    void broadcast(const std::shared_ptr<const std::string> &text)
    {
        // sendEvent 可能关闭并移除慢连接，因此遍历快照。
        std::vector<std::shared_ptr<Connection>> copy(connections.begin(), connections.end());
        for (auto &connection : copy) connection->sendEvent(text);
    }

    void tick()
    {
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait([self = this->shared_from_this()](beast::error_code error) {
            if (error || !self->running.load()) return;
            auto text = std::make_shared<const std::string>(json::serialize(
                json::object{{"type", "snapshot"}, {"data", self->snapshot()}}));
            self->broadcast(text);
            self->expireTickets();
            self->security.expire();
            self->tick();
        });
    }

    void expireTickets()
    {
        const auto now = Clock::now();
        for (auto it = tickets.begin(); it != tickets.end();)
            if (it->second.expires <= now) it = tickets.erase(it); else ++it;
    }

    Principal consumeTicket(const std::string &target)
    {
        constexpr const char *prefix = "/api/v1/events?ticket=";
        if (target.rfind(prefix, 0) != 0) return {};
        auto found = tickets.find(target.substr(std::char_traits<char>::length(prefix)));
        if (found == tickets.end()) return {};
        const auto value = found->second;
        tickets.erase(found); // 无论过期与否都只可用一次
        return value.expires > Clock::now() && security.ownerAlive(value.principal.owner) ? value.principal : Principal{};
    }

    void commandLoop()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                commandReady.wait(lock, [&] { return commandsStopping || !jobs.empty(); });
                if (commandsStopping) return;
                job = std::move(jobs.front());
                jobs.pop_front();
                commands.at(job.id)["state"] = "running";
            }
            CommandResult result;
            std::string state;
            if (Clock::now() >= job.deadline)
            {
                state = "expired";
                result = {false, "command_expired", "command expired before execution", {}};
            }
            else
            {
                try { result = job.handler(job.action, job.parameters); }
                catch (...) { result = {false, "backend_exception", "device handler failed", {}}; }
                if (result.code.size() > 64 || result.message.size() > 1024 || json::serialize(result.data).size() > kBodyLimit)
                    result = {false, "backend_result_too_large", "device result exceeds gateway limits", {}};
                state = !result.success ? "failed" :
                    result.completion == CommandCompletion::dispatched ? "dispatched" : "succeeded";
            }
            json::object record;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto &entry = commands.at(job.id);
                entry["state"] = state;
                entry["finished_ms"] = nowMs();
                entry["result"] = json::object{{"success", result.success}, {"code", result.code},
                    {"message", result.message}, {"data", std::move(result.data)}};
                record = entry;
                --pendingCommands;
            }
            publish("command.result", std::move(record));
        }
    }

    Reply enqueue(const std::string &deviceId, const json::object &body)
    {
        const auto action = stringField(body, "action");
        json::object parameters;
        if (auto value = body.if_contains("parameters"))
        {
            if (!value->is_object()) throw std::invalid_argument("parameters must be an object");
            parameters = value->as_object();
        }
        std::int64_t ttl = 2000;
        if (auto value = body.if_contains("ttl_ms"))
        {
            if (!value->is_int64()) throw std::invalid_argument("ttl_ms must be an integer");
            ttl = value->as_int64();
        }
        if (!identifier(action) || ttl < 100 || ttl > 10000)
            throw std::invalid_argument("invalid action or ttl_ms (100..10000)");
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = devices.find(deviceId);
        if (found == devices.end()) return failure(404, "device_not_found", "unknown device");
        const auto &entry = found->second;
        if (!entry.device.enabled) return failure(409, "device_disabled", "device is disabled");
        if (!entry.handler) return failure(503, "backend_unavailable", "device adapter is not attached");
        if (std::find(entry.commands.begin(), entry.commands.end(), action) == entry.commands.end())
            return failure(422, "unsupported_action", "action is not supported by this device");
        if (commandsStopping || !running.load()) return failure(503, "stopping", "gateway is stopping");
        if (pendingCommands >= config.maxPendingCommands) return failure(429, "command_queue_full", "command queue is full");
        while (commands.size() >= kCommandHistory)
        {
            const auto removable = std::find_if(commandOrder.begin(), commandOrder.end(), [&](const auto &id) {
                const auto &state = commands.at(id).at("state").as_string();
                return state != "queued" && state != "running";
            });
            if (removable == commandOrder.end()) return failure(429, "command_history_full", "too many active commands");
            commands.erase(*removable);
            commandOrder.erase(removable);
        }
        const auto id = randomId();
        json::object record{{"id", id}, {"device_id", deviceId}, {"action", action},
                            {"state", "queued"}, {"created_ms", nowMs()}, {"ttl_ms", ttl}};
        commands.emplace(id, record);
        commandOrder.push_back(id);
        jobs.push_back({id, deviceId, action, std::move(parameters), entry.handler,
                        Clock::now() + std::chrono::milliseconds(ttl)});
        ++pendingCommands;
        commandReady.notify_one();
        return success(std::move(record), 202);
    }

    Reply route(const Request &request, const std::string &peer)
    {
        const std::string path(request.target());
        const auto method = request.method();
        if (path == "/healthz" && method == http::verb::get)
            return success(json::object{{"service", "gateway"}, {"running", running.load()}});
        if (path == "/readyz" && method == http::verb::get)
            return success(json::object{{"ready", running.load()}, {"scope", "gateway"}}, running.load() ? 200 : 503);
        if (path == "/" && method == http::verb::get)
            return success(json::object{{"service", "UAV Gateway"}, {"api", "/api/v1"}, {"events", "/api/v1/events"}});
        if (path == "/api/v1/auth/login" && method == http::verb::post)
        {
            auto result = securityReply(security.login(stringField(parseBody(request), "token"), peer));
            recordAudit({}, "auth.login", "session", result.status);
            return result;
        }
        if (path == "/api/v1/auth/refresh" && method == http::verb::post)
            return securityReply(security.refresh(stringField(parseBody(request), "refresh_token"), peer));
        if (path == "/api/v1/media/authorize" && method == http::verb::post)
        {
            if (config.mediaServiceToken.empty()) return failure(503, "media_auth_disabled", "media service credential is not configured");
            if (!detail::tokenEqual(std::string(request[http::field::authorization]), "Bearer " + config.mediaServiceToken))
                return failure(401, "unauthorized", "media service credential required");
            const auto body = parseBody(request);
            const bool allowed = validateMedia(stringField(body, "token"), stringField(body, "stream_id"), stringField(body, "protocol"));
            return allowed ? success(json::object{{"authorized", true}}) : failure(403, "media_token_invalid", "playback permission is invalid or expired");
        }

        // 这里的候选地址解析严格先于鉴权；未授权请求不会得到地址或流是否存在的信息。
        if (path == "/api/v1/playback" && method == http::verb::post)
        {
            const auto body = parseBody(request);
            const auto id = stringField(body, "stream_id");
            const auto protocol = stringField(body, "protocol");
            if (!identifier(id) || !protocolType(protocol)) throw std::invalid_argument("invalid stream or protocol");
            if (protocol == "gb28181") return failure(400, "native_protocol", "GB28181 uses its independent SIP service");
            StreamResolver resolver;
            bool exists = false;
            bool enabled = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto found = streams.find(id);
                if (found != streams.end())
                {
                    const auto &entry = found->second;
                    const auto device = devices.find(entry.stream.deviceId);
                    enabled = device != devices.end() && device->second.device.enabled;
                    exists = std::find(entry.stream.protocols.begin(), entry.stream.protocols.end(), protocol) != entry.stream.protocols.end();
                    if (exists) resolver = entry.resolver;
                }
            }
            std::optional<PlaybackEndpoint> endpoint;
            bool failed = false;
            try { if (resolver) endpoint = resolver(protocol); } catch (...) { failed = true; }
            const auto principal = authenticate(request);
            if (!principal) return failure(401, "unauthorized", "valid Bearer token required");
            if (!exists) return failure(404, "stream_not_found", "stream or protocol is not registered");
            if (!enabled) return failure(409, "device_disabled", "stream device is disabled");
            if (failed || !endpoint || !endpoint->ready || endpoint->url.empty())
                return failure(503, "stream_unavailable", "media service is not ready");
            if (endpoint->url.size() > 2048 || !playbackUrl(endpoint->url, protocol) || json::serialize(endpoint->parameters).size() > kBodyLimit)
                return failure(502, "invalid_media_response", "media endpoint exceeds gateway limits");
            auto grant = security.grant(principal, id, protocol);
            if (!grant.error.empty()) return securityReply(std::move(grant));
            grant.data["url"] = endpoint->url;
            grant.data["parameters"] = std::move(endpoint->parameters);
            recordAudit(principal, "playback.create", id, 200);
            return success(std::move(grant.data));
        }

        const auto principal = authenticate(request);
        const auto role = principal.role;
        if (role == Role::none) return failure(401, "unauthorized", "valid Bearer token required");
        if (method != http::verb::get && method != http::verb::post && method != http::verb::put && method != http::verb::delete_)
            return failure(405, "method_not_allowed", "method is not supported");
        if (path == "/api/v1/auth/me" && method == http::verb::get)
            return success(json::object{{"role", role == Role::admin ? "admin" : "viewer"}, {"session_id", principal.owner}});
        if (path == "/api/v1/auth/logout" && method == http::verb::post)
        {
            if (principal.owner.rfind("root.", 0) == 0)
                return failure(400, "login_session_required", "provisioned API credentials cannot be revoked by logout; login first");
            security.logout(principal);
            recordAudit(principal, "auth.logout", "session", 200);
            return success(json::object{{"logged_out", true}});
        }
        constexpr const char *playbackPrefix = "/api/v1/playback/";
        if (path.rfind(playbackPrefix, 0) == 0)
        {
            auto id = path.substr(std::char_traits<char>::length(playbackPrefix));
            const bool renew = id.size() > 6 && id.substr(id.size() - 6) == "/renew";
            if (renew) id.resize(id.size() - 6);
            if (!identifier(id)) throw std::invalid_argument("invalid playback session id");
            if ((renew && method != http::verb::post) || (!renew && method != http::verb::get && method != http::verb::delete_))
                return failure(405, "method_not_allowed", "unsupported playback operation");
            auto result = securityReply(security.playback(principal, id, method == http::verb::delete_, renew));
            if (method != http::verb::get) recordAudit(principal, renew ? "playback.renew" : "playback.delete", id, result.status);
            return result;
        }
        if (method != http::verb::get && path != "/api/v1/ws-ticket" && role != Role::admin)
            return failure(403, "forbidden", "admin token required for changes");

        if (path == "/api/v1/openapi.json" && method == http::verb::get)
            return success(json::parse(detail::openapi));
        if (path == "/api/v1/audit" && method == http::verb::get)
        {
            if (role != Role::admin) return failure(403, "forbidden", "admin token required");
            std::lock_guard<std::mutex> lock(mutex);
            json::array entries;
            for (const auto &entry : audit) entries.push_back(entry);
            return success(std::move(entries));
        }
        if (path == "/api/v1/status" && method == http::verb::get) return success(snapshot());
        if (path == "/api/v1/commands" && method == http::verb::get)
        {
            std::lock_guard<std::mutex> lock(mutex);
            json::array result;
            for (auto it = commandOrder.rbegin(); it != commandOrder.rend() && result.size() < 32; ++it)
                result.push_back(commands.at(*it));
            return success(std::move(result));
        }
        if (path == "/api/v1/devices" && method == http::verb::get)
        {
            std::lock_guard<std::mutex> lock(mutex);
            json::array result;
            for (const auto &entry : devices) result.push_back(deviceJson(entry.second, role == Role::admin));
            return success(std::move(result));
        }
        if (path == "/api/v1/streams" && method == http::verb::get)
        {
            std::lock_guard<std::mutex> lock(mutex);
            json::array result;
            for (const auto &item : streams)
            {
                const auto &entry = item.second;
                json::array protocols;
                for (const auto &value : entry.stream.protocols) protocols.emplace_back(value);
                result.push_back(json::object{{"id", entry.stream.id}, {"device_id", entry.stream.deviceId},
                    {"codec", entry.stream.codec}, {"protocols", std::move(protocols)}, {"backend_attached", bool(entry.resolver)}});
            }
            return success(std::move(result));
        }
        if (path.rfind("/api/v1/streams/", 0) == 0 && method == http::verb::get)
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = streams.find(path.substr(16));
            if (found == streams.end()) return failure(404, "stream_not_found", "unknown stream");
            json::array protocols;
            for (const auto &protocol : found->second.stream.protocols) protocols.emplace_back(protocol);
            return success(json::object{{"id", found->first}, {"device_id", found->second.stream.deviceId},
                {"codec", found->second.stream.codec}, {"protocols", std::move(protocols)}, {"backend_attached", bool(found->second.resolver)}});
        }
        if (path == "/api/v1/ws-ticket" && method == http::verb::post)
        {
            expireTickets();
            if (tickets.size() >= 128) return failure(429, "ticket_limit", "too many pending WebSocket tickets");
            const auto id = randomId();
            tickets.emplace(id, Ticket{principal, Clock::now() + config.ticketLifetime});
            return success(json::object{{"url", "/api/v1/events?ticket=" + id},
                {"expires_in_seconds", config.ticketLifetime.count()}}, 201);
        }
        constexpr const char *commandPrefix = "/api/v1/commands/";
        if (path.rfind(commandPrefix, 0) == 0 && method == http::verb::delete_)
        {
            const auto id = path.substr(std::char_traits<char>::length(commandPrefix));
            json::object record;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto found = commands.find(id);
                if (found == commands.end()) return failure(404, "command_not_found", "unknown command");
                if (found->second.at("state") != "queued") return failure(409, "command_not_queued", "only queued commands can be cancelled");
                const auto job = std::find_if(jobs.begin(), jobs.end(), [&](const Job &value) { return value.id == id; });
                if (job == jobs.end()) return failure(409, "command_not_queued", "command has started");
                jobs.erase(job); --pendingCommands;
                found->second["state"] = "cancelled"; found->second["finished_ms"] = nowMs();
                record = found->second;
            }
            publish("command.result", record);
            recordAudit(principal, "command.cancel", id, 200);
            return success(std::move(record));
        }
        if (path.rfind(commandPrefix, 0) == 0 && method == http::verb::get)
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = commands.find(path.substr(std::char_traits<char>::length(commandPrefix)));
            return found != commands.end() ? success(found->second) : failure(404, "command_not_found", "unknown command");
        }
        constexpr const char *devicePrefix = "/api/v1/devices/";
        if (path.rfind(devicePrefix, 0) == 0)
        {
            const auto suffix = path.substr(std::char_traits<char>::length(devicePrefix));
            const auto slash = suffix.find('/');
            const auto id = suffix.substr(0, slash);
            if (!identifier(id)) throw std::invalid_argument("invalid device id");
            if (slash != std::string::npos)
            {
                if (suffix.substr(slash) == "/commands" && method == http::verb::post)
                {
                    auto result = enqueue(id, parseBody(request));
                    recordAudit(principal, "command.submit", id, result.status);
                    if (result.status == 202) publish("command.accepted", result.body.at("data"));
                    return result;
                }
                return failure(404, "not_found", "unknown resource");
            }
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = devices.find(id);
            if (found == devices.end()) return failure(404, "device_not_found", "unknown device");
            if (method == http::verb::get) return success(deviceJson(found->second, role == Role::admin));
            return failure(405, "method_not_allowed", "method is not supported for device");
        }
        if (path == "/api/v1/events") return failure(426, "upgrade_required", "use WebSocket with a one-time ticket");
        return failure(404, "not_found", "unknown resource");
    }

    void accept();
    void start();
    void stop();
    Reply configure(const Request &request);
    void restoreState();
    json::object registryJson(const std::map<std::string, DeviceEntry> &, const std::map<std::string, StreamEntry> &) const;
    bool validateMedia(const std::string &token, const std::string &id, const std::string &protocol) const
    {
        if (!running.load()) return false;
        { std::lock_guard<std::mutex> lock(mutex);
          auto found = streams.find(id);
          if (found == streams.end()) return false;
          auto device = devices.find(found->second.stream.deviceId);
          if (device == devices.end() || !device->second.device.enabled) return false;
        }
        return security.validate(token, id, protocol);
    }
};

json::object Gateway::Impl::registryJson(const std::map<std::string, DeviceEntry> &deviceValues,
                                        const std::map<std::string, StreamEntry> &streamValues) const
{
    json::array deviceList, streamList;
    for (const auto &entry : deviceValues)
    {
        const auto &device = entry.second.device;
        deviceList.push_back(json::object{{"id", device.id}, {"type", device.type}, {"name", device.name},
            {"enabled", device.enabled}, {"config", device.config}});
    }
    for (const auto &entry : streamValues)
    {
        const auto &stream = entry.second.stream;
        json::array protocols;
        for (const auto &protocol : stream.protocols) protocols.emplace_back(protocol);
        streamList.push_back(json::object{{"id", stream.id}, {"device_id", stream.deviceId},
            {"codec", stream.codec}, {"protocols", std::move(protocols)}});
    }
    return {{"version", 1}, {"devices", std::move(deviceList)}, {"streams", std::move(streamList)}};
}

void Gateway::Impl::restoreState()
{
    if (config.stateFile.empty() || !std::filesystem::exists(config.stateFile)) return;
    const auto root = detail::readRegistryFile(config.stateFile);
    if (!root.if_contains("version") || root.at("version") != 1)
        throw std::invalid_argument("unsupported gateway state version");
    std::map<std::string, DeviceEntry> restoredDevices;
    std::map<std::string, StreamEntry> restoredStreams;
    for (const auto &value : root.at("devices").as_array())
    {
        const auto &body = value.as_object();
        auto device = parseDevice(stringField(body, "id"), body);
        auto id = device.id;
        if (!restoredDevices.emplace(id, DeviceEntry{std::move(device), {}, {}}).second)
            throw std::invalid_argument("duplicate saved device id");
    }
    for (const auto &entry : devices)
        if (entry.second.handler) restoredDevices[entry.first] = entry.second;
    for (const auto &value : root.at("streams").as_array())
    {
        const auto &body = value.as_object();
        auto stream = parseStream(stringField(body, "id"), body);
        auto id = stream.id;
        if (!restoredStreams.emplace(id, StreamEntry{std::move(stream), {}}).second)
            throw std::invalid_argument("duplicate saved stream id");
    }
    for (const auto &entry : streams)
        if (entry.second.resolver)
        {
            restoredStreams[entry.first] = entry.second;
            restoredDevices[entry.second.stream.deviceId] = devices.at(entry.second.stream.deviceId);
        }
    if (restoredDevices.size() > config.maxDevices || restoredStreams.size() > 128)
        throw std::invalid_argument("saved registry exceeds resource limits");
    for (const auto &entry : restoredStreams)
        if (!restoredDevices.count(entry.second.stream.deviceId)) throw std::invalid_argument("saved stream references unknown device");
    devices = std::move(restoredDevices); streams = std::move(restoredStreams);
}

Reply Gateway::Impl::configure(const Request &request)
{
    const auto principal = authenticate(request);
    if (!principal) return failure(401, "unauthorized", "valid Bearer token required");
    if (principal.role != Role::admin) return failure(403, "forbidden", "admin token required");
    if (!running.load()) return failure(503, "stopping", "gateway is stopping");
    const std::string path(request.target());
    const bool isDevice = path.rfind("/api/v1/devices/", 0) == 0;
    const auto id = path.substr(16); // devices / streams 目录前缀均为 16 字节
    if (!identifier(id)) throw std::invalid_argument("invalid resource id");
    const bool remove = request.method() == http::verb::delete_;
    std::map<std::string, DeviceEntry> candidateDevices;
    std::map<std::string, StreamEntry> candidateStreams;
    { std::lock_guard<std::mutex> lock(mutex); candidateDevices = devices; candidateStreams = streams; }
    json::object result;
    bool created = false;
    if (isDevice)
    {
        auto found = candidateDevices.find(id);
        bool managed = found != candidateDevices.end() && bool(found->second.handler);
        for (const auto &stream : candidateStreams)
            if (stream.second.stream.deviceId == id && stream.second.resolver) managed = true;
        if (managed) return failure(409, "managed_device", "attached adapter is managed by the application");
        if (remove)
        {
            if (found == candidateDevices.end()) return failure(404, "device_not_found", "unknown device");
            for (const auto &stream : candidateStreams)
                if (stream.second.stream.deviceId == id) return failure(409, "device_in_use", "device has streams");
            candidateDevices.erase(found); result = {{"deleted", id}};
        }
        else
        {
            created = found == candidateDevices.end();
            if (created && candidateDevices.size() >= config.maxDevices) return failure(429, "device_limit", "device registry is full");
            candidateDevices[id] = {parseDevice(id, parseBody(request)), {}, {}};
            result = deviceJson(candidateDevices.at(id), true);
        }
    }
    else
    {
        auto found = candidateStreams.find(id);
        if (found != candidateStreams.end() && found->second.resolver)
            return failure(409, "managed_stream", "attached media adapter is managed by the application");
        if (remove)
        {
            if (found == candidateStreams.end()) return failure(404, "stream_not_found", "unknown stream");
            candidateStreams.erase(found); result = {{"deleted", id}};
        }
        else
        {
            auto stream = parseStream(id, parseBody(request));
            if (!candidateDevices.count(stream.deviceId)) return failure(400, "unknown_device", "stream device is not registered");
            created = found == candidateStreams.end();
            if (created && candidateStreams.size() >= 128) return failure(429, "stream_limit", "stream registry is full");
            candidateStreams[id] = {std::move(stream), {}};
            result = {{"id", id}, {"backend_attached", false}};
        }
    }
    try { detail::writeRegistryFile(config.stateFile, registryJson(candidateDevices, candidateStreams)); }
    catch (...) { return failure(503, "persistence_failed", "configuration was not committed; state storage is unavailable"); }
    { // 文件线程串行处理配置；只提交目标条目，保留其他线程刚更新的设备状态。
        std::lock_guard<std::mutex> lock(mutex);
        if (isDevice) { if (remove) devices.erase(id); else devices[id] = std::move(candidateDevices.at(id)); }
        else { if (remove) streams.erase(id); else streams[id] = std::move(candidateStreams.at(id)); }
    }
    const unsigned status = created ? 201 : 200;
    publish("registry.changed", json::object{{"kind", isDevice ? "device" : "stream"}, {"id", id}, {"deleted", remove}});
    return success(std::move(result), status);
}

template<class StreamType>
class Gateway::Impl::WsSession : public Connection, public std::enable_shared_from_this<WsSession<StreamType>>
{
public:
    WsSession(StreamType stream, std::weak_ptr<Impl> server, std::string principalOwner)
        : socket(std::move(stream)), owner(std::move(server)), sessionOwner(std::move(principalOwner)) {}

    void run(Request request)
    {
        handshake = std::move(request);
        beast::get_lowest_layer(socket).expires_never();
        socket.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
        socket.read_message_max(8192);
        socket.async_accept(handshake, [self = this->shared_from_this()](beast::error_code error) {
            if (error) { self->close(); return; }
            self->ready = true;
            if (auto server = self->owner.lock())
                self->sendEvent(std::make_shared<const std::string>(json::serialize(
                    json::object{{"type", "snapshot"}, {"data", server->snapshot()}})));
            self->read();
        });
    }

    void sendEvent(const std::shared_ptr<const std::string> &text) override
    {
        if (!ready || closed) return;
        if (auto server = owner.lock(); !server || !server->security.ownerAlive(sessionOwner)) { close(); return; }
        if (queue.size() >= 32 || queuedBytes + text->size() > 1024 * 1024)
        {
            close(); // 每个会话独立断开，禁止慢客户端积压整个进程
            return;
        }
        queuedBytes += text->size();
        queue.push_back(text);
        if (queue.size() == 1) write();
    }

    void close() override
    {
        if (closed) return;
        closed = true;
        beast::error_code ignored;
        auto &tcpSocket = beast::get_lowest_layer(socket).socket();
        tcpSocket.cancel(ignored);
        tcpSocket.shutdown(tcp::socket::shutdown_both, ignored);
        tcpSocket.close(ignored);
        if (auto server = owner.lock()) server->connections.erase(this->shared_from_this());
    }

private:
    void read()
    {
        socket.async_read(buffer, [self = this->shared_from_this()](beast::error_code error, std::size_t) {
            if (error || !self->socket.got_text()) { self->close(); return; }
            beast::error_code parseError;
            auto message = json::parse(beast::buffers_to_string(self->buffer.data()), parseError);
            self->buffer.consume(self->buffer.size());
            if (parseError || !message.is_object() || !message.as_object().if_contains("type") ||
                message.as_object().at("type") != "ping")
                self->sendEvent(std::make_shared<const std::string>(json::serialize(
                    json::object{{"type", "error"}, {"code", "unsupported_message"}, {"message", "send {type:ping}; commands use HTTP API"}})));
            else self->sendEvent(std::make_shared<const std::string>("{\"type\":\"pong\"}"));
            if (!self->closed) self->read();
        });
    }

    void write()
    {
        socket.text(true);
        auto text = queue.front();
        socket.async_write(net::buffer(*text), [self = this->shared_from_this(), text](beast::error_code error, std::size_t) {
            if (error) { self->close(); return; }
            self->queuedBytes -= text->size();
            self->queue.pop_front();
            if (!self->closed && !self->queue.empty()) self->write();
        });
    }

    ws::stream<StreamType> socket;
    std::weak_ptr<Impl> owner;
    std::string sessionOwner;
    Request handshake;
    beast::flat_buffer buffer;
    std::deque<std::shared_ptr<const std::string>> queue;
    std::size_t queuedBytes{0};
    bool ready{false}, closed{false};
};

template<class StreamType>
class Gateway::Impl::HttpSession : public Connection, public std::enable_shared_from_this<HttpSession<StreamType>>
{
public:
    HttpSession(StreamType value, std::weak_ptr<Impl> server, std::string address)
        : stream(std::move(value)), owner(std::move(server)), peer(std::move(address)) {}
    void run()
    {
        if constexpr (std::is_same_v<StreamType, TlsStream>)
        {
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
            stream.async_handshake(net::ssl::stream_base::server,
                [self = this->shared_from_this()](beast::error_code error) {
                    if (error) self->close(); else self->read();
                });
        }
        else read();
    }
    void close() override
    {
        if (closed) return;
        closed = true;
        beast::error_code ignored;
        auto &socket = beast::get_lowest_layer(stream).socket();
        socket.cancel(ignored);
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
        if (auto server = owner.lock()) server->connections.erase(this->shared_from_this());
    }

private:
    void read()
    {
        parser.emplace();
        requestId = randomId();
        requestOrigin.clear();
        parser->body_limit(kBodyLimit);
        parser->header_limit(8192);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
        http::async_read(stream, buffer, *parser, [self = this->shared_from_this()](beast::error_code error, std::size_t) {
            self->onRead(error);
        });
    }

    void onRead(beast::error_code error)
    {
        if (closed) return;
        if (error)
        {
            if (error == http::error::body_limit)
                send(response(Request{}, failure(413, "body_too_large", "body exceeds 64 KiB"), true));
            else if (error == http::error::header_limit)
                send(response(Request{}, failure(431, "headers_too_large", "headers exceed 8 KiB"), true));
            else if (error == http::error::end_of_stream || error == net::error::operation_aborted || error == beast::error::timeout)
                close();
            else send(response(Request{}, failure(400, "bad_request", "invalid HTTP request"), true));
            return;
        }
        auto server = owner.lock();
        if (!server || !server->running.load()) { close(); return; }
        auto request = parser->release();
        ++server->requests;
        if (request.target().size() > 2048 || request.target().empty() || request.target()[0] != '/')
        { send(response(request, failure(400, "bad_target", "invalid request target"))); return; }
        if (!server->originAllowed(request))
        { send(response(request, failure(403, "origin_forbidden", "browser origin is not allowed"))); return; }
        requestOrigin = std::string(request[http::field::origin]);
        if (request.method() == http::verb::options)
        {
            Response reply{http::status::no_content, request.version()};
            reply.keep_alive(request.keep_alive());
            reply.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            reply.set("Access-Control-Allow-Headers", "Authorization, Content-Type, X-Request-ID");
            reply.set("Access-Control-Max-Age", "600");
            send(std::move(reply)); return;
        }
        if (ws::is_upgrade(request))
        {
            const std::string target(request.target());
            if (target.rfind("/api/v1/events?ticket=", 0) != 0)
            { send(response(request, failure(404, "not_found", "unknown WebSocket endpoint"))); return; }
            if (buffer.size() != 0)
            { send(response(request, failure(400, "bad_upgrade", "unexpected data before WebSocket upgrade"), true)); return; }
            const auto principal = server->consumeTicket(target);
            if (!principal)
            { send(response(request, failure(401, "invalid_ticket", "valid one-time WebSocket ticket required"))); return; }
            auto session = std::make_shared<WsSession<StreamType>>(std::move(stream), server, principal.owner);
            server->connections.erase(this->shared_from_this());
            closed = true; // stream 已转移，不能再关闭原 socket
            server->connections.insert(session);
            session->run(std::move(request));
            return;
        }
        const std::string target(request.target());
        const bool configure = (request.method() == http::verb::put || request.method() == http::verb::delete_) &&
            (target.rfind("/api/v1/devices/", 0) == 0 || target.rfind("/api/v1/streams/", 0) == 0);
        const bool asset = !server->config.webRoot.empty() && target.rfind("/api/", 0) != 0 &&
            target != "/healthz" && target != "/readyz" && target != "/openapi.json";
        if (configure || asset)
        {
            auto self = this->shared_from_this();
            auto input = std::make_shared<Request>(std::move(request));
            if (!server->postFile([self, server, input, configure] {
                Response reply;
                const auto actor = server->authenticate(*input);
                try
                {
                    if (configure) reply = response(*input, server->configure(*input));
                    else if (input->method() != http::verb::get && input->method() != http::verb::head)
                        reply = response(*input, failure(405, "method_not_allowed", "assets require GET or HEAD"));
                    else
                    {
                        auto file = detail::readAsset(server->config.webRoot, std::string(input->target()),
                            (*input)[http::field::accept].find("text/html") != beast::string_view::npos);
                        reply = Response{static_cast<http::status>(file.status), input->version()};
                        reply.keep_alive(input->keep_alive());
                        reply.set(http::field::content_type, file.contentType);
                        reply.set(http::field::cache_control, "no-cache");
                        if (!file.etag.empty()) reply.set(http::field::etag, file.etag);
                        const std::string condition((*input)[http::field::if_none_match]);
                        bool cached = false;
                        std::size_t begin = 0;
                        while (begin < condition.size())
                        {
                            auto end = condition.find(',', begin);
                            auto value = condition.substr(begin, end == std::string::npos ? end : end - begin);
                            auto first = value.find_first_not_of(" \t");
                            if (first != std::string::npos) value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
                            if (value.rfind("W/", 0) == 0) value.erase(0, 2);
                            if (file.status == 200 && (value == file.etag || value == "*")) cached = true;
                            if (end == std::string::npos) break;
                            begin = end + 1;
                        }
                        if (cached) reply.result(http::status::not_modified);
                        else
                        {
                            reply.content_length(file.body.size());
                            if (input->method() != http::verb::head) reply.body() = std::move(file.body);
                        }
                        reply.set("Content-Security-Policy", "frame-ancestors 'none'; base-uri 'self'; object-src 'none'");
                    }
                }
                catch (const std::invalid_argument &error) { reply = response(*input, failure(400, "invalid_request", error.what())); }
                catch (...) { reply = response(*input, failure(500, "file_error", "gateway file operation failed")); }
                if (configure)
                {
                    std::string target(input->target()); target.resize(target.find('?') == std::string::npos ? target.size() : target.find('?'));
                    server->recordAudit(actor, input->method() == http::verb::put ? "config.put" : "config.delete", target, reply.result_int());
                }
                net::post(server->io, [self, reply = std::move(reply)]() mutable { if (!self->closed) self->send(std::move(reply)); });
            })) send(response(*input, failure(429, "file_queue_full", "file operation queue is full")));
            return;
        }
        if (target == "/openapi.json" && request.method() == http::verb::get)
        {
            Response reply{http::status::ok, request.version()};
            reply.keep_alive(request.keep_alive());
            reply.set(http::field::content_type, "application/json");
            reply.body() = detail::openapi; reply.prepare_payload(); send(std::move(reply)); return;
        }
        try { send(response(request, server->route(request, peer))); }
        catch (const std::invalid_argument &error)
        { send(response(request, failure(400, "invalid_request", error.what()))); }
        catch (...)
        { send(response(request, failure(500, "internal_error", "gateway could not process request"))); }
    }

    void send(Response value)
    {
        if (auto server = owner.lock(); server && value.result_int() >= 400) ++server->errors;
        value.set("X-Request-ID", requestId);
        value.set("X-Content-Type-Options", "nosniff");
        value.set("Referrer-Policy", "no-referrer");
        if (!requestOrigin.empty())
        {
            value.set("Access-Control-Allow-Origin", requestOrigin);
            value.set("Access-Control-Expose-Headers", "X-Request-ID");
            value.set(http::field::vary, "Origin");
        }
        auto message = std::make_shared<Response>(std::move(value));
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
        http::async_write(stream, *message, [self = this->shared_from_this(), message](beast::error_code error, std::size_t) {
            if (error || !message->keep_alive()) { self->close(); return; }
            self->read();
        });
    }

    StreamType stream;
    std::weak_ptr<Impl> owner;
    std::string peer, requestId, requestOrigin;
    beast::flat_buffer buffer;
    std::optional<http::request_parser<http::string_body>> parser;
    bool closed{false};
};

void Gateway::Impl::accept()
{
    acceptor.async_accept([self = this->shared_from_this()](beast::error_code error, tcp::socket socket) {
        if (!self->running.load()) return;
        if (!error)
        {
            if (self->connections.size() < self->config.maxConnections)
            {
                const auto peer = socket.remote_endpoint().address().to_string();
                if (self->config.tlsCertificate.empty())
                {
                    auto session = std::make_shared<HttpSession<beast::tcp_stream>>(beast::tcp_stream(std::move(socket)), self, peer);
                    self->connections.insert(session); session->run();
                }
                else
                {
                    auto session = std::make_shared<HttpSession<TlsStream>>(TlsStream(std::move(socket), self->tls), self, peer);
                    self->connections.insert(session); session->run();
                }
            }
            else { beast::error_code ignored; socket.close(ignored); }
        }
        if (error != net::error::operation_aborted) self->accept();
    });
}

void Gateway::Impl::start()
{
    if (started) throw std::logic_error("Gateway instance can only be started once");
    if (config.adminToken.empty() || config.adminToken.size() > 4096 || config.viewerToken.size() > 4096 ||
        (!config.viewerToken.empty() && config.viewerToken == config.adminToken))
        throw std::invalid_argument("set a nonempty UAV_GATEWAY_TOKEN; viewer and admin tokens must differ");
    if (config.maxConnections == 0 || config.maxConnections > 1024 || config.maxPendingCommands == 0 || config.maxPendingCommands > kCommandHistory ||
        config.maxDevices == 0 || config.maxDevices > 128 || config.ticketLifetime.count() <= 0 ||
        config.ticketLifetime.count() > 300 || config.accessLifetime.count() <= 0 || config.accessLifetime.count() > 86400 ||
        config.refreshLifetime < config.accessLifetime || config.refreshLifetime.count() > 604800 ||
        config.playbackLifetime.count() <= 0 || config.playbackLifetime.count() > 3600)
        throw std::invalid_argument("invalid gateway resource limits");
    if (config.tlsCertificate.empty() != config.tlsPrivateKey.empty())
        throw std::invalid_argument("TLS requires both certificate and private key");
    if (!config.mediaServiceToken.empty() && (config.mediaServiceToken == config.adminToken || config.mediaServiceToken == config.viewerToken))
        throw std::invalid_argument("media service token must differ from API credentials");
    if (!config.webRoot.empty())
    {
        auto root = std::filesystem::canonical(config.webRoot);
        if (!std::filesystem::is_directory(root)) throw std::invalid_argument("web root must be a directory");
        config.webRoot = root.string();
        for (const auto &file : {config.stateFile, config.tlsPrivateKey})
            if (!file.empty())
            {
                const auto relative = std::filesystem::weakly_canonical(std::filesystem::absolute(file)).lexically_relative(root);
                if (!relative.empty() && *relative.begin() != "..") throw std::invalid_argument("state file and private key must be outside web root");
            }
    }
    if (!config.tlsCertificate.empty())
    {
        tls.set_options(net::ssl::context::default_workarounds | net::ssl::context::no_sslv2 | net::ssl::context::no_sslv3);
        tls.set_password_callback([](std::size_t, net::ssl::context::password_purpose) { return std::string{}; });
        if (SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_2_VERSION) != 1)
            throw std::runtime_error("cannot configure TLS minimum version");
        tls.use_certificate_chain_file(config.tlsCertificate);
        tls.use_private_key_file(config.tlsPrivateKey, net::ssl::context::pem);
        if (SSL_CTX_check_private_key(tls.native_handle()) != 1) throw std::invalid_argument("TLS key does not match certificate");
    }
    stateLock = std::make_unique<detail::StateLock>(config.stateFile);
    restoreState();
    const tcp::endpoint endpoint(net::ip::make_address(config.bindAddress), config.port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(net::socket_base::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen(net::socket_base::max_listen_connections);
    boundPort = acceptor.local_endpoint().port();
    started = true;
    running.store(true);
    guard.emplace(net::make_work_guard(io));
    try
    {
        commandThread = std::thread([self = this->shared_from_this()] { self->commandLoop(); });
        fileThread = std::thread([self = this->shared_from_this()] { self->fileLoop(); });
        accept();
        tick();
        networkThread = std::thread([self = this->shared_from_this()] { self->io.run(); });
    }
    catch (...)
    {
        running.store(false);
        { std::lock_guard<std::mutex> lock(mutex); commandsStopping = true; }
        commandReady.notify_all();
        if (commandThread.joinable()) commandThread.join();
        { std::lock_guard<std::mutex> lock(fileMutex); filesStopping = true; }
        fileReady.notify_all();
        if (fileThread.joinable()) fileThread.join();
        beast::error_code ignored;
        acceptor.close(ignored);
        timer.cancel();
        guard.reset();
        io.poll();
        throw;
    }
}

void Gateway::Impl::stop()
{
    if (!running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        commandsStopping = true;
        for (const auto &job : jobs)
        {
            auto &record = commands.at(job.id);
            record["state"] = "cancelled";
            record["finished_ms"] = nowMs();
            --pendingCommands;
        }
        jobs.clear();
    }
    commandReady.notify_all();
    if (commandThread.joinable()) commandThread.join();
    { std::lock_guard<std::mutex> lock(fileMutex); filesStopping = true; }
    fileReady.notify_all();
    if (fileThread.joinable()) fileThread.join();
    net::post(io, [self = this->shared_from_this()] {
        beast::error_code ignored;
        self->acceptor.close(ignored);
        self->timer.cancel();
        std::vector<std::shared_ptr<Connection>> copy(self->connections.begin(), self->connections.end());
        for (auto &connection : copy) connection->close();
        self->tickets.clear();
        self->security.clear();
        self->guard.reset(); // 排空取消回调，io.run 自然结束
    });
    if (networkThread.joinable()) networkThread.join();
    stateLock.reset();
}

Gateway::Gateway(Config config) : m_impl(std::make_shared<Impl>(std::move(config))) {}
Gateway::~Gateway() { stop(); }
void Gateway::start() { m_impl->start(); }
void Gateway::stop() { m_impl->stop(); }
std::uint16_t Gateway::port() const { return m_impl->boundPort; }

void Gateway::registerDevice(Device device, std::vector<std::string> commands, CommandHandler handler)
{
    if (m_impl->started) throw std::logic_error("register adapters before starting Gateway");
    if (!identifier(device.id) || !deviceType(device.type) || device.name.size() > 128 ||
        json::serialize(device.config).size() > 16 * 1024 || json::serialize(device.status).size() > 2048)
        throw std::invalid_argument("invalid device descriptor");
    if ((!commands.empty() && !handler) || (handler && commands.empty()))
        throw std::invalid_argument("commands and handler must be registered together");
    for (const auto &action : commands) if (!identifier(action)) throw std::invalid_argument("invalid action");
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->devices.count(device.id)) throw std::invalid_argument("duplicate device id");
    if (m_impl->devices.size() >= m_impl->config.maxDevices) throw std::invalid_argument("device registry is full");
    auto id = device.id;
    m_impl->devices.emplace(id, Impl::DeviceEntry{std::move(device), std::move(commands), std::move(handler)});
}

void Gateway::registerStream(Stream stream, StreamResolver resolver)
{
    if (m_impl->started) throw std::logic_error("register adapters before starting Gateway");
    if (!identifier(stream.id) || !identifier(stream.deviceId) || stream.protocols.empty() ||
        (stream.codec != "h264" && stream.codec != "h265")) throw std::invalid_argument("invalid stream descriptor");
    for (const auto &protocol : stream.protocols)
        if (!protocolType(protocol)) throw std::invalid_argument("invalid stream protocol");
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->devices.count(stream.deviceId)) throw std::invalid_argument("stream device is not registered");
    if (m_impl->streams.count(stream.id)) throw std::invalid_argument("duplicate stream id");
    if (m_impl->streams.size() >= 128) throw std::invalid_argument("stream registry is full");
    auto id = stream.id;
    m_impl->streams.emplace(id, Impl::StreamEntry{std::move(stream), std::move(resolver)});
}

void Gateway::updateDeviceStatus(const std::string &id, json::object status)
{
    if (json::serialize(status).size() > 2048) throw std::invalid_argument("status exceeds 2 KiB");
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->devices.find(id);
    if (found == m_impl->devices.end()) throw std::invalid_argument("unknown device id");
    found->second.device.status = std::move(status);
}

bool Gateway::publish(std::string type, json::value data)
{
    return m_impl->publish(std::move(type), std::move(data));
}
bool Gateway::validatePlaybackToken(const std::string &token, const std::string &streamId, const std::string &protocol) const
{
    return m_impl->validateMedia(token, streamId, protocol);
}
} // namespace uav::gateway
