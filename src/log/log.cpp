#include"log/log.h"
#include"log/RotateFileWriter.h"

#include<cctype>
#include<chrono>
#include<cstdarg>
#include<cstdio>
#include<ctime>
#include<sstream>

#if defined(__linux__)
#include<sys/syscall.h>
#include<unistd.h>
#endif

namespace
{

// configure() 写入、LogManager 构造函数读取的待生效配置。
// 约定：只在首次 getInstance() 之前由启动线程设置。
LogConfig& pendingConfig()
{
    static LogConfig config;
    return config;
}

std::atomic<bool>& instanceCreatedFlag()
{
    static std::atomic<bool> created{false};
    return created;
}

std::string currentTimeString()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis  = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch()).count() % 1000;

    struct tm tmv;
    localtime_r(&seconds, &tmv);          // linux；windows 用 localtime_s

    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);

    char out[48] = {0};
    std::snprintf(out, sizeof(out), "%s.%03d", buf, static_cast<int>(millis));
    return std::string(out);
}

std::string currentThreadId()
{
#if defined(__linux__)
    // 用内核 tid，便于和 gdb / top -H / ps -L 对齐
    return std::to_string(static_cast<long>(::syscall(SYS_gettid)));
#else
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
#endif
}

const char* shortFileName(const char* path)
{
    const char* name = path;
    for (const char* p = path; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
            name = p + 1;
    }
    return name;
}

std::string buildLine(const std::string& time,
                      const char* level,
                      const std::string& thread,
                      const std::string& location,
                      const std::string& message)
{
    std::string line;
    line.reserve(time.size() + thread.size() + location.size() + message.size() + 16);
    line += '[';
    line += time;
    line += "] [";
    line += level;
    line += "] [tid:";
    line += thread;
    line += "] [";
    line += location;
    line += "] ";
    line += message;
    return line;
}

std::string droppedNotice(std::uint64_t total, std::uint64_t full, std::uint64_t stopped)
{
    std::ostringstream oss;
    oss << "dropped " << total << " log record(s) (queue full: " << full
        << ", after stop: " << stopped << ")";
    return oss.str();
}

}  // namespace

const char* logLevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::TRACE: return "TRACE";
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO ";
    case LogLevel::WARN:  return "WARN ";
    case LogLevel::ERROR: return "ERROR";
    case LogLevel::FATAL: return "FATAL";
    }
    return "INFO ";
}

bool parseLogLevel(const std::string& text, LogLevel& out)
{
    std::string upper;
    upper.reserve(text.size());
    for (const char c : text)
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (upper == "TRACE") { out = LogLevel::TRACE; return true; }
    if (upper == "DEBUG") { out = LogLevel::DEBUG; return true; }
    if (upper == "INFO")  { out = LogLevel::INFO;  return true; }
    if (upper == "WARN" || upper == "WARNING") { out = LogLevel::WARN; return true; }
    if (upper == "ERROR") { out = LogLevel::ERROR; return true; }
    if (upper == "FATAL") { out = LogLevel::FATAL; return true; }
    return false;
}

bool LogManager::configure(const LogConfig& config)
{
    if (instanceCreatedFlag().load())
        return false;
    pendingConfig() = config;
    return true;
}

LogManager::LogManager()
    : maxQueueSize_(4096),
      level_(LogLevel::INFO),
      stopFlag_(false),
      writtenCount_(0),
      droppedFull_(0),
      droppedStopped_(0),
      reportedDropped_(0)
{
    const LogConfig config = pendingConfig();
    maxQueueSize_ = config.maxQueueSize > 0 ? config.maxQueueSize : 1;
    level_.store(config.level, std::memory_order_relaxed);

    rotateFileWriter_ = std::make_unique<RotateFileWriter>(config.filePath,
                                                           config.maxFileSize,
                                                           config.maxFiles);

    logWorkerThread_ = std::thread(&LogManager::worker, this);

    // 只有构造全部成功才置位，构造抛异常时允许后续重新调用 getInstance()
    instanceCreatedFlag().store(true);
}

LogManager::~LogManager()
{
    stop();
    if (logWorkerThread_.joinable())
        logWorkerThread_.join();

    // worker 已退出，此处丢弃计数不会再变化，补一条最终统计（正常无丢弃时不打扰）
    const std::uint64_t total = droppedFull_.load() + droppedStopped_.load();
    if (total > 0 && total != reportedDropped_.load())
    {
        reportedDropped_.store(total);
        writeRaw(buildLine(currentTimeString(), logLevelName(LogLevel::WARN), currentThreadId(),
                           "log.cpp:shutdown",
                           droppedNotice(total, droppedFull_.load(), droppedStopped_.load())));
    }
}

void LogManager::worker()
{
    for (;;)
    {
        std::unique_lock<std::mutex> lock(logMutex_);
        logCondition_.wait(lock, [this] {
            return !logQueue_.empty() || stopFlag_.load();
        });

        while (!logQueue_.empty())
        {
            Log item = std::move(logQueue_.front());
            logQueue_.pop();

            lock.unlock();
            writeRaw(buildLine(item.logTime, item.logLevel.c_str(), item.logThread,
                               item.loglocation, item.logMessage));
            writtenCount_.fetch_add(1, std::memory_order_relaxed);
            lock.lock();
        }

        // 队列已排空：丢弃计数有变化就在日志里留痕，避免静默丢失
        const std::uint64_t total = droppedFull_.load() + droppedStopped_.load();
        const bool reportDrops = (total != reportedDropped_.load());
        if (reportDrops)
            reportedDropped_.store(total);

        // 关键：必须"已停止 且 队列为空"才退出，否则退出前排队中的日志会被整批丢掉
        const bool finished = stopFlag_.load() && logQueue_.empty();

        lock.unlock();

        if (reportDrops)
        {
            writeRaw(buildLine(currentTimeString(), logLevelName(LogLevel::WARN), currentThreadId(),
                               "log.cpp:worker",
                               droppedNotice(total, droppedFull_.load(), droppedStopped_.load())));
        }

        if (finished)
            break;
    }
}

void LogManager::writeRaw(const std::string& line) noexcept
{
    // 日志组件绝不能因为写盘异常把业务线程/进程带崩
    try
    {
        if (rotateFileWriter_)
            rotateFileWriter_->writeLine(line);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[log] write exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[log] write exception: unknown" << std::endl;
    }
}

void LogManager::stop()
{
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (stopFlag_.exchange(true))
            return;                      // 幂等
    }
    logCondition_.notify_all();
}

bool LogManager::isStopped() const
{
    return stopFlag_.load();
}

bool LogManager::shouldLog(LogLevel level) const
{
    return static_cast<int>(level) >= static_cast<int>(level_.load(std::memory_order_relaxed));
}

void LogManager::setLevel(LogLevel level)
{
    level_.store(level, std::memory_order_relaxed);
}

LogLevel LogManager::level() const
{
    return level_.load(std::memory_order_relaxed);
}

std::uint64_t LogManager::writtenCount() const
{
    return writtenCount_.load(std::memory_order_relaxed);
}

std::uint64_t LogManager::droppedCount() const
{
    return droppedFull_.load(std::memory_order_relaxed)
         + droppedStopped_.load(std::memory_order_relaxed);
}

std::size_t LogManager::queueSize() const
{
    std::lock_guard<std::mutex> lock(logMutex_);
    return logQueue_.size();
}

void LogManager::log(LogLevel lvl, const char* file, int line, std::string message)
{
    if (!shouldLog(lvl))
        return;

    Log record;
    record.logLevel    = logLevelName(lvl);
    record.logTime     = currentTimeString();
    record.loglocation = std::string(shortFileName(file != nullptr ? file : "unknown")) + ":" +
                         std::to_string(line);
    record.logThread   = currentThreadId();
    record.logMessage  = std::move(message);

    pushLog(std::move(record));
}

void LogManager::logPrintf(LogLevel lvl, const char* file, int line, const char* fmt, ...)
{
    LogManager& self = getInstance();

    if (!self.shouldLog(lvl))
        return;                      // 被过滤时不付出格式化代价

    if (fmt == nullptr)
        fmt = "(null format)";

    // 先写栈上缓冲，不够再按 vsnprintf 返回的长度扩容，避免截断
    char stackBuffer[512];

    va_list args;
    va_start(args, fmt);

    va_list argsCopy;
    va_copy(argsCopy, args);
    const int needed = std::vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, args);
    va_end(args);

    std::string message;
    if (needed < 0)
    {
        message = "(log format error)";
    }
    else if (static_cast<std::size_t>(needed) < sizeof(stackBuffer))
    {
        message.assign(stackBuffer, static_cast<std::size_t>(needed));
    }
    else
    {
        std::vector<char> buffer(static_cast<std::size_t>(needed) + 1);
        std::vsnprintf(buffer.data(), buffer.size(), fmt, argsCopy);
        message.assign(buffer.data(), static_cast<std::size_t>(needed));
    }
    va_end(argsCopy);

    self.log(lvl, file, line, std::move(message));
}

void LogManager::pushLog(Log item)
{
    if (item.logTime.empty())
        item.logTime = currentTimeString();
    if (item.logThread.empty())
        item.logThread = currentThreadId();
    if (item.logLevel.empty())
        item.logLevel = logLevelName(LogLevel::INFO);
    if (item.loglocation.empty())
        item.loglocation = "unknown";

    std::unique_lock<std::mutex> lock(logMutex_);

    if (stopFlag_.load())
    {
        ++droppedStopped_;               // 已停止：计数丢弃，不再静默堆积
        return;
    }

    if (logQueue_.size() >= maxQueueSize_)
    {
        logQueue_.pop();                 // 丢最旧，保留最新
        ++droppedFull_;
    }

    logQueue_.push(std::move(item));
    lock.unlock();

    logCondition_.notify_one();          // 解锁后再唤醒，避免被唤醒线程立刻又阻塞在锁上
}
