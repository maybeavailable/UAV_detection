//=============================================================================
// Urt 串口类自测（用伪终端 pty 模拟串口，无需真实硬件）
//
// 验证内容：
//   1. open/is_open/is_configured 状态正确
//   2. write → 对端收到完全相同的字节
//   3. read / read_some / 超时语义
//   4. transact 一问一答（清缓冲→写→drain→读）
//   5. 并发写：多线程同时写各自的定长帧，验证不串包、不损坏（写锁有效）
//   6. 后台接收线程回调
//   7. 接收线程运行中 close() 不会挂死（生命周期与在途 I/O 不冲突）
//
// 编译运行：
//   g++ -std=c++17 -Wall -Wextra -I src tools/urt_selftest.cpp src/log/log.cpp
//       src/log/RotateFileWriter.cpp -lpthread -o build/urt_selftest
//   ./build/urt_selftest
//
// 说明：pty 上波特率设置会被忽略（cfsetattr 仍会成功），因此本测试验证的是
//       打开/配置/收发/并发这些逻辑；实际波特率、校验位、流控需要接真设备确认。
//=============================================================================
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "common/urt.h"
#include "log/log.h"

// 造一对伪终端；成功返回 true，master 与 slave 路径通过出参返回
static bool makePty(int &master, std::string &slavePath)
{
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
    {
        return false;
    }
    const char *name = ptsname(master);
    if (name == nullptr)
    {
        return false;
    }
    slavePath = name;
    return true;
}

static int g_fail = 0;
#define CHECK(cond, name)                              \
    do {                                               \
        if (cond) { printf("[PASS] %s\n", name); }     \
        else { printf("[FAIL] %s\n", name); g_fail++; } \
    } while (0)

int main()
{
    LogConfig cfg;
    cfg.level = LogLevel::ERROR;      // 自测时压掉 info 日志
    LogManager::configure(cfg);

    // 主流程用一对伪终端：master 端相当于"另一侧的串口设备"。
    // 这里显式 exclusive=false —— 独占（TIOCEXCL）会让 pty master 存在期间
    // slave 无法再次打开，不利于本测试反复 close/open；独占保护在步骤 9 单独验证。
    int master = -1;
    std::string slaveName;
    if (!makePty(master, slaveName))
    {
        printf("创建伪终端失败\n");
        return 1;
    }
    printf("伪终端: master=%d, slave=%s\n\n", master, slaveName.c_str());

    Urt urt;

    // ---------------- 1. 打开 ----------------
    CHECK(urt.open(slaveName, 115200, 8, 'N', 1, false, false) == 0, "open() 打开串口");
    CHECK(urt.is_open(), "is_open() = true");
    CHECK(urt.is_configured(), "is_configured() = true（termios 配置成功）");
    CHECK(urt.device() == std::string(slaveName), "device() 返回设备路径");

    // ---------------- 2. 写 ----------------
    const std::uint8_t tx[] = {0x01, 0x02, 0x03, 0xA5, 0x5A};
    std::uint8_t rx[64] = {0};
    CHECK(urt.write(tx, sizeof(tx)) == static_cast<int>(sizeof(tx)), "write() 返回写入字节数");

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 300 * 1000;
    ::setsockopt(master, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));   // 对 pty 无效也无妨
    int n = static_cast<int>(::read(master, rx, sizeof(rx)));
    CHECK(n == static_cast<int>(sizeof(tx)) && memcmp(rx, tx, sizeof(tx)) == 0,
          "对端收到完全相同的字节");

    // ---------------- 3. 读 / 超时 ----------------
    const std::uint8_t reply[] = {0xAA, 0xBB, 0xCC};
    ::write(master, reply, sizeof(reply));
    n = urt.read(rx, sizeof(reply), 300);
    CHECK(n == 3 && memcmp(rx, reply, 3) == 0, "read() 读满指定长度");

    n = urt.read(rx, sizeof(rx), 100);
    CHECK(n == 0, "无数据时 read() 超时返回 0");

    // 读满语义：只有 1 字节时，read(4) 应等到超时并返回 1
    ::write(master, reply, 1);
    n = urt.read(rx, 4, 150);
    CHECK(n == 1, "read() 未读满时返回已读字节数");

    // read_some：有 1 字节就立刻返回
    ::write(master, reply, 1);
    n = urt.read_some(rx, 32, 300);
    CHECK(n == 1, "read_some() 收到 1 字节即返回");

    // ---------------- 4. transact 一问一答 ----------------
    std::thread responder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::uint8_t buf[64];
        const int r = static_cast<int>(::read(master, buf, sizeof(buf)));   // 收请求
        const std::uint8_t ack[] = {0x90, 0x50, 0x00};
        if (r > 0)
        {
            ::write(master, ack, sizeof(ack));                             // 回应答
        }
    });
    const std::uint8_t req[] = {0x81, 0x01, 0x06, 0x01, 0xFF};             // VISCA 风格请求
    n = urt.transact(req, sizeof(req), rx, sizeof(rx), 500);
    responder.join();
    CHECK(n == 3 && rx[0] == 0x90 && rx[1] == 0x50, "transact() 收到正确应答");
    CHECK(urt.write(req, sizeof(req)) == static_cast<int>(sizeof(req)), "transact 后仍可正常写");

    // 清掉前面几次写留下的字节：用 poll 限时排空，避免阻塞在空串口上死等
    {
        std::uint8_t drain[128];
        while (true)
        {
            pollfd pfd{master, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0)
            {
                break;                  // 50ms 内没有新数据即认为已排空
            }
            if (::read(master, drain, sizeof(drain)) <= 0)
            {
                break;
            }
        }
    }

    // ---------------- 5. 并发写：验证写锁 ----------------
    const int kThreads = 4;
    const int kFrames = 25;
    const int kFrameLen = 64;
    std::atomic<int> received{0};
    std::atomic<int> corrupt{0};
    const int kExpect = kThreads * kFrames;

    std::thread reader([&]() {
        std::uint8_t buf[kFrameLen];
        while (received.load() < kExpect)
        {
            const int r = static_cast<int>(::read(master, buf, sizeof(buf)));
            if (r <= 0)
            {
                continue;
            }
            if (r == kFrameLen)
            {
                const std::uint8_t mark = buf[0];
                for (int i = 1; i < kFrameLen; ++i)
                {
                    if (buf[i] != mark)
                    {
                        ++corrupt;      // 出现别的线程的字节 → 串包
                        break;
                    }
                }
                ++received;
            }
            else
            {
                ++corrupt;              // 帧被截断
            }
        }
    });

    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t)
    {
        writers.emplace_back([&, t]() {
            std::vector<std::uint8_t> frame(kFrameLen, static_cast<std::uint8_t>(0x10 + t));
            for (int f = 0; f < kFrames; ++f)
            {
                urt.write(frame.data(), frame.size());
            }
        });
    }
    for (auto &w : writers)
    {
        w.join();
    }
    reader.join();

    printf("并发写: %d 线程 × %d 帧 = %d 帧, 收到 %d 帧, 串包/截断 %d 次\n",
           kThreads, kFrames, kExpect, received.load(), corrupt.load());
    CHECK(corrupt.load() == 0, "并发写下无串包、无截断（写锁有效）");
    CHECK(received.load() == kExpect, "并发写的帧全部到达");

    // ---------------- 6. 后台接收线程 ----------------
    std::atomic<int> cbBytes{0};
    CHECK(urt.start_receive([&](const std::uint8_t *data, std::size_t len) {
              (void)data;
              cbBytes += static_cast<int>(len);
          }) == 0, "start_receive() 启动接收线程");
    CHECK(urt.is_receiving(), "is_receiving() = true");

    const std::uint8_t burst[] = {1, 2, 3, 4, 5, 6, 7, 8};
    ::write(master, burst, sizeof(burst));
    for (int i = 0; i < 40 && cbBytes.load() < static_cast<int>(sizeof(burst)); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(cbBytes.load() == static_cast<int>(sizeof(burst)), "接收线程回调收到全部字节");

    // ---------------- 7. 接收线程运行中 close 不挂死 ----------------
    const auto t0 = std::chrono::steady_clock::now();
    urt.close();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
    printf("接收线程运行中 close() 耗时 %lld ms\n", static_cast<long long>(elapsedMs));
    CHECK(elapsedMs < 1000, "接收线程运行中 close() 及时返回（无死锁）");
    CHECK(!urt.is_open(), "close() 后 is_open() = false");
    CHECK(!urt.is_receiving(), "close() 同时停掉了接收线程");
    CHECK(urt.write(tx, sizeof(tx)) == -1, "close() 后 write() 返回 -1");
    CHECK(urt.read(rx, sizeof(rx), 50) == -1, "close() 后 read() 返回 -1");

    // ---------------- 8. 非法参数 ----------------
    CHECK(urt.open("", 115200) == -1, "空设备路径 → 失败");
    CHECK(urt.open("/dev/definitely-not-exist-urt", 115200) == -1, "不存在的设备 → 失败");
    CHECK(urt.open(slaveName, 12345) == -1, "不支持的波特率 → 失败");

    // ---------------- 8b. 链路异常回调 ----------------
    {
        int master2 = -1;
        std::string slave2;
        if (makePty(master2, slave2))
        {
            Urt u;
            std::atomic<int> cb_count{0};
            std::atomic<int> cb_errno{0};
            std::atomic<int> last_err{ -1 };
            u.set_error_callback([&](Urt::Error err, int sys_errno) {
                last_err = static_cast<int>(err);
                cb_errno = sys_errno;
                cb_count++;
            });

            // (1) 未打开就调用 read → NotOpen，应触发一次回调
            std::uint8_t buf[16] = {0};
            CHECK(u.read(buf, sizeof(buf), 50) == -1, "未打开时 read 返回 -1");
            CHECK(cb_count.load() == 1, "链路异常回调被触发(未打开)");
            CHECK(last_err.load() == static_cast<int>(Urt::Error::NotOpen), "异常类型为 NotOpen");
            CHECK(u.has_error(), "has_error() = true");

            // (2) 沿触发：连续多次失败不应重复回调
            u.read(buf, sizeof(buf), 50);
            u.read(buf, sizeof(buf), 50);
            CHECK(cb_count.load() == 1, "异常状态持续时回调不重复触发(沿触发)");

            // (3) 打开后读写成功 → 恢复，重新武装回调
            CHECK(u.open(slave2, 115200, 8, 'N', 1, false, false) == 0, "重新打开串口");
            CHECK(u.write(buf, 4) == 4, "写入成功");
            ::read(master2, buf, sizeof(buf));          // 取走，保持缓冲干净
            CHECK(!u.has_error(), "读到/写到数据后 has_error() 复位(链路恢复)");

            u.read(buf, sizeof(buf), 50);
            CHECK(cb_count.load() == 1, "恢复后正常读超时不触发回调(超时不算异常)");

            // (4) 拔线：关闭对端 master → 从端读到 EOF/EIO，应触发链路断开回调
            ::close(master2);
            CHECK(u.start_receive([](const std::uint8_t *, std::size_t) {}) == 0,
                  "启动接收线程(用于捕捉拔线)");
            for (int i = 0; i < 60 && cb_count.load() < 2; i++)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            CHECK(cb_count.load() == 2, "对端断开后异常回调被触发(接收线程内发现)");
            const int err = last_err.load();
            CHECK(err == static_cast<int>(Urt::Error::PeerClosed) ||
                  err == static_cast<int>(Urt::Error::ReadFailed),
                  "异常类型为 PeerClosed 或 ReadFailed");
            CHECK(u.has_error(), "拔线后 has_error() = true");

            // (5) 拔线后回调不刷屏：再等一会不应重复触发
            const int before = cb_count.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(cb_count.load() == before, "链路异常状态下回调不刷屏");

            u.stop_receive();
            u.close();
        }
    }

    // ---------------- 9. 独占保护（TIOCEXCL）----------------
    // 单独用一对 pty：内核里 TIOCEXCL 标志只要 tty 还有引用就不会清除，
    // 所以独占测试不能和上面的反复 close/open 混用同一个 pty
    {
        int master2 = -1;
        std::string slave2;
        if (makePty(master2, slave2))
        {
            Urt a;
            CHECK(a.open(slave2, 115200, 8, 'N', 1, false, true) == 0, "实例 A 独占打开成功");
            Urt b;
            CHECK(b.open(slave2, 115200, 8, 'N', 1, false, true) == -1,
                  "实例 B 打开同一串口被拒(TIOCEXCL 独占保护)");
            a.close();
            ::close(master2);
        }
    }

    // ---------------- 10. 关闭后可重新打开 ----------------
    CHECK(urt.open(slaveName, 9600, 8, 'N', 1, false, false) == 0, "close() 后可重新 open");
    CHECK(urt.device() == slaveName, "重开后 device() 正确");
    urt.close();
    printf("\nRESULT: %s\n", g_fail == 0 ? "ALL PASS" : "HAS FAILURES");
    return g_fail == 0 ? 0 : 1;
}
