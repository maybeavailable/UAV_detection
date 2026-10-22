#pragma once
#include "gateway/gateway.h"
#include <map>
#include <mutex>

namespace uav::gateway::detail {
using Time = std::chrono::steady_clock;
enum class Role { none, viewer, admin };
struct Principal
{
    Role role{Role::none};
    std::string owner;
    explicit operator bool() const { return role != Role::none; }
};
struct SecurityReply { unsigned status; boost::json::object data; std::string error; };
std::string randomId();
bool tokenEqual(const std::string &, const std::string &);

class Security
{
public:
    explicit Security(const Config &config) : config(config) {}
    Principal authenticate(const std::string &bearer) const;
    SecurityReply login(const std::string &token, const std::string &peer);
    SecurityReply refresh(const std::string &token, const std::string &peer);
    void logout(const Principal &principal);
    bool ownerAlive(const std::string &owner) const;
    SecurityReply grant(const Principal &, const std::string &stream, const std::string &protocol);
    SecurityReply playback(const Principal &, const std::string &id, bool remove, bool renew);
    bool validate(const std::string &token, const std::string &stream, const std::string &protocol) const;
    std::optional<std::string> grantId(const std::string &token, const std::string &stream, const std::string &protocol) const;
    bool validateGrant(const std::string &id, const std::string &stream, const std::string &protocol) const;
    void expire();
    void clear();
private:
    struct Session
    {
        Role role;
        std::string access, refresh;
        Time::time_point accessExpires, refreshExpires;
    };
    struct Grant
    {
        std::string owner, stream, protocol, token;
        Time::time_point expires;
    };
    struct Attempts { unsigned count{0}; Time::time_point until; };
    bool alive(const std::string &) const;
    bool limited(const std::string &peer);
    boost::json::object sessionJson(const std::string &id, const Session &) const;
    boost::json::object grantJson(const std::string &id, const Grant &, bool includeToken) const;
    void cleanup();
    Config config;
    mutable std::mutex mutex;
    std::map<std::string, Session> sessions;
    std::map<std::string, Grant> grants;
    std::map<std::string, Attempts> attempts;
};
} // namespace uav::gateway::detail
