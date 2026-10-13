#pragma once

#include"common/common.h"

#include<atomic>
#include<cstring>
#include<functional>

#include<fcntl.h>
#include<poll.h>
#include<sys/ioctl.h>
#include<termios.h>
#include<unistd.h>

#include"log/log.h"

// ---------------------------------------------------------------------------
// 线程安全的串口读写（POSIX termios，header-only）
//
// 设计要点：
//   1) 读、写各一把锁。串口是全双工的，"一边收一边发"是常态，
//      共用一把锁会让收发互相阻塞；分开后收发可以真正并发。
//   2) open/close 与在途 I/O 不打架。close() 先停接收线程，再按
//      write → read → state 的固定顺序取锁，保证没有 I/O 在飞行中才关 fd。
//      所有 I/O 都是"先在短锁里取 fd，再放锁做实际读写"，因此不会和
//      close 形成环状等待（不会死锁）。
//   3) 超时统一用 poll 控制（不依赖 VMIN/VTIME），EINTR 自动重试；
//      write 会循环直到整包发完（串口驱动可能只收一部分）。
//   4) transact() 给"一问一答"型协议（云台/镜头指令）用：清输入缓冲 → 写 →
//      tcdrain 等发完 → 读应答，全程持双锁，不会被别的线程插进来。
//
// 用法：
//   Urt urt;
//   if (urt.open("/dev/ttyS0", 9600) != 0) { ... }        // 8N1
//
//   uint8_t cmd[8] = {...};
//   uint8_t ack[16] = {0};
//   int n = urt.transact(cmd, sizeof(cmd), ack, sizeof(ack), 200);
//
//   // 或者后台收、随时发：
//   urt.start_receive([](const uint8_t *data, size_t len) { ... });
//   urt.write(cmd, sizeof(cmd));
//
// 注意：open()/close() 属于生命周期操作，请由同一个控制线程负责调用，
//       不要与其它线程的 open/close 并发（读写则可以任意线程并发）。
// ---------------------------------------------------------------------------
class Urt
{
public:
    // 接收回调：在内部接收线程里执行；data 的内存仅在该回调期间有效，
    // 需要留存请自行拷贝
    using receive_callback_t = std::function<void(const std::uint8_t *data, std::size_t len)>;

    // 链路异常类型
    enum class Error
    {
        NotOpen = 0,    // 串口未打开（或已被关闭）
        ReadFailed,     // 读取失败（errno 级，如设备拔出 EIO/ENXIO）
        WriteFailed,    // 写入失败
        PeerClosed,     // 读到 EOF：对端/设备已断开（伪终端从端关闭、USB 串口被摘掉）
        WriteTimeout    // 等待可写超时
    };

    static const char *error_str(Error err)
    {
        switch (err)
        {
        case Error::NotOpen:      return "串口未打开";
        case Error::ReadFailed:   return "读取失败";
        case Error::WriteFailed:  return "写入失败";
        case Error::PeerClosed:   return "链路断开(EOF)";
        case Error::WriteTimeout: return "写等待超时";
        }
        return "未知错误";
    }

    // 链路异常回调：err 为异常类型，sys_errno 为对应的 errno（没有则为 0）
    //
    // 语义与注意事项：
    //   * **沿触发**：只在"正常 → 异常"时回调一次，不会每 100ms 重复刷；
    //     链路恢复（读到数据/写入成功，或显式 clear_error()）后才会再次触发。
    //   * 回调在**发现异常的那个线程**执行（通常是接收线程），且已释放内部锁，
    //     因此回调里可以安全调用 read/write；但不要在里面 close()/stop_receive()
    //     或 join 接收线程（那是在等自己），需要重连请投递到别的线程做。
    //   * 请在 open() 之前或之后任意时刻设置，设置本身线程安全。
    using error_callback_t = std::function<void(Error err, int sys_errno)>;

    Urt() = default;
    ~Urt() { close(); }

    Urt(const Urt &) = delete;
    Urt &operator=(const Urt &) = delete;
    Urt(Urt &&) = delete;
    Urt &operator=(Urt &&) = delete;

    // -----------------------------------------------------------------------
    // 打开串口：打开设备节点并配置为原始模式
    //   dev       : /dev/ttyS0、/dev/ttyUSB0 等
    //   baudrate  : 1200~921600，见 baud_to_speed()
    //   databits  : 5/6/7/8
    //   parity    : 'N' 无校验 / 'E' 偶校验 / 'O' 奇校验
    //   stopbits  : 1/2
    //   flow_ctrl : 硬件流控(RTS/CTS)
    //   exclusive : 打开后加 TIOCEXCL 独占该串口，防止其它进程/实例同时打开
    //               （被独占时后来者 open 会得到 EBUSY）。同一进程内 close 后
    //               只要没有别的 fd 持有该 tty，就可以再次 open
    // 成功返回 0，失败返回 -1（失败时内部已释放资源，可直接重试）
    // -----------------------------------------------------------------------
    int open(const std::string &dev,
             int  baudrate = 115200,
             int  databits = 8,
             char parity = 'N',
             int  stopbits = 1,
             bool flow_ctrl = false,
             bool exclusive = true)
    {
        close();    // 幂等：重复 open 视为重新配置

        if (dev.empty())
        {
            UAV_LOG_WARN << "Urt::open 未指定设备节点\n";
            return -1;
        }

        const speed_t speed = baud_to_speed(baudrate);
        if (speed == 0)
        {
            UAV_LOG_WARN << "Urt::open 不支持的波特率: " << baudrate << "\n";
            return -1;
        }

        // O_NONBLOCK：避免某些串口(如未接线的 ttyS)在 open 阶段长时间阻塞；
        // 之后所有等待都由 poll 控制超时，因此保持非阻塞语义。
        const int fd = ::open(dev.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0)
        {
            UAV_LOG_ERROR << "Urt::open 打开 " << dev << " 失败: " << strerror(errno) << "\n";
            return -1;
        }

        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            dev_path = dev;
            m_fd = fd;
            open_ok = true;
            set_ok = false;
        }

        if (configure(fd, speed, databits, parity, stopbits, flow_ctrl, exclusive) != 0)
        {
            close();
            return -1;
        }

        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            set_ok = true;
        }

        UAV_LOG_INFO << "串口已打开: " << dev << " @ " << baudrate
                     << " " << databits << parity << stopbits
                     << (flow_ctrl ? " 硬件流控" : "")
                     << (exclusive ? " 独占" : "") << "\n";
        return 0;
    }

    // 关闭串口（先停接收线程，再等在途读写结束）。可重复调用
    void close()
    {
        stop_receive();     // 接收线程持有读锁，必须先停它

        std::lock_guard<std::mutex> wlock(m_write_mutex);
        std::lock_guard<std::mutex> rlock(m_read_mutex);
        std::lock_guard<std::mutex> slock(m_state_mutex);

        if (m_fd >= 0)
        {
            ::close(m_fd);
            m_fd = -1;
        }
        open_ok = false;
        set_ok = false;
    }

    bool is_open() const
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return open_ok && m_fd >= 0;
    }

    // 是否已成功配置过波特率等参数
    bool is_configured() const
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return set_ok;
    }

    const std::string device() const
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return dev_path;
    }

    // -----------------------------------------------------------------------
    // 写：整包写完才返回。返回写入字节数，失败返回 -1。线程安全
    // -----------------------------------------------------------------------
    int write(const void *data, std::size_t len)
    {
        if (data == nullptr || len == 0)
        {
            return -1;
        }
        int n = 0;
        {
            std::lock_guard<std::mutex> wlock(m_write_mutex);
            n = write_locked(data, len);
        }
        dispatch_error();
        return n;
    }

    int write(const std::vector<std::uint8_t> &data)
    {
        return write(data.data(), data.size());
    }

    int write_string(const std::string &text)
    {
        return write(text.data(), text.size());
    }

    // -----------------------------------------------------------------------
    // 读：读满 len 字节或超时返回
    //   返回 >0 实际字节数；0 表示超时；-1 出错。线程安全
    // timeout_ms < 0 时使用内部默认值(default_read_timeout_ms)
    // -----------------------------------------------------------------------
    int read(void *buf, std::size_t len, int timeout_ms = -1)
    {
        if (buf == nullptr || len == 0)
        {
            return -1;
        }
        int n = 0;
        {
            std::lock_guard<std::mutex> rlock(m_read_mutex);
            n = read_locked(buf, len, resolve_timeout(timeout_ms), false);
        }
        dispatch_error();      // 回调在锁外派发，回调里可以安全再调用 read/write
        return n;
    }

    // 收到任意字节即返回（等一条短应答时延迟最低）
    int read_some(void *buf, std::size_t len, int timeout_ms = -1)
    {
        if (buf == nullptr || len == 0)
        {
            return -1;
        }
        int n = 0;
        {
            std::lock_guard<std::mutex> rlock(m_read_mutex);
            n = read_locked(buf, len, resolve_timeout(timeout_ms), true);
        }
        dispatch_error();
        return n;
    }

    // -----------------------------------------------------------------------
    // 一问一答（云台/镜头指令）：清输入缓冲 → 写 → 等发完 → 读应答
    // 全程持读写两把锁，不会被其它线程的收发穿插
    // 返回读到的字节数（0 表示超时无应答），失败返回 -1
    // -----------------------------------------------------------------------
    int transact(const void *tx, std::size_t tx_len,
                 void *rx, std::size_t rx_len,
                 int timeout_ms = 200)
    {
        if (tx == nullptr || tx_len == 0 || rx == nullptr || rx_len == 0)
        {
            return -1;
        }

        int result = 0;
        {
            std::lock_guard<std::mutex> wlock(m_write_mutex);
            std::lock_guard<std::mutex> rlock(m_read_mutex);

            const int fd = current_fd();
            if (fd < 0)
            {
                UAV_LOG_WARN << "Urt::transact 串口未打开\n";
                mark_error(Error::NotOpen, 0);
                result = -1;
            }
            else
            {
                ::tcflush(fd, TCIFLUSH);    // 丢掉上一次可能残留的字节，避免误判应答

                if (write_locked(tx, tx_len) < 0)
                {
                    result = -1;
                }
                else
                {
                    ::tcdrain(fd);          // 等数据真正送出，再开始等应答
                    result = read_locked(rx, rx_len, resolve_timeout(timeout_ms), true);
                }
            }
        }
        dispatch_error();                   // 锁外派发，回调里可安全再调用 read/write
        return result;
    }

    // 丢弃收发缓冲中的数据
    void flush()
    {
        std::lock_guard<std::mutex> wlock(m_write_mutex);
        std::lock_guard<std::mutex> rlock(m_read_mutex);
        const int fd = current_fd();
        if (fd >= 0)
        {
            ::tcflush(fd, TCIOFLUSH);
        }
    }

    // -----------------------------------------------------------------------
    // 后台接收线程：不断读取并回调。重复调用会先停掉旧的
    // buf_size 为单次读取的最大字节数
    // -----------------------------------------------------------------------
    int start_receive(receive_callback_t callback, std::size_t buf_size = 512)
    {
        if (!callback)
        {
            UAV_LOG_WARN << "Urt::start_receive 回调为空\n";
            return -1;
        }
        if (!is_open())
        {
            UAV_LOG_WARN << "Urt::start_receive 串口未打开\n";
            return -1;
        }

        stop_receive();

        m_rx_callback = std::move(callback);
        m_rx_running = true;
        m_rx_thread = std::thread([this, buf_size]() {
            std::vector<std::uint8_t> buf(buf_size > 0 ? buf_size : 512);
            while (m_rx_running.load())
            {
                // 100ms 读超时：既能及时响应停止，也不会忙等
                const int n = read_some(buf.data(), buf.size(), 100);
                if (n > 0)
                {
                    if (m_rx_callback)
                    {
                        m_rx_callback(buf.data(), static_cast<std::size_t>(n));
                    }
                }
                else if (n < 0)
                {
                    // 出错或串口被关：稍等再试，避免空转刷日志
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
        return 0;
    }

    // 停止接收线程（阻塞到线程退出）
    void stop_receive()
    {
        m_rx_running = false;
        if (m_rx_thread.joinable())
        {
            m_rx_thread.join();
        }
        m_rx_callback = nullptr;
    }

    bool is_receiving() const { return m_rx_running.load(); }

    // 设置 read()/read_some()/transact() 不显式传超时时使用的默认值（毫秒）
    void set_read_timeout_ms(int timeout_ms) { m_default_read_timeout_ms = timeout_ms; }

    // ---- 链路异常回调与状态 ----
    void set_error_callback(error_callback_t callback)
    {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        m_error_callback = std::move(callback);
    }

    // 当前是否处于"链路异常"状态（异常后一直为 true，直到链路恢复或 clear_error()）
    bool has_error() const { return m_error_active.load(std::memory_order_acquire); }
    Error last_error() const
    {
        return static_cast<Error>(m_last_error.load(std::memory_order_relaxed));
    }
    int last_errno() const { return m_last_errno.load(std::memory_order_relaxed); }

    // 手动清除异常状态（例如上层重连成功后调用），会重新武装回调
    void clear_error()
    {
        m_error_active.store(false, std::memory_order_release);
        m_error_pending.store(false, std::memory_order_release);
    }

private:
    // 取当前 fd（短锁）。未打开返回 -1
    int current_fd() const
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return m_fd;
    }

    int resolve_timeout(int timeout_ms) const
    {
        return (timeout_ms < 0) ? m_default_read_timeout_ms : timeout_ms;
    }

    // 波特率 → termios 常量；不支持返回 0
    static speed_t baud_to_speed(int baud)
    {
        switch (baud)
        {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default:     return 0;
        }
    }

    // 配置 termios 为原始模式
    int configure(int fd, speed_t speed, int databits, char parity,
                  int stopbits, bool flow_ctrl, bool exclusive)
    {
        termios tty{};
        if (::tcgetattr(fd, &tty) != 0)
        {
            UAV_LOG_ERROR << "Urt::open tcgetattr 失败: " << strerror(errno) << "\n";
            return -1;
        }

        ::cfmakeraw(&tty);                  // 原始模式：不做任何字符加工
        ::cfsetispeed(&tty, speed);
        ::cfsetospeed(&tty, speed);

        tty.c_cflag |= (CLOCAL | CREAD);    // 忽略调制解调器控制线，允许接收
        tty.c_cflag &= ~CSIZE;
        switch (databits)
        {
        case 5:  tty.c_cflag |= CS5; break;
        case 6:  tty.c_cflag |= CS6; break;
        case 7:  tty.c_cflag |= CS7; break;
        case 8:
        default: tty.c_cflag |= CS8; break;
        }

        switch (parity)
        {
        case 'E':
        case 'e':
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            tty.c_iflag |= INPCK;
            break;
        case 'O':
        case 'o':
            tty.c_cflag |= PARENB;
            tty.c_cflag |= PARODD;
            tty.c_iflag |= INPCK;
            break;
        case 'N':
        case 'n':
        default:
            tty.c_cflag &= ~PARENB;
            tty.c_iflag &= ~INPCK;
            break;
        }

        if (stopbits == 2)
        {
            tty.c_cflag |= CSTOPB;
        }
        else
        {
            tty.c_cflag &= ~CSTOPB;
        }

        if (flow_ctrl)
        {
            tty.c_cflag |= CRTSCTS;
        }
        else
        {
            tty.c_cflag &= ~CRTSCTS;
        }

        // 不用 VMIN/VTIME 的阻塞语义，超时统一交给 poll
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;

        if (::tcsetattr(fd, TCSANOW, &tty) != 0)
        {
            UAV_LOG_ERROR << "Urt::open tcsetattr 失败: " << strerror(errno) << "\n";
            return -1;
        }

        ::tcflush(fd, TCIOFLUSH);

        // 独占占用：防止其它进程/实例同时打开同一串口互相抢数据。
        // 部分驱动/伪终端不支持 TIOCEXCL，失败只记日志、不影响使用。
        if (exclusive && ::ioctl(fd, TIOCEXCL) != 0)
        {
            UAV_LOG_WARN << "Urt::open TIOCEXCL 失败(不影响使用): " << strerror(errno) << "\n";
        }

        return 0;
    }

    // 调用方须持有 m_write_mutex
    int write_locked(const void *data, std::size_t len)
    {
        const int fd = current_fd();
        if (fd < 0)
        {
            UAV_LOG_WARN << "Urt::write 串口未打开\n";
            mark_error(Error::NotOpen, 0);
            return -1;
        }

        const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
        std::size_t sent = 0;
        while (sent < len)
        {
            const ssize_t n = ::write(fd, p + sent, len - sent);
            if (n > 0)
            {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                pollfd pfd{fd, POLLOUT, 0};             // 发送缓冲满：等可写
                if (::poll(&pfd, 1, m_write_timeout_ms) > 0)
                {
                    continue;
                }
                UAV_LOG_WARN << "Urt::write 等待可写超时(" << m_write_timeout_ms << "ms)\n";
                mark_error(Error::WriteTimeout, 0);
                return (sent > 0) ? static_cast<int>(sent) : -1;
            }
            UAV_LOG_ERROR << "Urt::write 失败: " << strerror(errno) << "\n";
            mark_error(Error::WriteFailed, errno);
            return (sent > 0) ? static_cast<int>(sent) : -1;
        }

        mark_recovered();   // 写成功 → 链路正常
        return static_cast<int>(sent);
    }

    // 调用方须持有 m_read_mutex
    // return_on_first_byte=true 时读到任意字节即返回，否则读满 len 或超时
    int read_locked(void *buf, std::size_t len, int timeout_ms, bool return_on_first_byte)
    {
        const int fd = current_fd();
        if (fd < 0)
        {
            UAV_LOG_WARN << "Urt::read 串口未打开\n";
            mark_error(Error::NotOpen, 0);
            return -1;
        }

        std::uint8_t *p = static_cast<std::uint8_t *>(buf);
        std::size_t got = 0;

        while (got < len)
        {
            pollfd pfd{fd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                UAV_LOG_ERROR << "Urt::read poll 失败: " << strerror(errno) << "\n";
                mark_error(Error::ReadFailed, errno);
                return (got > 0) ? static_cast<int>(got) : -1;
            }
            if (pr == 0)
            {
                break;      // 超时：返回已读到的字节数（可能为 0）
            }

            const ssize_t n = ::read(fd, p + got, len - got);
            if (n > 0)
            {
                got += static_cast<std::size_t>(n);
                if (return_on_first_byte)
                {
                    break;
                }
                continue;
            }
            if (n < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    continue;
                }
                UAV_LOG_ERROR << "Urt::read 失败: " << strerror(errno) << "\n";
                mark_error(Error::ReadFailed, errno);
                return (got > 0) ? static_cast<int>(got) : -1;
            }

            // n == 0：读到 EOF —— 设备被摘掉/对端关闭，属于链路断开而不是"没数据"。
            // 这里返回 -1（而不是 0），既能与超时区分，也避免接收线程在 EOF 上空转
            UAV_LOG_ERROR << "Urt::read 链路断开(EOF): 设备可能已被拔出\n";
            mark_error(Error::PeerClosed, 0);
            return (got > 0) ? static_cast<int>(got) : -1;
        }

        if (got > 0)
        {
            mark_recovered();   // 读到数据 → 链路正常，重新武装异常回调
        }
        return static_cast<int>(got);
    }

    // ---- 状态（m_state_mutex 保护）----
    mutable std::mutex m_state_mutex;
    std::string dev_path;
    int  m_fd{-1};
    bool open_ok{false};
    bool set_ok{false};

    // ---- 收发串行化 ----
    mutable std::mutex m_write_mutex;    // 串行化写
    mutable std::mutex m_read_mutex;     // 串行化读

    // ---- 链路异常状态 ----
    // 在持锁的 I/O 路径里只置标志（mark_error），回调统一在锁外派发（dispatch_error），
    // 避免"回调里再调用 read/write 造成自锁"
    void mark_error(Error err, int sys_errno)
    {
        m_last_error.store(static_cast<int>(err), std::memory_order_relaxed);
        m_last_errno.store(sys_errno, std::memory_order_relaxed);
        if (!m_error_active.exchange(true, std::memory_order_acq_rel))
        {
            m_error_pending.store(true, std::memory_order_release);   // 只在"沿"上排队一次
        }
    }

    // 读到数据 / 写入成功 → 认为链路恢复正常，重新武装异常回调
    void mark_recovered()
    {
        m_error_active.store(false, std::memory_order_release);
    }

    // 在锁外派发异常回调（每次异常只回调一次）
    void dispatch_error()
    {
        if (!m_error_pending.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }
        error_callback_t callback;
        {
            std::lock_guard<std::mutex> lock(m_cb_mutex);
            callback = m_error_callback;
        }
        if (callback)
        {
            callback(static_cast<Error>(m_last_error.load(std::memory_order_relaxed)),
                     m_last_errno.load(std::memory_order_relaxed));
        }
    }

    mutable std::mutex m_cb_mutex;
    error_callback_t   m_error_callback;
    std::atomic<bool>  m_error_active{false};
    std::atomic<bool>  m_error_pending{false};
    std::atomic<int>   m_last_error{static_cast<int>(Error::NotOpen)};
    std::atomic<int>   m_last_errno{0};

    int m_default_read_timeout_ms{200};  // read()/transact() 不传超时时的默认值
    int m_write_timeout_ms{500};         // 写等可写的最长时间

    // ---- 接收线程 ----
    std::atomic<bool>  m_rx_running{false};
    std::thread        m_rx_thread;
    receive_callback_t m_rx_callback;    // 只在 start/stop 与接收线程之间传递，无并发
};
