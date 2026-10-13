#pragma once

#include<boost/asio/io_context.hpp>
#include<boost/asio/ip/udp.hpp>
#include<boost/system/error_code.hpp>

#include<chrono>
#include<cstddef>
#include<cstdint>
#include<functional>
#include<memory>
#include<optional>
#include<vector>

// 通用 UDP 数据报传输，供 MoQ/QUIC、WebRTC 等上层协议复用。
// 调用方提供并运行 io_context，本类不创建线程。声明在此，实现位于 udp_socket.cpp。
class UdpSocket
{
public:
    using Endpoint = boost::asio::ip::udp::endpoint;
    using ErrorCode = boost::system::error_code;
    using Bytes = std::vector<std::uint8_t>;
    using Clock = std::chrono::steady_clock;

    struct Config
    {
        Endpoint localEndpoint{boost::asio::ip::udp::v4(), 0}; // 端口 0 由系统分配
        std::optional<Endpoint> remoteEndpoint; // 有值时执行 UDP connect，内核只接收此对端
        bool reuseAddress{false};
        bool broadcast{false};
        bool ipv6Only{true}; // 仅对 IPv6 生效；双栈发送 IPv4 时使用 IPv4-mapped IPv6 地址
        int receiveBufferBytes{0}, sendBufferBytes{0}; // 0 使用系统默认
        std::size_t maxPendingBytes{2 * 1024 * 1024};
        std::size_t maxPendingDatagrams{4096}; // 两个上限均包含正在发送的数据报
    };
    struct Datagram
    {
        Bytes payload; // 拥有完整数据，可在回调中 move 到上层队列；允许空数据报
        Endpoint remoteEndpoint;
        Clock::time_point receivedAt; // 用户态接收完成时间，非内核时间戳
    };
    struct PendingStats { std::size_t bytes{0}, datagrams{0}; };

    using ReceiveHandler = std::function<void(Datagram)>;
    using ErrorHandler = std::function<void(ErrorCode)>;
    using SendHandler = std::function<void(ErrorCode, std::size_t)>;
    using CloseHandler = std::function<void(ErrorCode)>;

    // 构造时同步 open/bind/connect。无 ec 的版本失败时抛 boost::system::system_error。
    explicit UdpSocket(boost::asio::io_context& io);
    UdpSocket(boost::asio::io_context& io, const Config& config);
    UdpSocket(boost::asio::io_context& io, const Config& config, ErrorCode& ec);
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool isOpen() const;
    Endpoint localEndpoint() const; // 实际绑定端点，关闭后仍保留；构造失败时为空端点
    std::optional<Endpoint> remoteEndpoint() const;
    PendingStats pending() const;

    // 以下接口可从任意线程调用；同一实例的 I/O 及回调由 strand 串行执行。
    // 回调不会在调用接口时内联执行，且不得抛异常。io_context 必须活到所有回调结束。
    // 重复 startReceive 替换回调，不增加并行读；接收错误暂停循环，之后可再次启动。
    void startReceive(ReceiveHandler onReceive, ErrorHandler onError = {});
    void stopReceive(); // 仅取消接收，不取消发送

    // 按值取得发送缓冲所有权；传入 std::move(payload) 可避免复制。
    // sendTo 支持多个对端，适用于共享端口；固定对端模式仅允许发送给配置中的对端。
    // send 需要配置 remoteEndpoint。成功仅表示交给本机网络栈，不保证远端交付。
    // 队列满返回 no_buffer_space，超长返回 message_size，关闭后返回 operation_aborted。
    void asyncSendTo(Bytes payload, const Endpoint& remote, SendHandler onSend = {});
    void asyncSend(Bytes payload, SendHandler onSend = {});

    // 立即拒绝新发送，随后在 strand 中关闭并取消接收和排队发送。
    // onClose 在已接受发送的完成回调及在途接收处理结束后执行；重复关闭安全。
    // 析构等同请求 close；调用方须继续运行 io_context 才能完成取消和回调。
    // 关闭后不可重新打开，需要新的 UdpSocket 实例。
    void close(CloseHandler onClose = {});

private:
    struct Impl;
    std::shared_ptr<Impl> impl_; // 异步操作持有状态，防止对象析构后缓冲区失效
};
