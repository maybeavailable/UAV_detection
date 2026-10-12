#pragma once
#include <boost/json.hpp>
#include <string>

namespace uav::gateway::detail {
class StateLock
{
public:
    explicit StateLock(const std::string &path);
    ~StateLock();
    StateLock(const StateLock &) = delete;
    StateLock &operator=(const StateLock &) = delete;
private:
    int fd{-1};
};
boost::json::object readRegistryFile(const std::string &path);
void writeRegistryFile(const std::string &path, const boost::json::object &value);
struct Asset { unsigned status; std::string body, contentType; std::string etag{}; };
Asset readAsset(const std::string &root, const std::string &target, bool htmlFallback);
} // namespace uav::gateway::detail
