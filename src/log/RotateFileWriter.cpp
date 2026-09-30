#include"log/RotateFileWriter.h"

#include<algorithm>
#include<cstdio>
#include<ctime>
#include<filesystem>
#include<vector>

namespace fs = std::filesystem;   // 只在 .cpp 内使用，避免污染其他翻译单元

namespace
{

// 打开失败后每 N 行重试一次：既能自动恢复，又不会每行刷屏
constexpr std::size_t kOpenRetryEvery = 100;

// 时间戳文件名形如 <stem>_20260930_110700[_01]<ext>
constexpr std::size_t kTimestampLen   = 8 + 1 + 6;   // YYYYmmdd_HHMMSS

std::size_t fileSizeOrZero(const fs::path& path)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::size_t>(size);
}

bool isDigits(const std::string& s, std::size_t pos, std::size_t count)
{
    if (count == 0 || pos + count > s.size())
        return false;
    for (std::size_t i = 0; i < count; ++i)
    {
        const char c = s[pos + i];
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

}  // namespace

RotateFileWriter::RotateFileWriter(std::string basePath, std::size_t maxSize, std::size_t maxFiles)
    : m_basePath(std::move(basePath)),
      m_maxSize(maxSize == 0 ? 1 : maxSize),
      m_maxFiles(maxFiles),
      m_written(0),
      m_rotateRetryAt(maxSize == 0 ? 1 : maxSize),
      m_openAttempts(0),
      m_warnedFallback(false)
{
    const fs::path path(m_basePath);
    const fs::path parent = path.parent_path();

    m_dir  = parent.empty() ? std::string(".") : parent.string();
    m_stem = path.stem().string();
    m_ext  = path.extension().string();

    if (!parent.empty())
    {
        std::error_code ec;
        fs::create_directories(parent, ec);
        // 目录创建失败不再抛异常（旧实现在静态初始化期抛出会导致 std::terminate），
        // 交给 openFile() 报错并走 stderr 兜底
        if (ec)
        {
            std::cerr << "[log] create directory failed: " << parent.string()
                      << ": " << ec.message() << std::endl;
        }
    }

    openFile();
}

RotateFileWriter::~RotateFileWriter()
{
    try
    {
        if (m_ofs.is_open())
        {
            m_ofs.flush();
            m_ofs.close();
        }
    }
    catch (...)
    {
        // 析构中不允许抛出
    }
}

void RotateFileWriter::writeLine(const std::string& line)
{
    if (!m_ofs.is_open() && (m_openAttempts++ % kOpenRetryEvery) == 0)
        openFile();

    if (m_ofs.is_open())
    {
        m_ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
        m_ofs.put('\n');
        m_ofs.flush();          // 每行落盘：崩溃/断电后已写日志不丢

        if (m_ofs.good())
        {
            m_written += line.size() + 1;
        }
        else
        {
            // 磁盘满等写失败：关掉文件走 stderr 兜底，别让日志静默消失
            std::cerr << "[log] write failed, falling back to stderr: " << m_basePath << std::endl;
            m_ofs.close();
            m_ofs.clear();
        }
    }

    if (!m_ofs.is_open())
    {
        writeToStderrFallback(line);
        return;
    }

    if (m_written >= m_rotateRetryAt)
        rotate();
}

void RotateFileWriter::openFile()
{
    if (m_ofs.is_open())
    {
        m_ofs.flush();
        m_ofs.close();
    }
    m_ofs.clear();

    m_ofs.open(m_basePath, std::ios::out | std::ios::app);
    if (!m_ofs.is_open())
    {
        std::cerr << "[log] open log file failed: " << m_basePath << std::endl;
        m_written = 0;
        return;
    }

    // 每次打开只 stat 一次，之后靠 m_written 累加
    m_written = fileSizeOrZero(m_basePath);
}

void RotateFileWriter::writeToStderrFallback(const std::string& line)
{
    if (!m_warnedFallback)
    {
        m_warnedFallback = true;
        std::cerr << "[log] cannot write to " << m_basePath
                  << ", logging to stderr instead" << std::endl;
    }
    std::cerr << line << std::endl;
}

std::string RotateFileWriter::getTimeStamp() const
{
    const std::time_t now = std::time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv); // linux；windows用localtime_s

    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
    return std::string(buf);
}

void RotateFileWriter::rotate()
{
    if (m_ofs.is_open())
    {
        m_ofs.flush();
        m_ofs.close();
    }

    const fs::path src(m_basePath);
    const fs::path parent = src.parent_path();
    const std::string timestamp = getTimeStamp();

    // 同一秒内多次分片：_01、_02 ……（与注释一致，两位补零）
    fs::path target = parent / (m_stem + "_" + timestamp + m_ext);
    for (int seq = 1; seq < 1000; ++seq)
    {
        std::error_code existEc;
        if (!fs::exists(target, existEc))
            break;

        char suffix[16] = {0};
        std::snprintf(suffix, sizeof(suffix), "_%02d", seq);
        target = parent / (m_stem + "_" + timestamp + suffix + m_ext);
    }

    std::error_code renameEc;
    fs::rename(src, target, renameEc);
    const bool renamed = !renameEc;
    if (!renamed)
    {
        std::cerr << "[log] rotate failed (" << renameEc.message() << "): "
                  << src.string() << " -> " << target.string() << std::endl;
    }

    openFile();   // 成功后拿到新的空文件；失败则继续追加原文件

    if (renamed)
    {
        m_rotateRetryAt = m_maxSize;
        trimOldFiles();
    }
    else
    {
        // 退避：再涨一个 maxSize 才重试，避免每行都 close/rename/open 并刷屏
        m_rotateRetryAt = m_written + m_maxSize;
    }
}

bool RotateFileWriter::isRotatedName(const std::string& name) const
{
    const std::string prefix = m_stem + "_";
    if (name.size() < prefix.size() + kTimestampLen)
        return false;
    if (name.compare(0, prefix.size(), prefix) != 0)
        return false;
    if (name.size() <= m_ext.size())
        return false;
    if (!m_ext.empty() &&
        name.compare(name.size() - m_ext.size(), m_ext.size(), m_ext) != 0)
    {
        return false;
    }

    const std::size_t middleLen = name.size() - prefix.size() - m_ext.size();
    const std::string middle    = name.substr(prefix.size(), middleLen);

    if (!isDigits(middle, 0, 8) || middle[8] != '_' || !isDigits(middle, 9, 6))
        return false;
    if (middle.size() == kTimestampLen)
        return true;
    if (middle.size() > kTimestampLen + 1 && middle[kTimestampLen] == '_' &&
        isDigits(middle, kTimestampLen + 1, middle.size() - kTimestampLen - 1))
    {
        return true;
    }
    return false;
}

void RotateFileWriter::trimOldFiles()
{
    if (m_maxFiles == 0)
        return;                       // 0 = 不清理

    std::error_code iterEc;
    fs::directory_iterator it(m_dir, iterEc);
    if (iterEc)
    {
        std::cerr << "[log] scan directory failed: " << m_dir << ": " << iterEc.message() << std::endl;
        return;
    }

    std::vector<std::string> rotated;
    for (const auto& entry : it)
    {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc))
            continue;
        const std::string name = entry.path().filename().string();
        if (isRotatedName(name))      // 只认自己产生的文件，绝不误删其他文件
            rotated.push_back(name);
    }

    if (rotated.size() <= m_maxFiles)
        return;

    // 时间戳定长且补零，字典序即时间序
    std::sort(rotated.begin(), rotated.end());

    const std::size_t removeCount = rotated.size() - m_maxFiles;
    for (std::size_t i = 0; i < removeCount; ++i)
    {
        std::error_code removeEc;
        fs::remove(fs::path(m_dir) / rotated[i], removeEc);
        if (removeEc)
        {
            std::cerr << "[log] remove old log failed: " << rotated[i]
                      << ": " << removeEc.message() << std::endl;
        }
    }
}
