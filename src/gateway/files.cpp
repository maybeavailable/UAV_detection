#include "gateway/files.h"
#include "gateway/security.h"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <openssl/sha.h>

namespace uav::gateway::detail {
namespace fs = std::filesystem;
namespace {
std::string readFile(const fs::path &path, std::size_t limit)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > static_cast<std::streamoff>(limit))
        throw std::runtime_error("cannot read file or file exceeds size limit");
    file.seekg(0);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
}
StateLock::StateLock(const std::string &path)
{
    if (path.empty()) return;
    auto directory = fs::path(path).parent_path();
    if (!directory.empty()) fs::create_directories(directory);
    fd = ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (fd >= 0) ::close(fd);
        fd = -1;
        throw std::runtime_error("gateway state file is locked or inaccessible");
    }
}
StateLock::~StateLock() { if (fd >= 0) ::close(fd); }
boost::json::object readRegistryFile(const std::string &path)
{
    boost::json::parse_options options;
    options.max_depth = 20;
    return boost::json::parse(readFile(path, 4 * 1024 * 1024), {}, options).as_object();
}
void writeRegistryFile(const std::string &path, const boost::json::object &value)
{
    if (path.empty()) return;
    fs::path destination(path);
    auto directory = destination.parent_path();
    if (directory.empty()) directory = ".";
    fs::create_directories(directory);
    const auto temporary = destination.string() + ".tmp." + randomId();
    const auto text = boost::json::serialize(value);
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot create gateway state file");
    try
    {
        std::size_t offset = 0;
        while (offset < text.size())
        {
            const auto count = ::write(fd, text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("cannot write gateway state file");
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(fd) != 0) throw std::runtime_error("cannot sync gateway state file");
        ::close(fd); fd = -1;
        if (::rename(temporary.c_str(), destination.c_str()) != 0)
            throw std::runtime_error("cannot replace gateway state file");
        // rename 已是提交点。目录同步失败不回滚内存，防止内存与已替换文件分叉。
        int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir >= 0) { ::fsync(dir); ::close(dir); }
    }
    catch (...)
    {
        if (fd >= 0) ::close(fd);
        ::unlink(temporary.c_str());
        throw;
    }
}
Asset readAsset(const std::string &root, const std::string &target, bool htmlFallback)
{
    auto path = target.substr(0, target.find('?'));
    std::string decoded;
    auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        char value = path[i];
        if (value == '%')
        {
            if (i + 2 >= path.size() || hex(path[i+1]) < 0 || hex(path[i+2]) < 0)
                return {400, "invalid path", "text/plain"};
            value = static_cast<char>(hex(path[i+1]) * 16 + hex(path[i+2]));
            i += 2;
        }
        if (value == '\0' || value == '\\' || static_cast<unsigned char>(value) < 32)
            return {400, "invalid path", "text/plain"};
        decoded += value;
    }
    if (decoded.empty() || decoded[0] != '/') return {400, "invalid path", "text/plain"};
    fs::path relative(decoded.substr(1));
    for (const auto &part : relative)
        if (part == ".." || (!part.empty() && part.string()[0] == '.'))
            return {403, "forbidden", "text/plain"};
    auto base = fs::canonical(root);
    auto candidate = fs::weakly_canonical(base / relative);
    auto underRoot = [&](const fs::path &file) {
        auto r = file.lexically_relative(base);
        return !r.empty() && *r.begin() != "..";
    };
    if (candidate != base && !underRoot(candidate)) return {403, "forbidden", "text/plain"};
    if (fs::is_directory(candidate)) candidate /= "index.html";
    if (!fs::is_regular_file(candidate) && htmlFallback && relative.extension().empty()) candidate = base / "index.html";
    candidate = fs::weakly_canonical(candidate);
    if (!underRoot(candidate)) return {403, "forbidden", "text/plain"};
    if (!fs::is_regular_file(candidate)) return {404, "not found", "text/plain"};
    static const std::map<std::string, std::string> types{
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".json", "application/json"}, {".svg", "image/svg+xml"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".ico", "image/x-icon"}, {".webp", "image/webp"}, {".woff2", "font/woff2"},
        {".txt", "text/plain; charset=utf-8"}, {".map", "application/json"}};
    auto type = types.find(candidate.extension().string());
    if (type == types.end()) return {403, "unsupported asset type", "text/plain"};
    if (fs::file_size(candidate) > 4 * 1024 * 1024) return {413, "asset exceeds 4 MiB", "text/plain"};
    auto body = readFile(candidate, 4 * 1024 * 1024);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(body.data()), body.size(), digest);
    constexpr char digits[] = "0123456789abcdef";
    std::string etag = "\"";
    for (auto value : digest) { etag += digits[value >> 4]; etag += digits[value & 15]; }
    etag += '"';
    return {200, std::move(body), type->second, std::move(etag)};
}
} // namespace uav::gateway::detail
