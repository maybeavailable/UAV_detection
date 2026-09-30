#pragma once

#include"common/common.h"
#include"common/single.h"

#include<cstddef>
#include<cstdint>
#include<sstream>

class RotateFileWriter;   // 前置声明：使用方无需知道轮转实现，降低头文件耦合

// 日志级别：数值越大越严重，低于当前阈值的记录会被直接丢弃
enum class LogLevel : int
{
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5
};

// 定长（5 字符）级别名，便于日志列对齐
const char* logLevelName(LogLevel level);
bool        parseLogLevel(const std::string& text, LogLevel& out);

struct Log
{
    std::string logMessage;
    std::string logLevel;
    std::string logTime;
    std::string loglocation;
    std::string logThread;
};

// 日志配置。必须在第一次 getInstance() 之前调用 LogManager::configure() 才会生效。
struct LogConfig
{
    std::string filePath     = "logs/uav_detection.log"; // 相对路径基于进程当前工作目录
    std::size_t maxFileSize  = 10u * 1024u * 1024u;      // 单个文件大小上限（字节）
    std::size_t maxFiles     = 10;                       // 保留的历史文件数量，0 = 不清理
    std::size_t maxQueueSize = 4096;                     // 异步队列上限（条），溢出丢弃最旧记录
    LogLevel    level        = LogLevel::INFO;           // 过滤阈值
};

// 异步日志：调用方 push 入队后立即返回，由后台线程落盘。
// 线程安全；configure() 例外，必须在首次 getInstance() 之前调用。
class LogManager : public Singleton<LogManager>
{
    friend class Singleton<LogManager>;

public:
    // 建议在 main 开头调用；实例已创建时返回 false 且配置不生效
    static bool configure(const LogConfig& config);

    // 推荐通过下面的 UAV_LOG_* 宏使用
    void log(LogLevel level, const char* file, int line, std::string message);

    // printf 风格：带 __attribute__((format(printf,...)))，格式串写错会在编译期报出。
    // 消息超出栈上缓冲时自动扩容，不会截断。
    // 用 static：非静态成员函数的 format 属性会把隐式 this 算作第 1 个参数，
    // 索引会平白偏移一位（GCC/Clang 一致），静态成员没有这个问题。
    static void logPrintf(LogLevel level, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;

    // 低层接口：logTime / logThread / loglocation 为空时自动补齐；
    // 该接口不做等级过滤（显式构造的记录一律接收）。
    void pushLog(Log log);

    bool     shouldLog(LogLevel level) const;
    void     setLevel(LogLevel level);
    LogLevel level() const;

    std::uint64_t writtenCount() const;   // 已落盘条数
    std::uint64_t droppedCount() const;   // 被丢弃条数（队列溢出 + 停止后上报）
    std::size_t   queueSize() const;

    // 停止后台线程并排空队列（幂等）；此后的 pushLog 会被计数丢弃
    void  stop();
    bool  isStopped() const;

private:
    LogManager();
    ~LogManager();

    void worker();
    void writeRaw(const std::string& line) noexcept;   // 任何异常都不得逃出日志线程

    std::queue<Log>              logQueue_;
    std::thread                  logWorkerThread_;
    std::condition_variable      logCondition_;
    mutable std::mutex           logMutex_;
    std::size_t                  maxQueueSize_;
    std::atomic<LogLevel>        level_;
    std::atomic<bool>            stopFlag_;
    std::atomic<std::uint64_t>   writtenCount_;
    std::atomic<std::uint64_t>   droppedFull_;      // 队列满丢弃
    std::atomic<std::uint64_t>   droppedStopped_;   // 停止后丢弃
    std::atomic<std::uint64_t>   reportedDropped_;  // 已写入日志文件的丢弃数

    std::unique_ptr<RotateFileWriter> rotateFileWriter_;
};

// 用 UAV_LOG_ 前缀而不是 LOG_：<syslog.h> 已经占用了 LOG_INFO / LOG_DEBUG 等宏名。
//
// 三种用法：
//   1) cout 风格（日常推荐，仿 std::cout 与 glog 的 LOG(INFO) << ...）：
//        UAV_LOG_INFO << "x=" << x << " y=" << y;
//      行尾分号处自动换行落盘，不需要也不要用 std::endl；
//      std::hex 等操纵符可用，且每条日志一个全新流，格式化状态不会像 cout 那样残留。
//      关键好处：消息完全走 operator<<，不经过宏参数分割，
//      花括号里的逗号不再是问题：
//        UAV_LOG_INFO << std::pair<int,int>{1,2};
//   2) printf 风格（格式串是字面量时享受编译期检查）：
//        UAV_LOG_INFO_F("x=%d y=%s", x, y.c_str());
//   3) 条件日志（条件不满足时连实参表达式都不求值，适合热路径）：
//        UAV_LOG_IF(LogLevel::DEBUG, cond) << "x=" << x;
//
// 与"宏传参流式"的区别（也是取舍）：
// cout 风格下，operator<< 的实参总是在进入函数前求值，所以级别被过滤时
// 实参表达式照样会执行（和 std::cout 一个德行）——热路径上的 TRACE/DEBUG
// 请用 UAV_LOG_IF 包一层。而 printf 风格没有宏样板，代价是 %s 要自己 .c_str()。

// 临时流对象：一条日志一个对象，<< 链结束后（行尾分号）由析构函数落盘
class LogStream
{
public:
    LogStream(LogLevel level, const char* file, int line)
        : level_(level),
          file_(file != nullptr ? file : "unknown"),
          line_(line),
          active_(LogManager::getInstance().shouldLog(level))
    {
    }

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    ~LogStream()
    {
        if (active_ && !oss_.str().empty())
            LogManager::getInstance().log(level_, file_, line_, oss_.str());
    }

    template<typename T>
    LogStream& operator<<(const T& value)
    {
        if (active_)
            oss_ << value;
        return *this;
    }

private:
    LogLevel           level_;
    const char*        file_;
    int                line_;
    bool               active_;
    std::ostringstream oss_;
};

// UAV_LOG_IF 的辅助类：用 operator& 吃掉 LogStream 临时对象（glog 同款技巧）。
// 宏里没有 if，因此不会引入悬垂 else 问题。
class LogStreamVoidify
{
public:
    void operator&(const LogStream&) const {}
};

#define UAV_LOG_INFO  ::LogStream(LogLevel::INFO,  __FILE__, __LINE__)
#define UAV_LOG_WARN  ::LogStream(LogLevel::WARN,  __FILE__, __LINE__)
#define UAV_LOG_ERROR ::LogStream(LogLevel::ERROR, __FILE__, __LINE__)
#define UAV_LOG_FATAL ::LogStream(LogLevel::FATAL, __FILE__, __LINE__)
#define UAV_LOG_DEBUG ::LogStream(LogLevel::DEBUG, __FILE__, __LINE__)
#define UAV_LOG_TRACE ::LogStream(LogLevel::TRACE, __FILE__, __LINE__)

// 条件日志：cond 为假时 LogStream 根本不构造，实参零开销
#define UAV_LOG_IF(LEVEL, cond) \
    !(cond) ? (void)0 : ::LogStreamVoidify() & ::LogStream((LEVEL), __FILE__, __LINE__)

// printf 风格（格式串必须是字面量才能享受编译期检查）
#define UAV_LOG_PRINTF_AT(LEVEL, ...)                                           \
    do {                                                                        \
        ::LogManager::logPrintf((LEVEL), __FILE__, __LINE__, __VA_ARGS__);      \
    } while (0)

#define UAV_LOG_TRACE_F(...) UAV_LOG_PRINTF_AT(LogLevel::TRACE, __VA_ARGS__)
#define UAV_LOG_DEBUG_F(...) UAV_LOG_PRINTF_AT(LogLevel::DEBUG, __VA_ARGS__)
#define UAV_LOG_INFO_F(...)  UAV_LOG_PRINTF_AT(LogLevel::INFO,  __VA_ARGS__)
#define UAV_LOG_WARN_F(...)  UAV_LOG_PRINTF_AT(LogLevel::WARN,  __VA_ARGS__)
#define UAV_LOG_ERROR_F(...) UAV_LOG_PRINTF_AT(LogLevel::ERROR, __VA_ARGS__)
#define UAV_LOG_FATAL_F(...) UAV_LOG_PRINTF_AT(LogLevel::FATAL, __VA_ARGS__)
