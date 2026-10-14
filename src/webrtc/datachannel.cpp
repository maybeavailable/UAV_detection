#include"webrtc/datachannel.h"
#include"log/log.h"

#include<boost/asio/post.hpp>
#include<boost/asio/steady_timer.hpp>
#include<boost/asio/strand.hpp>
#include<usrsctp.h>

#include<algorithm>
#include<atomic>
#include<cerrno>
#include<cstring>
#include<deque>
#include<map>
#include<mutex>
#include<set>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace net = boost::asio;
namespace
{
using Bytes = UdpSocket::Bytes;
using ErrorCode = UdpSocket::ErrorCode;
using Clock = std::chrono::steady_clock;
using Completion = std::function<void(ErrorCode)>;
constexpr std::uint32_t Control = 50, Text = 51, Binary = 53, EmptyText = 56, EmptyBinary = 57;
constexpr std::size_t MaxDcep = 12 + 2 * 65535;
const char* stateName(DataChannelState state)
{
    switch (state) { case DataChannelState::Connecting: return "Connecting"; case DataChannelState::Open: return "Open"; case DataChannelState::Closing: return "Closing"; case DataChannelState::Closed: return "Closed"; }
    return "Unknown";
}
const char* stateName(DataChannelTransportState state)
{
    switch (state) { case DataChannelTransportState::New: return "New"; case DataChannelTransportState::Connecting: return "Connecting"; case DataChannelTransportState::Connected: return "Connected"; case DataChannelTransportState::Failed: return "Failed"; case DataChannelTransportState::Closed: return "Closed"; }
    return "Unknown";
}
void complete(const Completion& done, ErrorCode ec = {}) { if (done) done(ec); }
ErrorCode socketError() { return {errno, boost::system::generic_category()}; }
bool utf8(const std::uint8_t* data, std::size_t size)
{
    for (std::size_t i = 0; i < size;)
    {
        auto first = data[i++]; if (first < 128) continue;
        unsigned more; std::uint32_t value, minimum;
        if (first >= 0xc2 && first <= 0xdf) { more = 1; value = first & 31; minimum = 128; }
        else if (first >= 0xe0 && first <= 0xef) { more = 2; value = first & 15; minimum = 2048; }
        else if (first >= 0xf0 && first <= 0xf4) { more = 3; value = first & 7; minimum = 65536; }
        else return false;
        if (size - i < more) return false;
        while (more--) { const auto c = data[i++]; if ((c & 0xc0) != 0x80) return false; value = (value << 6) | (c & 63); }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
bool utf8(const std::string& s) { return utf8(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
std::uint16_t read16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t read32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
void append16(Bytes& b, std::uint16_t v) { b.push_back(v >> 8); b.push_back(v & 255); }
void append32(Bytes& b, std::uint32_t v) { append16(b, v >> 16); append16(b, v & 65535); }
struct OutputRoute
{
    std::mutex mutex;
    bool enabled{true};
    std::size_t bytes{0}, packets{0}, maxBytes{0}, maxPackets{0}, mtu{0};
    std::function<bool(Bytes)> emit;
};
struct RouteRegistry
{
    std::mutex mutex;
    std::map<void*, std::shared_ptr<OutputRoute>> routes;
    std::atomic<std::uintptr_t> next{1};
};
RouteRegistry& routeRegistry()
{
    // usrsctp 的全局 timer 线程可能晚于 C++ 静态析构；路由表保持进程寿命。
    static auto* value = new RouteRegistry;
    return *value;
}
// AF_CONN 的地址是 opaque token，永不解引用；单调编号避免关闭后延迟回调命中新对象（地址复用）。
void* routeToken()
{
    const auto id = routeRegistry().next.fetch_add(1);
    if (!id || id == UINTPTR_MAX) throw std::runtime_error("SCTP route identifiers exhausted");
    return reinterpret_cast<void*>(id);
}
int output(void* address, void* buffer, std::size_t length, std::uint8_t, std::uint8_t) noexcept
{
    try
    {
        std::shared_ptr<OutputRoute> route;
        { auto& global = routeRegistry(); std::lock_guard<std::mutex> lock(global.mutex); const auto it = global.routes.find(address); if (it == global.routes.end()) return ENETDOWN; route = it->second; }
        std::lock_guard<std::mutex> lock(route->mutex);
        if (!route->enabled) return ENETDOWN;
        if (length > route->mtu) return EMSGSIZE;
        if (route->packets >= route->maxPackets || length > route->maxBytes - route->bytes) return ENOBUFS;
        const auto* begin = static_cast<const std::uint8_t*>(buffer);
        Bytes packet(begin, begin + length);
        ++route->packets; route->bytes += length;
        try { if (route->emit(std::move(packet))) return 0; }
        catch (...) { --route->packets; route->bytes -= length; return ENOMEM; }
        --route->packets; route->bytes -= length; return ENETDOWN;
    }
    catch (...) { return ENOMEM; }
}
void initializeSctp()
{
    static std::once_flag once;
    std::call_once(once, [] { usrsctp_init(0, output, nullptr); usrsctp_sysctl_set_sctp_ecn_enable(0); });
}
bool fail(std::string* reason, const char* message) { if (reason) *reason = message; return false; }
} // namespace

bool DataChannelConfig::validate(std::string* reason) const
{
    if (label.size() > 65535 || protocol.size() > 65535 || !utf8(label) || !utf8(protocol)) return fail(reason, "invalid DataChannel label/protocol UTF-8 or length");
    if (maxRetransmits && maxPacketLifeTime) return fail(reason, "maxRetransmits and maxPacketLifeTime are mutually exclusive");
    if ((id && *id == 65535) || (negotiated && !id)) return fail(reason, "negotiated DataChannel requires an id in 0..65534");
    if (reason) reason->clear();
    return true;
}
bool DataChannelTransport::Config::validate(std::string* reason) const
{
    if (!maxStreams || !maxChannels || maxChannels > maxStreams || !maxMessageSize || maxMessageSize > 16 * 1024 * 1024 ||
        maxBufferedSendBytes < maxMessageSize || maxBufferedSendBytes > 64 * 1024 * 1024 || !maxBufferedSendMessages ||
        maxBufferedReceiveBytes < std::max(maxMessageSize, MaxDcep) || maxBufferedReceiveBytes > 64 * 1024 * 1024 || !maxBufferedReceiveMessages ||
        maxPendingWireBytes < 65507 || maxPendingWireBytes > 64 * 1024 * 1024 || !maxPendingWirePackets ||
        socketBufferSize < std::max(maxMessageSize, MaxDcep) || socketBufferSize > 64 * 1024 * 1024 ||
        connectTimeout < std::chrono::milliseconds(20) || channelOpenTimeout < std::chrono::milliseconds(20) || channelCloseTimeout < std::chrono::milliseconds(20))
        return fail(reason, "invalid DataChannel transport configuration");
    if (reason) reason->clear();
    return true;
}
Bytes DcepOpen::encode() const
{
    if (!channel.validate() || channel.negotiated) throw std::invalid_argument("invalid DCEP OPEN configuration");
    Bytes b{3, static_cast<std::uint8_t>((channel.ordered ? 0 : 0x80) | (channel.maxRetransmits ? 1 : channel.maxPacketLifeTime ? 2 : 0))};
    append16(b, channel.priority); append32(b, channel.maxRetransmits.value_or(channel.maxPacketLifeTime.value_or(0)));
    append16(b, channel.label.size()); append16(b, channel.protocol.size());
    b.insert(b.end(), channel.label.begin(), channel.label.end()); b.insert(b.end(), channel.protocol.begin(), channel.protocol.end()); return b;
}
bool DcepOpen::parse(const Bytes& b, DcepOpen& out, std::string* reason)
{
    if (b.size() < 12 || b[0] != 3 || (b[1] & 0x7f) > 2) return fail(reason, "invalid DCEP OPEN header/type");
    const auto label = read16(b.data() + 8), protocol = read16(b.data() + 10);
    if (b.size() != 12 + std::size_t(label) + protocol) return fail(reason, "invalid DCEP OPEN lengths");
    DcepOpen parsed; auto& c = parsed.channel; c.ordered = !(b[1] & 0x80); c.priority = read16(b.data() + 2);
    const auto reliability = read32(b.data() + 4);
    if ((b[1] & 0x7f) != 0 && reliability > 65535) return fail(reason, "unsupported DataChannel reliability range");
    if ((b[1] & 0x7f) == 1) c.maxRetransmits = static_cast<std::uint16_t>(reliability);
    if ((b[1] & 0x7f) == 2) c.maxPacketLifeTime = static_cast<std::uint16_t>(reliability);
    c.label.assign(b.begin() + 12, b.begin() + 12 + label); c.protocol.assign(b.begin() + 12 + label, b.end());
    if (!c.validate(reason)) return false;
    out = std::move(parsed); return true;
}

struct DataChannel::Impl
{
    net::strand<net::io_context::executor_type> strand;
    std::weak_ptr<DataChannelTransport::Impl> owner;
    Snapshot current, published;
    Callbacks callbacks;
    mutable std::mutex mutex;
    std::deque<DataChannelMessage> received;
    bool opened{false}, resetRequested{false}, resetSubmitted{false};
    std::uint64_t loggedErrors{0};
    Clock::time_point openedAt{}, closingAt{};
    Impl(net::strand<net::io_context::executor_type> executor, DataChannelConfig config, Callbacks handlers)
        : strand(std::move(executor)), callbacks(std::move(handlers)) { current.config = std::move(config); publish(); }
    void publish() { std::lock_guard<std::mutex> lock(mutex); published = current; }
    void state(DataChannelState s)
    {
        if (current.state == s) return;
        const auto previous = current.state; current.state = s; publish();
        UAV_LOG_INFO << "[WebRTC][DataChannel " << this << "][sid=" << (current.config.id ? std::to_string(*current.config.id) : "pending")
            << "] state " << stateName(previous) << " -> " << stateName(s) << " buffered=" << current.bufferedAmount;
        if (s == DataChannelState::Open && callbacks.onOpen) callbacks.onOpen();
        if (s == DataChannelState::Closed && callbacks.onClosed) callbacks.onClosed();
    }
    void error(ErrorCode ec, const std::string& message)
    {
        if (ec != net::error::operation_aborted)
        {
            ++loggedErrors;
            UAV_LOG_IF(LogLevel::WARN, (loggedErrors & (loggedErrors - 1)) == 0)
                << "[WebRTC][DataChannel " << this << "][sid=" << (current.config.id ? std::to_string(*current.config.id) : "pending")
                << "] " << message << " error=" << ec.value() << ":" << ec.message() << " errors=" << loggedErrors;
        }
        if (callbacks.onError) callbacks.onError(ec, message);
    }
    void buffered(std::size_t amount)
    {
        const bool before = current.bufferedAmount > current.config.bufferedAmountLowThreshold;
        current.bufferedAmount = amount; publish();
        if (before && amount <= current.config.bufferedAmountLowThreshold && callbacks.onBufferedAmountLow) callbacks.onBufferedAmountLow();
    }
};

struct DataChannelTransport::Impl : std::enable_shared_from_this<DataChannelTransport::Impl>
{
    struct Queued
    {
        std::shared_ptr<DataChannel> channel;
        Bytes data;
        std::uint32_t ppid;
        UdpSocket::SendHandler done;
        std::size_t applicationSize{0};
        Clock::time_point queuedAt;
    };
    struct Partial { Bytes data; std::uint32_t ppid{0}; std::uint16_t ssn{0}; };
    net::strand<net::io_context::executor_type> strand;
    net::steady_timer timer;
    Sender sender;
    Config config;
    Callbacks callbacks;
    Snapshot current, published;
    mutable std::mutex mutex;
    std::optional<Parameters> parameters;
    SdpSetupRole allocationRole{SdpSetupRole::Active};
    struct socket* socket{nullptr};
    void* address{nullptr};
    std::shared_ptr<OutputRoute> route;
    std::vector<std::shared_ptr<DataChannel>> channels;
    std::set<std::uint16_t> retired;
    std::map<std::uint16_t, unsigned> resets;
    std::map<std::uint16_t, Partial> partial;
    std::deque<Queued> queue;
    std::deque<Bytes> wire;
    std::vector<std::uint8_t> receiveBuffer;
    std::size_t queuedBytes{0}, receiveBytes{0}, receiveMessages{0};
    bool closing{false}, closed{false}, closePrepared{false}, sendingWire{false}, timerPending{false}, pumping{false};
    std::mutex sendGate;
    bool acceptingSends{true};
    std::size_t reservedSendBytes{0}, reservedSendMessages{0};
    std::uint64_t sendSerial{0};
    std::uint64_t loggedErrors{0};
    std::atomic<std::size_t> queuedInputBytes{0}, queuedInputPackets{0};
    std::vector<Completion> closeCallbacks;

    Impl(net::io_context& io, Sender outputSender, Config options, Callbacks handlers)
        : strand(net::make_strand(io)), timer(strand), sender(std::move(outputSender)), config(std::move(options)), callbacks(std::move(handlers))
    {
        if (!sender || !config.validate()) throw std::invalid_argument("invalid DataChannel transport configuration");
        // SEND_FAILED notification 可能携带整条未发送的用户消息，控制事件也必须一次完整读取。
        receiveBuffer.resize(std::max(config.maxMessageSize, MaxDcep) + sizeof(union sctp_notification)); publish();
    }
    ~Impl() { destroySocket(); }
    void publish()
    {
        current.channels = std::count_if(channels.begin(), channels.end(), [](const auto& c) { return c->impl_->current.state != DataChannelState::Closed; });
        current.pendingSendBytes = queuedBytes;
        if (route) { std::lock_guard<std::mutex> lock(route->mutex); current.pendingWirePackets = route->packets; }
        std::lock_guard<std::mutex> lock(mutex); published = current;
    }
    void state(DataChannelTransportState s)
    {
        const auto previous = current.state; current.state = s; publish();
        if (previous != s) UAV_LOG_INFO << "[WebRTC][SCTP " << this << "][g=" << current.generation << "] state " << stateName(previous) << " -> " << stateName(s)
            << " inbound_streams=" << current.inboundStreams << " outbound_streams=" << current.outboundStreams;
        if (previous != s && callbacks.onState) callbacks.onState(s);
    }
    void error(ErrorCode ec, const std::string& message)
    {
        if (!closing)
        {
            ++loggedErrors;
            UAV_LOG_IF(LogLevel::WARN, (loggedErrors & (loggedErrors - 1)) == 0)
                << "[WebRTC][SCTP " << this << "][g=" << current.generation << "] " << message << " error=" << ec.value() << ":" << ec.message() << " errors=" << loggedErrors;
            if (callbacks.onError) callbacks.onError(ec, message);
        }
    }
    void releaseWire(std::size_t bytes)
    {
        std::lock_guard<std::mutex> lock(route->mutex); --route->packets; route->bytes -= bytes;
    }
    void destroySocket()
    {
        if (route) { std::lock_guard<std::mutex> lock(route->mutex); route->enabled = false; }
        if (address) { auto& global = routeRegistry(); std::lock_guard<std::mutex> lock(global.mutex); global.routes.erase(address); }
        if (socket) { usrsctp_close(socket); socket = nullptr; }
        if (address) { usrsctp_deregister_address(address); address = nullptr; }
    }
    void discardReceived(const std::shared_ptr<DataChannel>& c)
    {
        for (const auto& message : c->impl_->received) { receiveBytes -= message.data.size(); --receiveMessages; }
        c->impl_->received.clear();
    }
    void finishChannel(const std::shared_ptr<DataChannel>& c, ErrorCode ec = {})
    {
        auto& v = *c->impl_;
        if (v.current.state == DataChannelState::Closed) return;
        discardReceived(c);
        for (auto it = queue.begin(); it != queue.end();)
        {
            if (it->channel != c) { ++it; continue; }
            auto done = std::move(it->done); queuedBytes -= it->data.size(); it = queue.erase(it);
            if (done) done(ec ? ec : ErrorCode(net::error::operation_aborted), 0);
        }
        if (v.current.config.id)
        {
            retired.insert(*v.current.config.id);
            const auto found = partial.find(*v.current.config.id);
            if (found != partial.end()) { receiveBytes -= found->second.data.size(); partial.erase(found); }
        }
        v.buffered(0); if (ec) v.error(ec, "DataChannel closed"); v.state(DataChannelState::Closed); publish();
    }
    void fail(ErrorCode ec, const std::string& message)
    {
        if (closing || current.state == DataChannelTransportState::Failed) return;
        UAV_LOG_ERROR << "[WebRTC][SCTP " << this << "][g=" << current.generation << "] failed: " << message << " error=" << ec.value() << ":" << ec.message();
        current.paused = true; destroySocket();
        for (const auto& c : channels) finishChannel(c, ec);
        partial.clear(); receiveBytes = 0;
        state(DataChannelTransportState::Failed); error(ec, message);
        if (!sendingWire) while (!wire.empty()) { releaseWire(wire.front().size()); wire.pop_front(); }
    }
    std::shared_ptr<DataChannel> find(std::uint16_t sid) const
    {
        for (const auto& c : channels) if (c->impl_->current.config.id == sid && c->impl_->current.state != DataChannelState::Closed) return c;
        return {};
    }
    void schedule()
    {
        if (closing || timerPending || current.state == DataChannelTransportState::Failed || !socket) return;
        timerPending = true; timer.expires_after(std::chrono::milliseconds(10)); const auto self = shared_from_this();
        timer.async_wait([self](ErrorCode ec) {
            self->timerPending = false; if (ec || self->closing) return;
            self->receive(); self->pump(); self->sendWire(); self->checkTimeouts(); self->schedule();
        });
    }
    void sendWire()
    {
        if (closing || sendingWire || current.paused || wire.empty() || current.state == DataChannelTransportState::Failed) return;
        while (!wire.empty() && wire.front().size() > parameters->maxPacketSize) { releaseWire(wire.front().size()); wire.pop_front(); }
        if (wire.empty()) return;
        sendingWire = true; const auto self = shared_from_this(); const auto serial = ++sendSerial; const auto generation = current.generation;
        const auto expected = wire.front().size();
        auto finish = [self, serial, generation, expected](ErrorCode ec, std::size_t size) {
            net::post(self->strand, [self, serial, generation, expected, ec, size] {
                if (!self->sendingWire || serial != self->sendSerial) return;
                self->sendingWire = false;
                if (self->closing || (!ec && size == expected)) { self->releaseWire(self->wire.front().size()); self->wire.pop_front(); }
                else if (generation == self->current.generation && !self->current.paused && ec != net::error::no_buffer_space && ec != net::error::not_connected && ec != net::error::operation_aborted)
                { self->fail(ec ? ec : ErrorCode(net::error::message_size), "SCTP output through DTLS failed"); }
                self->publish(); self->finishClose();
                if (!ec) self->sendWire(); // 有背压时由定时器重试，避免忙循环
            });
        };
        try { sender(wire.front(), finish); } catch (const std::exception&) { finish(net::error::fault, 0); }
    }
    template<class T> ErrorCode option(int name, const T& value)
    { return usrsctp_setsockopt(socket, IPPROTO_SCTP, name, &value, sizeof(value)) < 0 ? socketError() : ErrorCode{}; }
    ErrorCode mtu(std::size_t size)
    {
        sctp_paddrparams p{}; p.spp_address.ss_family = AF_CONN; p.spp_flags = SPP_PMTUD_DISABLE; p.spp_pathmtu = static_cast<std::uint32_t>(size - 12); // AF_CONN 的 MTU 不计入 SCTP common header（12 字节）
        return option(SCTP_PEER_ADDR_PARAMS, p);
    }
    ErrorCode makeSocket()
    {
        initializeSctp(); address = routeToken(); route = std::make_shared<OutputRoute>();
        route->maxBytes = config.maxPendingWireBytes; route->maxPackets = config.maxPendingWirePackets; route->mtu = parameters->maxPacketSize;
        const auto weak = weak_from_this();
        route->emit = [weak](Bytes bytes) {
            if (auto self = weak.lock())
            {
                net::post(self->strand, [self, bytes = std::move(bytes)]() mutable {
                    if (self->closing || self->current.state == DataChannelTransportState::Failed) { self->releaseWire(bytes.size()); self->finishClose(); return; }
                    self->wire.push_back(std::move(bytes)); self->publish(); self->sendWire();
                });
                return true;
            }
            return false;
        };
        { auto& global = routeRegistry(); std::lock_guard<std::mutex> lock(global.mutex); global.routes.emplace(address, route); }
        usrsctp_register_address(address);
        socket = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, nullptr, nullptr, 0, nullptr);
        if (!socket) return socketError();
        if (usrsctp_set_non_blocking(socket, 1) < 0) return socketError();
        linger l{}; l.l_onoff = 1; l.l_linger = 0;
        if (usrsctp_setsockopt(socket, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0) return socketError();
        const auto buffer = static_cast<int>(config.socketBufferSize);
        if (usrsctp_setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) < 0 || usrsctp_setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) < 0) return socketError();
        int on = 1, interleave = 2;
        if (auto ec = option(SCTP_NODELAY, on)) return ec;
        if (auto ec = option(SCTP_RECVRCVINFO, on)) return ec;
        if (auto ec = option(SCTP_EXPLICIT_EOR, on)) return ec;
        if (auto ec = option(SCTP_FRAGMENT_INTERLEAVE, interleave)) return ec;
        sctp_assoc_value reset{}; reset.assoc_id = SCTP_ALL_ASSOC; reset.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ;
        if (auto ec = option(SCTP_ENABLE_STREAM_RESET, reset)) return ec;
        for (const auto type : {SCTP_ASSOC_CHANGE, SCTP_STREAM_RESET_EVENT, SCTP_SHUTDOWN_EVENT, SCTP_SENDER_DRY_EVENT, SCTP_PARTIAL_DELIVERY_EVENT, SCTP_SEND_FAILED_EVENT})
        {
            sctp_event e{}; e.se_assoc_id = SCTP_ALL_ASSOC; e.se_type = static_cast<std::uint16_t>(type); e.se_on = 1;
            if (auto ec = option(SCTP_EVENT, e)) return ec;
        }
        sctp_initmsg init{}; init.sinit_num_ostreams = config.maxStreams; init.sinit_max_instreams = config.maxStreams;
        if (auto ec = option(SCTP_INITMSG, init)) return ec;
        sctp_assoc_value scheduler{}; scheduler.assoc_id = SCTP_ALL_ASSOC; scheduler.assoc_value = SCTP_SS_PRIORITY;
        if (auto ec = option(SCTP_PLUGGABLE_SS, scheduler)) return ec;
        if (auto ec = mtu(parameters->maxPacketSize)) return ec;
        sockaddr_conn local{}; local.sconn_family = AF_CONN; local.sconn_port = htons(parameters->localPort); local.sconn_addr = address;
        if (usrsctp_bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) return socketError();
        auto remote = local; remote.sconn_port = htons(parameters->remotePort);
        if (usrsctp_connect(socket, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0 && errno != EINPROGRESS) return socketError();
        return {};
    }
    Clock::time_point connectedAt{};
    std::shared_ptr<DataChannel> makeChannel(DataChannelConfig options, DataChannel::Callbacks handlers)
    {
        channels.erase(std::remove_if(channels.begin(), channels.end(), [](const auto& c) { return c->impl_->current.state == DataChannelState::Closed; }), channels.end());
        auto impl = std::make_shared<DataChannel::Impl>(strand, std::move(options), std::move(handlers)); impl->owner = weak_from_this();
        impl->current.maxMessageSize = current.maxMessageSize; impl->publish();
        auto channel = std::shared_ptr<DataChannel>(new DataChannel(std::move(impl))); channels.push_back(channel); publish(); return channel;
    }
    bool enqueue(const std::shared_ptr<DataChannel>& c, Bytes bytes, std::uint32_t ppid, std::size_t size, UdpSocket::SendHandler done = {})
    {
        if (queue.size() >= config.maxBufferedSendMessages || bytes.size() > config.maxBufferedSendBytes - queuedBytes) return false;
        queuedBytes += bytes.size();
        queue.push_back({c, std::move(bytes), ppid, std::move(done), size, Clock::now()});
        if (ppid != Control) c->impl_->buffered(c->impl_->current.bufferedAmount + size);
        publish(); return true;
    }
    ErrorCode assign(const std::shared_ptr<DataChannel>& c)
    {
        auto& v = *c->impl_; auto& cfg = v.current.config;
        if (v.opened || v.current.state != DataChannelState::Connecting) return {};
        if (!cfg.id)
        {
            for (unsigned sid = allocationRole == SdpSetupRole::Active ? 0 : 1; sid < current.outboundStreams; sid += 2)
                if (!find(static_cast<std::uint16_t>(sid)) && !retired.count(static_cast<std::uint16_t>(sid))) { cfg.id = static_cast<std::uint16_t>(sid); break; }
            if (!cfg.id) return net::error::no_buffer_space;
        }
        if (*cfg.id >= current.outboundStreams || *cfg.id >= current.inboundStreams ||
            (!cfg.negotiated && (*cfg.id % 2 != (allocationRole == SdpSetupRole::Active ? 0 : 1)))) return net::error::invalid_argument;
        v.publish();
        sctp_stream_value priority{}; priority.assoc_id = SCTP_ALL_ASSOC; priority.stream_id = *cfg.id; priority.stream_value = 65535 - cfg.priority;
        if (auto ec = option(SCTP_SS_VALUE, priority)) return ec;
        if (cfg.negotiated) { v.opened = true; v.state(DataChannelState::Open); return {}; }
        if (!enqueue(c, DcepOpen{cfg}.encode(), Control, 0)) return net::error::no_buffer_space;
        v.opened = true; v.openedAt = Clock::now(); return {};
    }
    void assignChannels()
    {
        for (const auto& c : channels) if (auto ec = assign(c)) finishChannel(c, ec);
        pump();
    }
    void create(DataChannelConfig options, DataChannel::Callbacks handlers, CreateHandler done)
    {
        ErrorCode ec;
        if (closing || current.state == DataChannelTransportState::Failed) ec = net::error::operation_aborted;
        else if (!options.validate()) ec = net::error::invalid_argument;
        else if (current.channels >= config.maxChannels || (options.id && (find(*options.id) || retired.count(*options.id)))) ec = net::error::no_buffer_space;
        else if (options.id && parameters && !options.negotiated && *options.id % 2 != (allocationRole == SdpSetupRole::Active ? 0 : 1)) ec = net::error::invalid_argument;
        if (ec) { if (done) done(ec, {}); return; }
        auto c = makeChannel(std::move(options), std::move(handlers));
        const auto& cfg = c->impl_->current.config;
        UAV_LOG_INFO << "[WebRTC][SCTP " << this << "][g=" << current.generation << "] create local channel=" << c->impl_.get()
            << " negotiated=" << cfg.negotiated << " ordered=" << cfg.ordered << " label_bytes=" << cfg.label.size() << " protocol_bytes=" << cfg.protocol.size()
            << " max_retransmits=" << (cfg.maxRetransmits ? std::to_string(*cfg.maxRetransmits) : "none")
            << " lifetime_ms=" << (cfg.maxPacketLifeTime ? std::to_string(*cfg.maxPacketLifeTime) : "unlimited");
        if (current.state == DataChannelTransportState::Connected) ec = assign(c);
        if (ec) finishChannel(c, ec);
        if (done) done(ec, ec ? nullptr : c);
        pump();
    }
    void submit(const std::shared_ptr<DataChannel::Impl>& channel, Bytes bytes, bool binary, UdpSocket::SendHandler done)
    {
        const auto size = bytes.size(); ErrorCode ec;
        {
            std::lock_guard<std::mutex> lock(sendGate);
            if (!acceptingSends) ec = net::error::operation_aborted;
            else if (size > config.maxMessageSize) ec = net::error::message_size;
            else if (reservedSendMessages >= config.maxBufferedSendMessages || size > config.maxBufferedSendBytes - reservedSendBytes) ec = net::error::no_buffer_space;
            else { ++reservedSendMessages; reservedSendBytes += size; }
        }
        if (ec) { net::post(strand, [done, ec] { if (done) done(ec, 0); }); return; }
        const auto self = shared_from_this();
        UdpSocket::SendHandler finish = [self, size, done = std::move(done)](ErrorCode result, std::size_t sent) {
            { std::lock_guard<std::mutex> lock(self->sendGate); --self->reservedSendMessages; self->reservedSendBytes -= size; }
            if (done) done(result, sent);
            self->finishClose();
        };
        try { net::post(strand, [self, channel, bytes = std::move(bytes), binary, finish = std::move(finish)]() mutable { self->send(channel, std::move(bytes), binary, std::move(finish)); }); }
        catch (...) { std::lock_guard<std::mutex> lock(sendGate); --reservedSendMessages; reservedSendBytes -= size; throw; }
    }
    void send(const std::shared_ptr<DataChannel::Impl>& channel, Bytes bytes, bool binary, UdpSocket::SendHandler done)
    {
        const auto c = std::find_if(channels.begin(), channels.end(), [&](const auto& c) { return c->impl_ == channel; });
        ErrorCode ec;
        if (closing || current.state == DataChannelTransportState::Failed || channel->current.state == DataChannelState::Closed) ec = net::error::operation_aborted;
        else if (c == channels.end() || channel->current.state != DataChannelState::Open) ec = net::error::not_connected;
        else if (bytes.size() > current.maxMessageSize) ec = net::error::message_size;
        else if (!binary && !utf8(bytes.data(), bytes.size())) ec = net::error::invalid_argument;
        if (ec) { if (done) done(ec, 0); return; }
        const auto size = bytes.size(); const auto ppid = bytes.empty() ? (binary ? EmptyBinary : EmptyText) : (binary ? Binary : Text);
        if (bytes.empty()) bytes.push_back(0); // RFC 8831 空消息使用一字节占位和独立 PPID
        if (!enqueue(*c, std::move(bytes), ppid, size, done)) { if (done) done(net::error::no_buffer_space, 0); return; }
        pump();
    }
    void pump()
    {
        if (pumping || closing || current.paused || current.state != DataChannelTransportState::Connected || !socket) return;
        pumping = true;
        while (!queue.empty())
        {
            auto& q = queue.front(); auto& v = *q.channel->impl_;
            if (q.ppid != Control && v.current.state != DataChannelState::Open) break;
            sctp_sendv_spa info{}; info.sendv_flags = SCTP_SEND_SNDINFO_VALID;
            info.sendv_sndinfo.snd_sid = *v.current.config.id; info.sendv_sndinfo.snd_ppid = htonl(q.ppid);
            info.sendv_sndinfo.snd_flags = SCTP_EOR;
            ErrorCode ec;
            if (q.ppid != Control)
            {
                if (!v.current.config.ordered) info.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;
                if (v.current.config.maxRetransmits)
                {
                    info.sendv_flags |= SCTP_SEND_PRINFO_VALID; info.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
                    info.sendv_prinfo.pr_value = *v.current.config.maxRetransmits;
                }
                if (v.current.config.maxPacketLifeTime)
                {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - q.queuedAt).count();
                    if (elapsed >= *v.current.config.maxPacketLifeTime) ec = net::error::timed_out;
                    else
                    {
                        info.sendv_flags |= SCTP_SEND_PRINFO_VALID; info.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
                        info.sendv_prinfo.pr_value = static_cast<std::uint32_t>(*v.current.config.maxPacketLifeTime - elapsed);
                    }
                }
            }
            ssize_t sent = -1;
            if (!ec)
            {
                sent = usrsctp_sendv(socket, q.data.data(), q.data.size(), nullptr, 0, &info, sizeof(info), SCTP_SENDV_SPA, 0);
                if (sent < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS)) break;
                if (sent < 0) ec = socketError(); else if (static_cast<std::size_t>(sent) != q.data.size()) ec = net::error::fault;
            }
            auto finished = std::move(q); queue.pop_front(); queuedBytes -= finished.data.size();
            if (finished.ppid != Control)
            {
                v.buffered(v.current.bufferedAmount - finished.applicationSize);
                if (!ec) { ++v.current.sentMessages; v.current.sentBytes += finished.applicationSize; v.publish(); }
            }
            if (finished.done) finished.done(ec, ec ? 0 : finished.applicationSize);
            if (ec && finished.ppid == Control) finishChannel(finished.channel, ec);
        }
        pumping = false; publish(); sendWire();
    }
    void requestReset(std::uint16_t sid)
    {
        if (!socket || current.state != DataChannelTransportState::Connected) return;
        std::vector<std::uint32_t> storage((sizeof(sctp_reset_streams) + sizeof(std::uint16_t) + 3) / 4);
        auto* reset = reinterpret_cast<sctp_reset_streams*>(storage.data());
        reset->srs_assoc_id = SCTP_ALL_ASSOC; reset->srs_flags = SCTP_STREAM_RESET_OUTGOING; reset->srs_number_streams = 1; reset->srs_stream_list[0] = sid;
        if (usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_RESET_STREAMS, reset, sizeof(sctp_reset_streams) + sizeof(std::uint16_t)) < 0)
        {
            if (errno != EINPROGRESS && errno != EALREADY) if (auto c = find(sid)) c->impl_->error(socketError(), "SCTP stream reset request failed");
        }
        else if (auto c = find(sid)) c->impl_->resetSubmitted = true;
    }
    void closeChannel(const std::shared_ptr<DataChannel::Impl>& impl, Completion done)
    {
        auto found = std::find_if(channels.begin(), channels.end(), [&](const auto& c) { return c->impl_ == impl; });
        if (found == channels.end() || impl->current.state == DataChannelState::Closed) { complete(done); return; }
        auto c = *found;
        if (impl->current.state == DataChannelState::Closing) { complete(done); return; }
        impl->state(DataChannelState::Closing); impl->closingAt = Clock::now(); discardReceived(c);
        for (auto it = queue.begin(); it != queue.end();)
        {
            if (it->channel != c) { ++it; continue; }
            auto callback = std::move(it->done); queuedBytes -= it->data.size(); it = queue.erase(it);
            if (callback) callback(net::error::operation_aborted, 0);
        }
        impl->buffered(0);
        if (!impl->current.config.id || !impl->opened || current.state != DataChannelTransportState::Connected) finishChannel(c);
        else { impl->resetRequested = true; retired.insert(*impl->current.config.id); requestReset(*impl->current.config.id); }
        publish(); complete(done);
    }
    void flushMessages(const std::shared_ptr<DataChannel::Impl>& c)
    {
        while (!c->received.empty() && c->callbacks.onMessage && c->current.state == DataChannelState::Open)
        {
            auto message = std::move(c->received.front()); c->received.pop_front(); receiveBytes -= message.data.size(); --receiveMessages;
            c->callbacks.onMessage(std::move(message));
        }
    }
    void message(std::uint16_t sid, std::uint32_t ppid, Bytes bytes)
    {
        auto c = find(sid);
        if (ppid == Control)
        {
            if (bytes == Bytes{2})
            {
                if (c && !c->impl_->current.config.negotiated && c->impl_->opened && c->impl_->current.state == DataChannelState::Connecting) c->impl_->state(DataChannelState::Open);
                return;
            }
            DcepOpen open;
            if (!DcepOpen::parse(bytes, open) || sid % 2 == (allocationRole == SdpSetupRole::Active ? 0 : 1) || c || retired.count(sid) ||
                current.channels >= config.maxChannels || sid >= current.outboundStreams)
            { requestReset(sid); error(net::error::invalid_argument, "invalid or conflicting remote DCEP OPEN"); return; }
            open.channel.id = sid; c = makeChannel(std::move(open.channel), {}); c->impl_->opened = true;
            UAV_LOG_INFO << "[WebRTC][SCTP " << this << "][g=" << current.generation << "] remote DCEP OPEN sid=" << sid << " channel=" << c->impl_.get()
                << " ordered=" << c->impl_->current.config.ordered << " label_bytes=" << c->impl_->current.config.label.size() << " protocol_bytes=" << c->impl_->current.config.protocol.size();
            sctp_stream_value priority{}; priority.assoc_id = SCTP_ALL_ASSOC; priority.stream_id = sid; priority.stream_value = 65535 - c->impl_->current.config.priority;
            if (auto ec = option(SCTP_SS_VALUE, priority)) { finishChannel(c, ec); requestReset(sid); return; }
            if (!enqueue(c, {2}, Control, 0)) { finishChannel(c, net::error::no_buffer_space); requestReset(sid); return; }
            if (callbacks.onDataChannel) callbacks.onDataChannel(c);
            c->impl_->state(DataChannelState::Open); pump(); return;
        }
        if (!c) { requestReset(sid); return; }
        auto& v = *c->impl_;
        if (v.current.state == DataChannelState::Closing || v.current.state == DataChannelState::Closed) return;
        const bool binary = ppid == Binary || ppid == EmptyBinary;
        const bool empty = ppid == EmptyBinary || ppid == EmptyText;
        if ((ppid != Text && ppid != Binary && !empty) || (empty && bytes.size() != 1) || (!binary && !empty && !utf8(bytes.data(), bytes.size())))
        { v.error(net::error::invalid_argument, "unsupported PPID or invalid DataChannel message"); closeChannel(c->impl_, {}); return; }
        if (empty) bytes.clear();
        if (bytes.size() > parameters->localMaxMessageSize)
        { v.error(net::error::message_size, "DataChannel message exceeds local limit"); closeChannel(c->impl_, {}); return; }
        if (v.current.state == DataChannelState::Connecting) v.state(DataChannelState::Open); // 用户数据也能确认 DCEP 握手
        ++v.current.receivedMessages; v.current.receivedBytes += bytes.size(); v.publish();
        if (v.callbacks.onMessage) v.callbacks.onMessage({binary, std::move(bytes)});
        else { receiveBytes += bytes.size(); ++receiveMessages; v.received.push_back({binary, std::move(bytes)}); }
    }
    void notification(const std::uint8_t* data, std::size_t size)
    {
        if (size < 8) return;
        union sctp_notification n{}; std::memcpy(&n, data, std::min(size, sizeof(n)));
        if (n.sn_header.sn_length != size) { fail(net::error::fault, "truncated SCTP notification"); return; }
        if (n.sn_header.sn_type == SCTP_ASSOC_CHANGE && size >= sizeof(sctp_assoc_change))
        {
            const auto& a = n.sn_assoc_change;
            if (a.sac_state == SCTP_COMM_UP)
            { current.inboundStreams = a.sac_inbound_streams; current.outboundStreams = a.sac_outbound_streams; state(DataChannelTransportState::Connected); assignChannels(); }
            else if (a.sac_state == SCTP_RESTART || a.sac_state == SCTP_COMM_LOST || a.sac_state == SCTP_CANT_STR_ASSOC || a.sac_state == SCTP_SHUTDOWN_COMP)
                fail(net::error::connection_reset, "SCTP association ended or restarted");
        }
        else if (n.sn_header.sn_type == SCTP_SHUTDOWN_EVENT) fail(net::error::connection_reset, "remote SCTP association shutdown");
        else if (n.sn_header.sn_type == SCTP_STREAM_RESET_EVENT && size >= sizeof(sctp_stream_reset_event))
        {
            const auto& event = n.sn_strreset_event;
            std::vector<std::uint16_t> streams;
            for (std::size_t offset = sizeof(sctp_stream_reset_event); offset + 2 <= size; offset += 2)
            { std::uint16_t sid; std::memcpy(&sid, data + offset, 2); streams.push_back(sid); }
            if (streams.empty()) for (const auto& c : channels) if (c->impl_->current.config.id) streams.push_back(*c->impl_->current.config.id);
            for (const auto sid : streams)
            {
                auto c = find(sid);
                if (event.strreset_flags & (SCTP_STREAM_RESET_DENIED | SCTP_STREAM_RESET_FAILED))
                { if (c) finishChannel(c, net::error::operation_not_supported); continue; }
                auto& bits = resets[sid];
                if (event.strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) bits |= 1;
                if (event.strreset_flags & SCTP_STREAM_RESET_OUTGOING_SSN) bits |= 2;
                if (c)
                {
                    if (c->impl_->current.state != DataChannelState::Closing) { c->impl_->state(DataChannelState::Closing); c->impl_->closingAt = Clock::now(); }
                    if ((bits & 1) && !c->impl_->resetRequested) { c->impl_->resetRequested = true; requestReset(sid); }
                    if (bits == 3) finishChannel(c);
                }
                if (bits == 3) { retired.erase(sid); resets.erase(sid); }
            }
        }
        else if (n.sn_header.sn_type == SCTP_PARTIAL_DELIVERY_EVENT && size >= sizeof(sctp_pdapi_event))
        {
            const auto found = partial.find(static_cast<std::uint16_t>(n.sn_pdapi_event.pdapi_stream));
            if (found != partial.end()) { receiveBytes -= found->second.data.size(); partial.erase(found); }
        }
        else if (n.sn_header.sn_type == SCTP_SEND_FAILED_EVENT && size >= sizeof(sctp_send_failed_event))
        {
            if (auto c = find(n.sn_send_failed_event.ssfe_info.snd_sid)) c->impl_->error(net::error::timed_out, "SCTP could not deliver a message");
        }
    }
    void receive()
    {
        if (!socket || closing || current.paused || current.state == DataChannelTransportState::Failed) return;
        for (unsigned count = 0; count < 64 && socket; ++count)
        {
            sctp_rcvinfo info{}; socklen_t infoSize = sizeof(info); unsigned infoType = 0; int flags = MSG_PEEK;
            const auto peek = usrsctp_recvv(socket, receiveBuffer.data(), 1, nullptr, nullptr, &info, &infoSize, &infoType, &flags);
            if (peek < 0) { if (errno != EWOULDBLOCK && errno != EAGAIN) fail(socketError(), "SCTP receive failed"); break; }
            if (peek == 0) { fail(net::error::connection_reset, "SCTP association closed"); break; }
            const bool controlEvent = flags & MSG_NOTIFICATION;
            if (!controlEvent && (receiveBytes >= config.maxBufferedReceiveBytes || receiveMessages >= config.maxBufferedReceiveMessages)) break;
            // 剩余容量不足完整分片时做部分读取，保留 native 接收窗口背压，不能误判合法消息超限。
            // notification 不占应用消息额度，应完整读取，不能随剩余应用空间截断。
            const auto available = controlEvent ? receiveBuffer.size() : std::min(receiveBuffer.size(), config.maxBufferedReceiveBytes - receiveBytes);
            info = {}; infoSize = sizeof(info); infoType = 0; flags = 0;
            const auto length = usrsctp_recvv(socket, receiveBuffer.data(), available, nullptr, nullptr, &info, &infoSize, &infoType, &flags);
            if (length < 0) { if (errno != EWOULDBLOCK && errno != EAGAIN) fail(socketError(), "SCTP receive failed"); break; }
            if (length == 0) { fail(net::error::connection_reset, "SCTP association closed"); break; }
            if (flags & MSG_NOTIFICATION) { notification(receiveBuffer.data(), static_cast<std::size_t>(length)); continue; }
            if (infoType != SCTP_RECVV_RCVINFO || infoSize < sizeof(info)) { fail(net::error::fault, "missing SCTP receive metadata"); break; }
            const auto sid = info.rcv_sid; const auto ppid = ntohl(info.rcv_ppid);
            auto& p = partial[sid];
            if (!p.data.empty() && (p.ppid != ppid || p.ssn != info.rcv_ssn)) { fail(net::error::fault, "inconsistent SCTP message fragments"); break; }
            p.ppid = ppid; p.ssn = info.rcv_ssn;
            const auto limit = ppid == Control ? MaxDcep : (ppid == EmptyText || ppid == EmptyBinary) ? 1 : parameters->localMaxMessageSize;
            if (static_cast<std::size_t>(length) > limit - std::min(limit, p.data.size()) || static_cast<std::size_t>(length) > config.maxBufferedReceiveBytes - receiveBytes)
            { fail(net::error::message_size, "SCTP reassembly limit exceeded"); break; }
            p.data.insert(p.data.end(), receiveBuffer.begin(), receiveBuffer.begin() + length); receiveBytes += static_cast<std::size_t>(length);
            if (flags & MSG_EOR)
            {
                auto bytes = std::move(p.data); partial.erase(sid); receiveBytes -= bytes.size();
                message(sid, ppid, std::move(bytes));
            }
            else if (p.data.size() == limit) { fail(net::error::message_size, "SCTP message exceeds local limit"); break; }
        }
        publish();
    }
    void checkTimeouts()
    {
        const auto now = Clock::now();
        if (current.state == DataChannelTransportState::Connecting && !current.paused && now - connectedAt >= config.connectTimeout)
        { fail(net::error::timed_out, "SCTP association handshake timed out"); return; }
        for (const auto& c : channels)
        {
            auto& v = *c->impl_;
            if (v.current.state == DataChannelState::Connecting && v.opened && !current.paused && now - v.openedAt >= config.channelOpenTimeout)
            { v.error(net::error::timed_out, "DCEP handshake timed out"); closeChannel(c->impl_, {}); }
            if (v.current.state == DataChannelState::Closing)
            {
                if (v.resetRequested && !v.resetSubmitted && v.current.config.id) requestReset(*v.current.config.id);
                if (now - v.closingAt >= config.channelCloseTimeout) finishChannel(c, net::error::timed_out);
            }
        }
    }
    void start(Parameters p, Completion done)
    {
        if (closing || current.state == DataChannelTransportState::Failed) { complete(done, net::error::operation_aborted); return; }
        if (p.generation < current.generation) { complete(done, net::error::operation_aborted); return; }
        if ((p.localSetup != SdpSetupRole::Active && p.localSetup != SdpSetupRole::Passive) || !p.localPort || !p.remotePort ||
            !p.localMaxMessageSize || p.localMaxMessageSize > config.maxMessageSize || p.maxPacketSize < 512 || p.maxPacketSize > 65507)
        { complete(done, net::error::invalid_argument); return; }
        if (parameters && (p.localPort != parameters->localPort || p.remotePort != parameters->remotePort || p.remoteIdentity != parameters->remoteIdentity))
        { complete(done, net::error::already_started); return; }
        const bool initial = !parameters;
        if (initial || current.paused || p.generation != current.generation)
            UAV_LOG_INFO << "[WebRTC][SCTP " << this << "][g=" << p.generation << "] " << (initial ? "start" : "resume")
                << " local_port=" << p.localPort << " remote_port=" << p.remotePort << " mtu=" << p.maxPacketSize
                << " local_message_limit=" << p.localMaxMessageSize << " remote_message_limit=" << p.remoteMaxMessageSize;
        if (initial) allocationRole = p.localSetup;
        current.generation = p.generation; current.paused = false;
        current.maxMessageSize = static_cast<std::size_t>(std::min<std::uint64_t>(config.maxMessageSize, p.remoteMaxMessageSize ? p.remoteMaxMessageSize : config.maxMessageSize));
        parameters = std::move(p);
        ErrorCode ec;
        if (initial)
        {
            connectedAt = Clock::now();
            try { ec = makeSocket(); } catch (const std::exception&) { ec = net::error::fault; }
            if (ec) { fail(ec, "could not initialize SCTP association"); complete(done, ec); return; }
            state(DataChannelTransportState::Connecting);
        }
        else
        {
            { std::lock_guard<std::mutex> lock(route->mutex); route->mtu = parameters->maxPacketSize; }
            ec = mtu(parameters->maxPacketSize);
        }
        for (const auto& c : channels) { c->impl_->current.maxMessageSize = current.maxMessageSize; c->impl_->publish(); }
        publish(); schedule(); sendWire(); pump(); receive(); complete(done, ec);
    }
    void finishClose()
    {
        if (!closing || !closePrepared || closed || sendingWire) return;
        { std::lock_guard<std::mutex> lock(sendGate); if (reservedSendMessages) return; }
        if (route) { std::lock_guard<std::mutex> lock(route->mutex); if (route->packets) return; }
        closed = true; publish();
        UAV_LOG_INFO << "[WebRTC][SCTP " << this << "][g=" << current.generation << "] close completed channels=" << current.channels;
        auto callbacks = std::move(closeCallbacks); for (const auto& done : callbacks) complete(done);
    }
    void close(Completion done)
    {
        if (closed) { complete(done); return; }
        if (done) closeCallbacks.push_back(std::move(done));
        if (closing) return;
        { std::lock_guard<std::mutex> lock(sendGate); acceptingSends = false; }
        closing = true; current.paused = true; timer.cancel(); destroySocket();
        for (const auto& c : channels) finishChannel(c, net::error::operation_aborted);
        // Sender 正在使用的队首由其回调归还，其余尚未提交的报文直接取消。
        if (sendingWire) while (wire.size() > 1) { releaseWire(wire.back().size()); wire.pop_back(); }
        else while (!wire.empty()) { releaseWire(wire.front().size()); wire.pop_front(); }
        partial.clear(); receiveBytes = 0;
        state(DataChannelTransportState::Closed); closePrepared = true; finishClose();
    }
};

DataChannel::DataChannel(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
DataChannel::~DataChannel() = default;
DataChannel::Snapshot DataChannel::snapshot() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->published; }
void DataChannel::setCallbacks(Callbacks callbacks)
{
    const auto self = impl_; net::post(self->strand, [self, callbacks = std::move(callbacks)]() mutable {
        self->callbacks = std::move(callbacks);
        if (self->current.state == DataChannelState::Open && self->callbacks.onOpen) self->callbacks.onOpen();
        if (self->current.state == DataChannelState::Closed && self->callbacks.onClosed) self->callbacks.onClosed();
        if (auto owner = self->owner.lock()) { owner->flushMessages(self); owner->receive(); }
    });
}
void DataChannel::sendText(std::string text, SendHandler done)
{
    const auto self = impl_;
    if (auto owner = self->owner.lock()) owner->submit(self, Bytes(text.begin(), text.end()), false, std::move(done));
    else net::post(self->strand, [done = std::move(done)] { if (done) done(net::error::operation_aborted, 0); });
}
void DataChannel::sendBinary(Bytes bytes, SendHandler done)
{
    const auto self = impl_;
    if (auto owner = self->owner.lock()) owner->submit(self, std::move(bytes), true, std::move(done));
    else net::post(self->strand, [done = std::move(done)] { if (done) done(net::error::operation_aborted, 0); });
}
void DataChannel::setBufferedAmountLowThreshold(std::size_t threshold, Completion done)
{
    const auto self = impl_; net::post(self->strand, [self, threshold, done = std::move(done)] {
        self->current.config.bufferedAmountLowThreshold = threshold; self->publish(); complete(done);
    });
}
void DataChannel::close(Completion done)
{
    const auto self = impl_; net::post(self->strand, [self, done = std::move(done)]() mutable {
        if (auto owner = self->owner.lock()) owner->closeChannel(self, std::move(done));
        else { self->state(DataChannelState::Closed); complete(done); }
    });
}
DataChannelTransport::DataChannelTransport(net::io_context& io, Sender sender) : DataChannelTransport(io, std::move(sender), Config{}) {}
DataChannelTransport::DataChannelTransport(net::io_context& io, Sender sender, Config config, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(io, std::move(sender), std::move(config), std::move(callbacks))) {}
DataChannelTransport::~DataChannelTransport() { close(); }
DataChannelTransport::Snapshot DataChannelTransport::snapshot() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->published; }
void DataChannelTransport::createDataChannel(DataChannelConfig config, CreateHandler done) { createDataChannel(std::move(config), {}, std::move(done)); }
void DataChannelTransport::createDataChannel(DataChannelConfig config, DataChannel::Callbacks callbacks, CreateHandler done)
{
    const auto self = impl_; net::post(self->strand, [self, config = std::move(config), callbacks = std::move(callbacks), done = std::move(done)]() mutable {
        self->create(std::move(config), std::move(callbacks), std::move(done));
    });
}
void DataChannelTransport::start(Parameters p, Completion done)
{
    const auto self = impl_; net::post(self->strand, [self, p = std::move(p), done = std::move(done)]() mutable { self->start(std::move(p), std::move(done)); });
}
void DataChannelTransport::handlePacket(Bytes bytes, std::uint64_t generation)
{
    const auto self = impl_;
    if (bytes.size() < 12 || bytes.size() > 65507) return;
    const auto size = bytes.size();
    if (self->queuedInputPackets.fetch_add(1) >= self->config.maxPendingWirePackets) { --self->queuedInputPackets; return; }
    if (self->queuedInputBytes.fetch_add(size) + size > self->config.maxPendingWireBytes)
    { self->queuedInputBytes -= size; --self->queuedInputPackets; return; }
    net::post(self->strand, [self, bytes = std::move(bytes), generation, size] {
        self->queuedInputBytes -= size; --self->queuedInputPackets;
        if (self->closing || self->current.paused || !self->socket || generation != self->current.generation || bytes.size() < 12 || bytes.size() > 65507) return;
        usrsctp_conninput(self->address, bytes.data(), bytes.size(), 0); self->receive(); self->pump(); self->sendWire();
    });
}
void DataChannelTransport::pause(std::uint64_t generation)
{
    const auto self = impl_; net::post(self->strand, [self, generation] {
        if (self->closing || generation < self->current.generation) return;
        if (!self->current.paused || generation != self->current.generation)
            UAV_LOG_INFO << "[WebRTC][SCTP " << self.get() << "][g=" << generation << "] paused channels=" << self->current.channels << " queued_bytes=" << self->queuedBytes;
        self->current.generation = generation; self->current.paused = true; self->publish();
    });
}
void DataChannelTransport::close(Completion done)
{
    const auto self = impl_; net::post(self->strand, [self, done = std::move(done)]() mutable { self->close(std::move(done)); });
}
} // namespace webrtc
