#include"udp_socket.h"

#include<boost/asio/bind_cancellation_slot.hpp>
#include<boost/asio/buffer.hpp>
#include<boost/asio/cancellation_signal.hpp>
#include<boost/asio/ip/v6_only.hpp>
#include<boost/asio/post.hpp>
#include<boost/asio/strand.hpp>
#include<boost/system/system_error.hpp>

#include<array>
#include<atomic>
#include<deque>
#include<mutex>
#include<utility>

namespace net = boost::asio;
using Udp = net::ip::udp;

struct UdpSocket::Impl : std::enable_shared_from_this<UdpSocket::Impl>
{
    struct SendRequest
    {
        Bytes payload;
        Endpoint remote;
        SendHandler handler;
        bool connected{false};
    };

    net::strand<net::io_context::executor_type> strand;
    Udp::socket socket;
    const Config config;
    Endpoint local;
    std::atomic<bool> opened{false}, closing{false};

    mutable std::mutex queueMutex;
    std::deque<std::shared_ptr<SendRequest>> queue;
    PendingStats pendingStats;
    bool drainScheduled{false}; // queueMutex 保护，生产者最多投递一个发送启动任务
    std::shared_ptr<SendRequest> active; // 以下状态只在 strand 中访问

    // 可容纳普通 IPv4/IPv6 的完整 UDP 数据报，避免小缓冲导致静默截断。
    // 不支持 IPv6 jumbogram。
    std::array<std::uint8_t, 65536> receiveBuffer{};
    Endpoint sender;
    net::cancellation_signal receiveCancellation;
    bool wantReceive{false}, receivePending{false};
    ReceiveHandler receiveHandler;
    ErrorHandler errorHandler;

    bool closeStarted{false}, closeFinished{false};
    ErrorCode closeError;
    std::vector<CloseHandler> closeHandlers;

    Impl(net::io_context& io, Config options)
        : strand(net::make_strand(io)), socket(strand), config(std::move(options)) {}

    ErrorCode validateRemote(const Endpoint& remote) const
    {
        if (remote.port() == 0) return net::error::invalid_argument;
        if (remote.protocol() != config.localEndpoint.protocol())
            return net::error::address_family_not_supported;
        return {};
    }

    void open(ErrorCode& ec)
    {
        ec.clear();
        if (config.maxPendingBytes == 0 || config.maxPendingDatagrams == 0 ||
            config.receiveBufferBytes < 0 || config.sendBufferBytes < 0)
            ec = net::error::invalid_argument;
        if (!ec && config.remoteEndpoint) ec = validateRemote(*config.remoteEndpoint);
        if (!ec) socket.open(config.localEndpoint.protocol(), ec);
        if (!ec && config.reuseAddress) socket.set_option(net::socket_base::reuse_address(true), ec);
        if (!ec && config.broadcast) socket.set_option(net::socket_base::broadcast(true), ec);
        if (!ec && config.localEndpoint.address().is_v6())
            socket.set_option(net::ip::v6_only(config.ipv6Only), ec);
        if (!ec && config.receiveBufferBytes > 0)
            socket.set_option(net::socket_base::receive_buffer_size(config.receiveBufferBytes), ec);
        if (!ec && config.sendBufferBytes > 0)
            socket.set_option(net::socket_base::send_buffer_size(config.sendBufferBytes), ec);
        if (!ec) socket.bind(config.localEndpoint, ec);
        if (!ec && config.remoteEndpoint) socket.connect(*config.remoteEndpoint, ec);
        if (!ec) local = socket.local_endpoint(ec);
        if (ec)
        {
            ErrorCode ignored;
            socket.close(ignored);
            closing.store(true);
            local = {};
            return;
        }
        opened.store(true);
    }

    void reject(SendHandler handler, ErrorCode ec)
    {
        if (handler) net::post(strand, [handler = std::move(handler), ec] { handler(ec, 0); });
    }

    void submit(Bytes payload, Endpoint remote, bool connected, SendHandler handler)
    {
        ErrorCode ec;
        if (closing.load() || !opened.load()) ec = net::error::operation_aborted;
        else if (connected && !config.remoteEndpoint) ec = net::error::not_connected;
        else ec = validateRemote(remote);
        if (!ec && config.remoteEndpoint && remote != *config.remoteEndpoint)
            ec = net::error::invalid_argument;
        const bool ipv4 = remote.address().is_v4() ||
                          (remote.address().is_v6() && remote.address().to_v6().is_v4_mapped());
        if (!ec && payload.size() > (ipv4 ? 65507u : 65527u)) ec = net::error::message_size;
        if (ec) { reject(std::move(handler), ec); return; }

        bool schedule = false;
        {
            // 在 post 前限流，避免事件循环来不及处理时形成无界任务/数据队列。
            std::lock_guard<std::mutex> lock(queueMutex);
            if (closing.load()) ec = net::error::operation_aborted;
            else if (payload.size() > config.maxPendingBytes ||
                     pendingStats.bytes > config.maxPendingBytes - payload.size() ||
                     pendingStats.datagrams >= config.maxPendingDatagrams)
                ec = net::error::no_buffer_space;
            else
            {
                const auto bytes = payload.size();
                queue.push_back(std::make_shared<SendRequest>(SendRequest{
                    std::move(payload), std::move(remote), std::move(handler), connected}));
                pendingStats.bytes += bytes;
                ++pendingStats.datagrams;
                if (!drainScheduled) { drainScheduled = true; schedule = true; }
            }
        }
        if (ec) reject(std::move(handler), ec);
        else if (schedule) net::post(strand, [self = shared_from_this()] { self->startSend(); });
    }

    void startSend()
    {
        if (closeStarted || active) return;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (queue.empty()) { drainScheduled = false; return; }
            active = std::move(queue.front());
            queue.pop_front();
        }
        auto request = active;
        auto completion = [self = shared_from_this(), request](ErrorCode ec, std::size_t size) {
            if (!ec && size != request->payload.size()) ec = net::error::message_size;
            {
                std::lock_guard<std::mutex> lock(self->queueMutex);
                self->pendingStats.bytes -= request->payload.size();
                --self->pendingStats.datagrams;
            }
            self->active.reset();
            if (!self->closeStarted) self->startSend();
            if (request->handler) request->handler(ec, size);
            self->finishClose();
        };
        // request 持有缓冲，取消完成前不释放；一次只启动一个数据报发送。
        if (request->connected)
            socket.async_send(net::buffer(request->payload), std::move(completion));
        else
            socket.async_send_to(net::buffer(request->payload), request->remote, std::move(completion));
    }

    void receiveNext()
    {
        if (!wantReceive || receivePending || closing.load()) return;
        receivePending = true;
        socket.async_receive_from(net::buffer(receiveBuffer), sender,
            net::bind_cancellation_slot(receiveCancellation.slot(),
                [self = shared_from_this()](ErrorCode ec, std::size_t size) { self->received(ec, size); }));
    }

    void received(ErrorCode ec, std::size_t size)
    {
        receivePending = false;
        if (closeStarted) { finishClose(); return; }
        if (ec)
        {
            if (ec == net::error::operation_aborted) { receiveNext(); return; }
            wantReceive = false; // 不对永久错误无限重试；由调用方决定何时恢复
            auto handler = errorHandler;
            if (handler && !closing.load()) handler(ec);
            return;
        }
        if (!wantReceive || closing.load()) return;
        Datagram datagram{Bytes(receiveBuffer.begin(), receiveBuffer.begin() + size), sender, Clock::now()};
        auto handler = receiveHandler;
        receiveNext(); // 数据已复制到独立所有权的 Datagram，可安全复用接收缓冲
        if (handler) handler(std::move(datagram));
    }

    void beginClose(CloseHandler handler)
    {
        if (closeFinished) { if (handler) handler(closeError); return; }
        if (handler) closeHandlers.push_back(std::move(handler));
        if (!closeStarted)
        {
            closeStarted = true;
            wantReceive = false;
            receiveHandler = {};
            errorHandler = {};
            if (socket.is_open()) socket.close(closeError);
            std::deque<std::shared_ptr<SendRequest>> cancelled;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                cancelled.swap(queue);
                for (const auto& request : cancelled)
                {
                    pendingStats.bytes -= request->payload.size();
                    --pendingStats.datagrams;
                }
                drainScheduled = static_cast<bool>(active);
            }
            for (const auto& request : cancelled)
                if (request->handler) request->handler(net::error::operation_aborted, 0);
        }
        finishClose();
    }

    void finishClose()
    {
        if (!closeStarted || closeFinished || active || receivePending) return;
        closeFinished = true;
        auto handlers = std::move(closeHandlers);
        for (auto& handler : handlers) handler(closeError);
    }
};

UdpSocket::UdpSocket(net::io_context& io) : UdpSocket(io, Config{}) {}
UdpSocket::UdpSocket(net::io_context& io, const Config& config)
    : impl_(std::make_shared<Impl>(io, config))
{
    ErrorCode ec;
    impl_->open(ec);
    if (ec) throw boost::system::system_error(ec, "UdpSocket open/bind/connect");
}
UdpSocket::UdpSocket(net::io_context& io, const Config& config, ErrorCode& ec)
    : impl_(std::make_shared<Impl>(io, config))
{
    impl_->open(ec);
}
UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::isOpen() const { return impl_->opened.load() && !impl_->closing.load(); }
UdpSocket::Endpoint UdpSocket::localEndpoint() const { return impl_->local; }
std::optional<UdpSocket::Endpoint> UdpSocket::remoteEndpoint() const { return impl_->config.remoteEndpoint; }
UdpSocket::PendingStats UdpSocket::pending() const
{
    std::lock_guard<std::mutex> lock(impl_->queueMutex);
    return impl_->pendingStats;
}
void UdpSocket::startReceive(ReceiveHandler onReceive, ErrorHandler onError)
{
    net::post(impl_->strand, [self = impl_, onReceive = std::move(onReceive), onError = std::move(onError)]() mutable {
        if (self->closing.load()) { if (onError) onError(net::error::operation_aborted); return; }
        if (!onReceive) { if (onError) onError(net::error::invalid_argument); return; }
        self->receiveHandler = std::move(onReceive);
        self->errorHandler = std::move(onError);
        self->wantReceive = true;
        self->receiveNext();
    });
}
void UdpSocket::stopReceive()
{
    net::post(impl_->strand, [self = impl_] {
        self->wantReceive = false;
        self->receiveHandler = {};
        self->errorHandler = {};
        if (self->receivePending) self->receiveCancellation.emit(net::cancellation_type::total);
    });
}
void UdpSocket::asyncSendTo(Bytes payload, const Endpoint& remote, SendHandler onSend)
{
    impl_->submit(std::move(payload), remote, false, std::move(onSend));
}
void UdpSocket::asyncSend(Bytes payload, SendHandler onSend)
{
    impl_->submit(std::move(payload), impl_->config.remoteEndpoint.value_or(Endpoint{}), true, std::move(onSend));
}
void UdpSocket::close(CloseHandler onClose)
{
    const bool alreadyClosing = impl_->closing.exchange(true);
    impl_->opened.store(false);
    if (alreadyClosing && !onClose) return;
    net::post(impl_->strand, [self = impl_, onClose = std::move(onClose)]() mutable {
        self->beginClose(std::move(onClose));
    });
}
