#include"ptz/ptz_protocol.h"

#include<fcntl.h>
#include<poll.h>
#include<sys/ioctl.h>
#include<termios.h>
#include<unistd.h>

#include<chrono>
#include<condition_variable>
#include<cstdlib>
#include<future>
#include<iostream>
#include<limits>
#include<mutex>
#include<sstream>
#include<stdexcept>
#include<thread>

using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

Bytes hex(const char* text)
{
    Bytes out;
    std::istringstream input(text);
    unsigned value;
    while (input >> std::hex >> value) out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

class PseudoTerminal
{
public:
    std::string port;
    PseudoTerminal()
    {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        CHECK(master_ >= 0);
        CHECK(::grantpt(master_) == 0 && ::unlockpt(master_) == 0);
        const char* name = ::ptsname(master_);
        CHECK(name);
        port = name;
        slave_ = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        CHECK(slave_ >= 0);
    }
    ~PseudoTerminal()
    {
        closeLink();
        if (slave_ >= 0) ::close(slave_);
    }
    void closeLink()
    {
        if (master_ >= 0) { ::close(master_); master_ = -1; }
    }
    void allowReopen() { CHECK(::ioctl(slave_, TIOCNXCL) == 0); }
    void checkConfiguration()
    {
        termios value{};
        CHECK(::tcgetattr(slave_, &value) == 0);
        CHECK(::cfgetispeed(&value) == B115200 && ::cfgetospeed(&value) == B115200);
        CHECK((value.c_cflag & CSIZE) == CS8 && !(value.c_cflag & (PARENB | CSTOPB)));
    }
    Bytes read(std::size_t count)
    {
        Bytes out(count);
        std::size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (offset < count)
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            pollfd p{master_, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            const auto size = ::read(master_, out.data() + offset, count - offset);
            if (size > 0) offset += static_cast<std::size_t>(size);
        }
        return out;
    }
    void expect(const char* text)
    {
        const auto expected = hex(text);
        CHECK(read(expected.size()) == expected);
    }
    bool readable(int milliseconds = 20)
    {
        pollfd p{master_, POLLIN, 0};
        return ::poll(&p, 1, milliseconds) > 0 && (p.revents & POLLIN);
    }
    void send(const char* text, bool fragmented = false)
    {
        const auto bytes = hex(text);
        for (std::size_t offset = 0; offset < bytes.size();)
        {
            const auto count = fragmented ? 1 : bytes.size() - offset;
            const auto size = ::write(master_, bytes.data() + offset, count);
            CHECK(size > 0);
            offset += static_cast<std::size_t>(size);
            if (fragmented) std::this_thread::sleep_for(1ms);
        }
    }
private:
    int master_{-1}, slave_{-1};
};

void sendTests()
{
    PseudoTerminal fake;
    PtzCore core(fake.port, 115200, 2);
    CHECK(!core.isConnected());
    CHECK(!core.stop() && core.lastResult().error == ptz::Error::NotConnected);
    CHECK(!core.setResponseTimeout(0) && core.lastResult().error == ptz::Error::InvalidArgument);
    CHECK(core.connect() && core.isConnected());
    fake.checkConfiguration();
    CHECK(core.stop());
    fake.expect("FF 02 00 00 00 00 02");
    CHECK(core.move(ptz::HorizontalDirection::Left, 64, ptz::VerticalDirection::Up, 32));
    fake.expect("FF 02 00 0C 40 20 6E");
    CHECK(core.setHorizontalAngle(10));
    fake.expect("FF 02 00 4B 03 E8 38");
    CHECK(core.setRealtimeReport(true));
    fake.expect("FF 02 00 09 00 05 10");
    CHECK(core.moveVariableSpeed(ptz::HorizontalDirection::Left, 2.5, ptz::VerticalDirection::Up, 1));
    fake.expect("6E 01 10 AA 0B 01 01 00 00 00 64 02 00 00 00 FA 00 E6");
    CHECK(core.setPosition(10, 20));
    fake.expect("6E 01 10 AA 0B 02 00 00 00 03 E8 00 00 00 07 D0 00 E6");
    CHECK(core.setPositionWithSpeed({true, 1, 10}, {false, 2.5, 20}));
    fake.expect("6E 01 10 AA 0B 03 01 00 64 03 E8 00 00 FA 07 D0 00 E6");
    CHECK(!core.setPreset(256) && core.lastResult().error == ptz::Error::InvalidArgument);
    CHECK(!core.setVerticalAngle(180) && core.lastResult().error == ptz::Error::InvalidArgument);
    CHECK(!core.move(ptz::HorizontalDirection::Right, 65, ptz::VerticalDirection::None, 0));
    auto invalid = ptz::makeStop();
    invalid[6] ^= 1;
    CHECK(core.send(invalid).error == ptz::Error::InvalidArgument);
    auto extended = ptz::makePosition(10, 20);
    extended[17] = 0;
    CHECK(core.send(extended).error == ptz::Error::InvalidArgument);
    extended = ptz::makePosition(10, 20);
    extended[7] = 1; // 保留字节必须为 0
    CHECK(core.send(extended).error == ptz::Error::InvalidArgument);
    extended = ptz::makeVariableSpeed(ptz::HorizontalDirection::Right, 1, ptz::VerticalDirection::Up, 1);
    extended[6] = 0; // 文档未定义方向 0
    CHECK(core.send(extended).error == ptz::Error::InvalidArgument);
    CHECK(!fake.readable()); // 参数错误不发送任何数据
    CHECK(core.send(ptz::makeStop(3)));
    fake.expect("FF 03 00 00 00 00 03");
    CHECK(core.send(ptz::makePosition(10, 20)));
    fake.expect("6E 01 10 AA 0B 02 00 00 00 03 E8 00 00 00 07 D0 00 E6");
    core.disconnect();
    CHECK(!core.isConnected());
    PtzCore invalidConfig(fake.port, std::numeric_limits<std::uint32_t>::max());
    CHECK(!invalidConfig.connect() && invalidConfig.lastResult().error == ptz::Error::NotConnected);
}

void queryAndCallbackTests()
{
    PseudoTerminal fake;
    // 回调引用的对象应比 Core 生命周期长，确保析构时接收线程已退出。
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<ptz::AngleReport> reports;
    unsigned frameCount = 0;
    PtzCore core(fake.port, 115200, 2);
    core.setAngleCallback([&](const auto& report) {
        std::lock_guard<std::mutex> lock(mutex);
        reports.push_back(report);
    });
    core.setFrameCallback([&](const auto&) {
        std::lock_guard<std::mutex> lock(mutex);
        ++frameCount;
        ready.notify_all();
    });
    CHECK(core.connect());
    CHECK(core.setResponseTimeout(1000));
    double angle = -1;
    auto query = std::async(std::launch::async, [&] { return core.getHorizontalAngle(angle); });
    fake.expect("FF 02 00 51 00 00 53");
    // 错地址、坏校验不能满足查询；另一轴派发为异步回传。
    fake.send("FF 01 00 59 03 E8 45 FF 02 00 59 03 E8 47 FF 02 00 5B 88 B8 9D");
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(ready.wait_for(lock, 1s, [&] { return frameCount == 1; }));
        CHECK(reports.size() == 1 && reports[0].axis == ptz::Axis::Vertical && reports[0].degrees == 350);
    }
    CHECK(query.wait_for(20ms) == std::future_status::timeout);
    fake.send("FF 02 00 59 03 E8 46", true);
    CHECK(query.get() && angle == 10);
    CHECK(core.lastResult().frame.address == 2);
    auto vertical = std::async(std::launch::async, [&] { return core.getVerticalAngle(angle); });
    fake.expect("FF 02 00 53 00 00 55");
    fake.send("FF 02 00 5B 88 B8 9D");
    CHECK(vertical.get() && angle == 350);
    // 两个实时回传帧粘在一次接收内。
    fake.send("FF 02 00 59 03 E8 46 FF 02 00 5B 88 B8 9D");
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(ready.wait_for(lock, 1s, [&] { return frameCount == 3; }));
        CHECK(reports.size() == 3 && reports[1].degrees == 10 && reports[2].degrees == 350);
    }
    CHECK(core.queryAngle(static_cast<ptz::Axis>(2)).error == ptz::Error::InvalidArgument);
    CHECK(core.queryAngle(ptz::Axis::Horizontal, 0).error == ptz::Error::InvalidArgument);
    CHECK(!fake.readable());

    auto timeout = std::async(std::launch::async, [&] { return core.queryAngle(ptz::Axis::Horizontal, 40); });
    fake.expect("FF 02 00 51 00 00 53");
    CHECK(timeout.get().error == ptz::Error::Timeout);
    CHECK(core.queryAngle(ptz::Axis::Vertical).error == ptz::Error::NeedsReconnect);
    CHECK(core.stop()); // 超时后仍可发送停止
    fake.expect("FF 02 00 00 00 00 02");
    fake.allowReopen();
    CHECK(core.reconnect());
    auto resumed = std::async(std::launch::async, [&] { return core.getHorizontalAngle(angle); });
    fake.expect("FF 02 00 51 00 00 53");
    fake.send("FF 02 00 59 03 E8 46");
    CHECK(resumed.get() && angle == 10);

    angle = 123;
    auto cancelled = std::async(std::launch::async, [&] { return core.getVerticalAngle(angle); });
    fake.expect("FF 02 00 53 00 00 55");
    core.disconnect();
    CHECK(cancelled.wait_for(100ms) == std::future_status::ready);
    CHECK(!cancelled.get() && angle == 123 && core.lastResult().error == ptz::Error::ConnectionLost);
    CHECK(!core.isConnected());
    fake.allowReopen();
    CHECK(core.connect());
    auto lost = std::async(std::launch::async, [&] { return core.queryAngle(ptz::Axis::Vertical); });
    fake.expect("FF 02 00 53 00 00 55");
    fake.closeLink();
    CHECK(lost.wait_for(500ms) == std::future_status::ready);
    CHECK(lost.get().error == ptz::Error::ConnectionLost && !core.isConnected());
}

int main()
{
    try
    {
        sendTests();
        queryAndCallbackTests();
        std::cout << "PASS: PTZ serial commands, queries, callbacks, timeout, reconnect and cancellation\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
