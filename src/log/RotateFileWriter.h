#pragma once

#include"common/common.h"

#include<cstddef>
#include<string>
#include<fstream>

// 按大小轮转的日志文件写入器。
//
// 相比旧实现的改动：
//   1) 不再在头文件里写 `namespace fs = boost::filesystem;`（不再污染包含者），
//      改用 C++17 std::filesystem，且别名只出现在 .cpp 内；
//   2) 文件大小在内存中累加，不再每行 stat 两次；
//   3) rename 失败后退避（再涨一个 maxSize 才重试），不会每行都重试并刷屏；
//   4) 新增历史文件保留策略，超出 maxFiles 时删除最旧的轮转文件（旧实现只增不减）；
//   5) 文件打不开/写不进时回退到 stderr，日志不会静默消失。
//
// 线程约定：本类非线程安全，由 LogManager 的单个 worker 线程独占使用。
class RotateFileWriter{
public:
    // maxFiles = 0 表示不清理历史文件
    RotateFileWriter(std::string basePath, std::size_t maxSize, std::size_t maxFiles = 10);
    ~RotateFileWriter();

    RotateFileWriter(const RotateFileWriter&) = delete;
    RotateFileWriter& operator=(const RotateFileWriter&) = delete;

    void writeLine(const std::string& line);

    const std::string& path() const { return m_basePath; }
    std::size_t        currentSize() const { return m_written; }
    bool               usingStderrFallback() const { return m_warnedFallback; }

private:
    void        openFile();
    void        rotate();
    void        trimOldFiles();
    bool        isRotatedName(const std::string& name) const;
    void        writeToStderrFallback(const std::string& line);
    std::string getTimeStamp() const;

    std::string   m_basePath;
    std::string   m_dir;            // 目录，缺省为 "."
    std::string   m_stem;           // 不含扩展名的文件名
    std::string   m_ext;            // 含点扩展名，可能为空
    std::size_t   m_maxSize;
    std::size_t   m_maxFiles;
    std::size_t   m_written;        // 当前文件已写字节数（内存计数，免 stat）
    std::size_t   m_rotateRetryAt;  // 达到该大小才尝试轮转（失败后退避）
    std::size_t   m_openAttempts;   // 打开失败后的重试计数
    bool          m_warnedFallback;
    std::ofstream m_ofs;
};
