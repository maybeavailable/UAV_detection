#include"common/udp_socket.h"

#include<boost/asio/executor_work_guard.hpp>
#include<boost/system/system_error.hpp>

#include<atomic>
#include<condition_variable>
#include<future>
#include<iostream>
#include<mutex>
#include<stdexcept>
#include<string>
#include<thread>

namespace net = boost::asio;
using Bytes = UdpSocket::Bytes;
using ErrorCode = UdpSocket::ErrorCode;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

UdpSocket::Config loopback(bool ipv6 = false)
{
    UdpSocket::Config config;
    config.localEndpoint = {net::ip::make_address(ipv6 ? "::1" : "127.0.0.1"), 0};
    return config;
}
template<class Predicate>
void runUntil(net::io_context& io, Predicate done)
{
    const auto deadline = UdpSocket::Clock::now() + 2s;
    while (!done() && UdpSocket::Clock::now() < deadline)
    {
        io.restart();
        io.run_for(2ms);
    }
    CHECK(done());
}
void closeBoth(net::io_context& io, UdpSocket& first, UdpSocket& second)
{
    unsigned closed = 0;
    first.close([&](ErrorCode ec) { CHECK(!ec); ++closed; });
    second.close([&](ErrorCode ec) { CHECK(!ec); ++closed; });
    runUntil(io, [&] { return closed == 2; });
}

void datagramTests(bool ipv6)
{
    net::io_context io;
    ErrorCode ec;
    UdpSocket receiver(io, loopback(ipv6), ec);
    if (ipv6 && (ec == net::error::address_family_not_supported ||
                 ec == boost::system::errc::make_error_condition(boost::system::errc::address_not_available)))
    {
        std::cout << "SKIP: IPv6 loopback unavailable\n";
        return;
    }
    CHECK(!ec && receiver.isOpen());
    UdpSocket first(io, loopback(ipv6)), second(io, loopback(ipv6));
    CHECK(first.localEndpoint().port() != 0 && first.localEndpoint() != second.localEndpoint());
    std::vector<UdpSocket::Datagram> received;
    unsigned sent = 0;
    receiver.startReceive([&](auto datagram) { received.push_back(std::move(datagram)); });
    const Bytes binary{0, 1, 0xFF, 0, 2};
    Bytes large(60000);
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<std::uint8_t>(i);
    const auto before = UdpSocket::Clock::now();
    first.asyncSendTo(binary, receiver.localEndpoint(), [&](ErrorCode error, std::size_t size) {
        CHECK(!error && size == binary.size()); ++sent;
    });
    second.asyncSendTo({}, receiver.localEndpoint(), [&](ErrorCode error, std::size_t size) {
        CHECK(!error && size == 0); ++sent;
    });
    first.asyncSendTo(large, receiver.localEndpoint(), [&](ErrorCode error, std::size_t size) {
        CHECK(!error && size == large.size()); ++sent;
    });
    runUntil(io, [&] { return received.size() == 3 && sent == 3; });
    bool sawBinary = false, sawEmpty = false, sawLarge = false;
    for (const auto& datagram : received)
    {
        CHECK(datagram.receivedAt >= before);
        CHECK(datagram.receivedAt <= UdpSocket::Clock::now());
        if (datagram.payload == binary) { sawBinary = true; CHECK(datagram.remoteEndpoint == first.localEndpoint()); }
        else if (datagram.payload.empty()) { sawEmpty = true; CHECK(datagram.remoteEndpoint == second.localEndpoint()); }
        else { sawLarge = datagram.payload == large; CHECK(datagram.remoteEndpoint == first.localEndpoint()); }
    }
    CHECK(sawBinary && sawEmpty && sawLarge);
    CHECK(first.pending().bytes == 0 && first.pending().datagrams == 0);
    closeBoth(io, receiver, first);
    second.close();
    io.restart(); io.poll();
}

void connectedTests()
{
    net::io_context io;
    UdpSocket server(io, loopback()), other(io, loopback());
    auto config = loopback();
    config.remoteEndpoint = server.localEndpoint();
    UdpSocket client(io, config);
    CHECK(client.remoteEndpoint() == server.localEndpoint());
    bool echoed = false, rejected = false;
    server.startReceive([&](auto datagram) { server.asyncSendTo(std::move(datagram.payload), datagram.remoteEndpoint); });
    client.startReceive([&](auto datagram) {
        CHECK(datagram.payload == Bytes({1, 0, 2}) && datagram.remoteEndpoint == server.localEndpoint());
        echoed = true;
    });
    other.asyncSendTo({99}, client.localEndpoint()); // 固定对端 socket 不接收此数据报
    client.asyncSendTo({1}, other.localEndpoint(), [&](ErrorCode ec, std::size_t size) {
        CHECK(ec == net::error::invalid_argument && size == 0); rejected = true;
    });
    client.asyncSend({1, 0, 2});
    runUntil(io, [&] { return echoed && rejected; });
    closeBoth(io, client, server);
    other.close(); io.restart(); io.poll();
}

void queueAndValidationTests()
{
    net::io_context io;
    UdpSocket receiver(io, loopback());
    auto config = loopback();
    config.maxPendingBytes = 3;
    config.maxPendingDatagrams = 2;
    UdpSocket sender(io, config);
    unsigned sent = 0, full = 0;
    std::vector<Bytes> received;
    receiver.startReceive([&](auto datagram) { received.push_back(std::move(datagram.payload)); });
    auto completion = [&](ErrorCode ec, std::size_t size) {
        if (ec) { CHECK(ec == net::error::no_buffer_space && size == 0); ++full; }
        else ++sent;
    };
    sender.asyncSendTo({1, 2}, receiver.localEndpoint(), completion);
    sender.asyncSendTo({3}, receiver.localEndpoint(), completion);
    sender.asyncSendTo({4}, receiver.localEndpoint(), completion);
    CHECK(sent == 0 && full == 0); // 拒绝也不得内联调用回调
    CHECK(sender.pending().bytes == 3 && sender.pending().datagrams == 2);
    runUntil(io, [&] { return sent == 2 && full == 1 && received.size() == 2; });
    CHECK(received[0] == Bytes({1, 2}) && received[1] == Bytes({3}));
    CHECK(sender.pending().bytes == 0 && sender.pending().datagrams == 0);

    unsigned errors = 0;
    sender.asyncSend({1}, [&](ErrorCode ec, std::size_t) { CHECK(ec == net::error::not_connected); ++errors; });
    sender.asyncSendTo(Bytes(65508), receiver.localEndpoint(), [&](ErrorCode ec, std::size_t) {
        CHECK(ec == net::error::message_size); ++errors;
    });
    sender.asyncSendTo({1}, {net::ip::make_address("::1"), 12345}, [&](ErrorCode ec, std::size_t) {
        CHECK(ec == net::error::address_family_not_supported); ++errors;
    });
    sender.asyncSendTo({1}, {}, [&](ErrorCode ec, std::size_t) { CHECK(ec == net::error::invalid_argument); ++errors; });
    sender.startReceive({}, [&](ErrorCode ec) { CHECK(ec == net::error::invalid_argument); ++errors; });
    runUntil(io, [&] { return errors == 5; });

    // 空包也占用包数配额，不能绕过队列上限。
    sender.asyncSendTo({}, receiver.localEndpoint(), completion);
    sender.asyncSendTo({}, receiver.localEndpoint(), completion);
    sender.asyncSendTo({}, receiver.localEndpoint(), completion);
    CHECK(sender.pending().bytes == 0 && sender.pending().datagrams == 2);
    runUntil(io, [&] { return sent == 4 && full == 2; });
    closeBoth(io, sender, receiver);
}

void cancellationTests()
{
    net::io_context io;
    UdpSocket sender(io, loopback()), receiver(io, loopback());
    unsigned old = 0, fresh = 0;
    receiver.startReceive([&](auto) { ++old; });
    io.run_for(1ms);
    receiver.stopReceive();
    receiver.startReceive([&](auto datagram) { CHECK(datagram.payload == Bytes({7})); ++fresh; });
    receiver.startReceive([&](auto datagram) { CHECK(datagram.payload == Bytes({7})); ++fresh; });
    io.restart(); io.run_for(1ms);
    sender.asyncSendTo({7}, receiver.localEndpoint());
    runUntil(io, [&] { return fresh == 1; });
    CHECK(old == 0);
    receiver.stopReceive();

    unsigned completed = 0, closed = 0, late = 0;
    for (unsigned i = 0; i < 64; ++i)
        sender.asyncSendTo(Bytes(100, 1), receiver.localEndpoint(), [&](ErrorCode ec, std::size_t size) {
            CHECK((!ec && size == 100) || (ec == net::error::operation_aborted && size == 0));
            ++completed;
        });
    sender.close([&](ErrorCode ec) { CHECK(!ec && completed == 64); ++closed; });
    sender.close([&](ErrorCode ec) { CHECK(!ec && completed == 64); ++closed; });
    CHECK(!sender.isOpen());
    sender.asyncSendTo({9}, receiver.localEndpoint(), [&](ErrorCode ec, std::size_t size) {
        CHECK(ec == net::error::operation_aborted && size == 0); ++late;
    });
    runUntil(io, [&] { return completed == 64 && closed == 2 && late == 1; });
    CHECK(sender.pending().bytes == 0 && sender.pending().datagrams == 0);
    receiver.close([&](ErrorCode ec) { CHECK(!ec); ++closed; });
    runUntil(io, [&] { return closed == 3; });

    // 外层对象可先销毁；在途接收/发送仍持有状态和缓冲，完成回调只调用一次。
    unsigned destroyedSend = 0;
    {
        auto socket = std::make_unique<UdpSocket>(io, loopback());
        socket->startReceive([](auto) { CHECK(false); });
        io.restart(); io.run_for(1ms);
        socket->asyncSendTo({1}, receiver.localEndpoint(), [&](ErrorCode ec, std::size_t) {
            CHECK(!ec || ec == net::error::operation_aborted); ++destroyedSend;
        });
    }
    runUntil(io, [&] { return destroyedSend == 1; });
}

void configTests()
{
    net::io_context io;
    ErrorCode ec;
    auto config = loopback();
    config.maxPendingBytes = 0;
    UdpSocket invalid(io, config, ec);
    CHECK(ec == net::error::invalid_argument && !invalid.isOpen());
    bool threw = false;
    try { UdpSocket bad(io, config); }
    catch (const boost::system::system_error& error) { threw = error.code() == net::error::invalid_argument; }
    CHECK(threw);
    UdpSocket bound(io, loopback());
    config = loopback(); config.localEndpoint = bound.localEndpoint();
    UdpSocket conflict(io, config, ec);
    CHECK(ec == net::error::address_in_use && !conflict.isOpen());
    unsigned closed = 0;
    invalid.close([&](ErrorCode error) { CHECK(!error); ++closed; });
    runUntil(io, [&] { return closed == 1; });
}

void multithreadTests()
{
    net::io_context io;
    auto guard = net::make_work_guard(io);
    auto config = loopback(); config.receiveBufferBytes = 1024 * 1024;
    UdpSocket sender(io, config), receiver(io, config);
    std::atomic<unsigned> sent{0}, received{0}, errors{0}, overlap{0};
    std::atomic<bool> sendingCallback{false}, receivingCallback{false};
    std::mutex mutex;
    std::condition_variable ready;
    receiver.startReceive([&](auto datagram) {
        if (receivingCallback.exchange(true)) ++overlap;
        if (datagram.payload.size() != 32) ++errors;
        ++received;
        receivingCallback.store(false);
        std::lock_guard<std::mutex> lock(mutex);
        ready.notify_all();
    }, [&](ErrorCode) { ++errors; });
    std::vector<std::future<void>> runners;
    for (unsigned i = 0; i < 3; ++i) runners.push_back(std::async(std::launch::async, [&] { io.run(); }));
    std::vector<std::thread> producers;
    for (unsigned i = 0; i < 4; ++i)
        producers.emplace_back([&] {
            for (unsigned j = 0; j < 64; ++j)
                sender.asyncSendTo(Bytes(32, static_cast<std::uint8_t>(j)), receiver.localEndpoint(),
                    [&](ErrorCode ec, std::size_t size) {
                        if (sendingCallback.exchange(true)) ++overlap;
                        if (ec || size != 32) ++errors;
                        ++sent;
                        sendingCallback.store(false);
                        std::lock_guard<std::mutex> lock(mutex);
                        ready.notify_all();
                    });
        });
    for (auto& producer : producers) producer.join();
    bool done;
    {
        std::unique_lock<std::mutex> lock(mutex);
        done = ready.wait_for(lock, 2s, [&] { return sent.load() == 256 && received.load() == 256; });
    }
    sender.close(); receiver.close(); guard.reset();
    for (auto& runner : runners) runner.get();
    CHECK(done && errors == 0 && overlap == 0);
    CHECK(sender.pending().datagrams == 0 && sender.pending().bytes == 0);
}

int main()
{
    try
    {
        datagramTests(false);
        datagramTests(true);
        connectedTests();
        queueAndValidationTests();
        cancellationTests();
        configTests();
        multithreadTests();
        std::cout << "PASS: UDP IPv4/IPv6, datagram boundaries, peers, bounded queues, cancellation and threads\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
