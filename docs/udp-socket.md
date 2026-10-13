# 通用 UDP Socket

`src/common/udp_socket.h/.cpp` 使用 Boost.Asio 1.88 和 C++17。`UdpSocket` 只传输 UDP 数据报，协议解析由 MoQ/QUIC、WebRTC 等上层模块处理。调用方提供 `io_context`，可供多个模块共享，也可由多个线程运行；同一 Socket 的 I/O 和回调由 strand 串行执行。

```cpp
#include "common/udp_socket.h"
#include <iostream>

boost::asio::io_context io;
UdpSocket::Config config;
config.localEndpoint = {boost::asio::ip::make_address("0.0.0.0"), 0};
UdpSocket socket(io, config); // 同步绑定；失败抛 boost::system::system_error

socket.startReceive(
    [](UdpSocket::Datagram datagram) {
        // 将 payload、remoteEndpoint、receivedAt 交给所属协议的处理器。
        // Datagram 拥有 payload，可 move 到其他队列。
    },
    [](UdpSocket::ErrorCode ec) { std::cerr << ec.message() << '\n'; });

UdpSocket::Endpoint peer{boost::asio::ip::make_address("127.0.0.1"), 9000};
socket.asyncSendTo({0x01, 0x02}, peer,
    [](UdpSocket::ErrorCode ec, std::size_t bytes) {
        // ec 为空表示 bytes 字节已交给本机网络栈。
    });
io.run(); // 接收循环会保持运行；控制逻辑调用 close() 后才会结束。
```

需要错误码构造时，使用 `UdpSocket socket(io, config, ec)`，随后检查 `ec` 和 `isOpen()`。`localEndpoint()` 返回实际分配的端口。

默认采用未连接的 Socket，同一个本地端口可通过 `asyncSendTo` 与多个对端通信，接收时附带来源端点。设置 `Config::remoteEndpoint` 后会执行 UDP connect，内核过滤其他来源；可使用 `asyncSend`，而 `asyncSendTo` 只允许指定同一对端。

IPv6 使用 `{boost::asio::ip::udp::v6(), port}` 或具体 IPv6 地址作为本地端点。默认 `ipv6Only=true`；双栈模式设为 false，IPv4 对端须表示为 IPv4-mapped IPv6 地址。数据报允许为空，普通 IPv4/IPv6 的最大发送负载分别为 65507/65527 字节；IPv4-mapped 对端按 IPv4 上限检查。不支持 IPv6 jumbogram。接收缓冲固定 65536 字节，避免应用缓冲过小造成截断。

发送按值取得数据所有权，使用 `std::move(payload)` 可避免复制。发送队列在投递到事件循环前就检查 `maxPendingBytes` 和 `maxPendingDatagrams`，统计包含在途发送。队列满时通过发送回调返回 `no_buffer_space`，由上层决定丢弃、限速或稍后重试；不会阻塞生产者等待空间。失败及成功回调均异步执行。

`startReceive` 可替换回调，始终只保留一个在途接收；`stopReceive` 只取消接收，保留发送能力。接收错误暂停接收循环并调用错误回调，调用方可再次启动。回调在 strand 中执行，不应阻塞或抛异常。

`close(onClose)` 立即拒绝新发送，随后关闭底层 Socket。排队发送收到 `operation_aborted`，在途发送可能已成功，仍会完成一次回调。关闭回调等已接受发送的完成回调和在途接收处理结束后执行。重复关闭安全，关闭后使用新实例建立 Socket。析构会请求关闭；`io_context` 必须比 Socket 活得更久，并继续运行到取消及回调完成。`io_context` 若已停止，需 `restart()` 后继续运行。回调捕获的数据也必须保持有效。

独立测试：

```bash
cmake -S tests/udp_socket -B /tmp/uav-udp-socket-build
cmake --build /tmp/uav-udp-socket-build --parallel 2
ctest --test-dir /tmp/uav-udp-socket-build --output-on-failure
```
