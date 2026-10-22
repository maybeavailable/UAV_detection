#include "gateway/security.h"
#include <array>
#include <cerrno>
#include <stdexcept>
#include <sys/random.h>

namespace uav::gateway::detail {
namespace json = boost::json;
namespace {
const char *roleName(Role role) { return role == Role::admin ? "admin" : "viewer"; }
std::int64_t remaining(Time::time_point until)
{
    return std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::seconds>(until - Time::now()).count());
}
}
bool tokenEqual(const std::string &left, const std::string &right)
{
    if (left.size() != right.size()) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
    return difference == 0;
}
std::string randomId()
{
    std::array<unsigned char, 24> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("cannot generate secure identifier");
        offset += static_cast<std::size_t>(count);
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
Principal Security::authenticate(const std::string &bearer) const
{
    if (bearer.rfind("Bearer ", 0) != 0) return {};
    const auto token = bearer.substr(7);
    if (token.empty()) return {};
    if (tokenEqual(token, config.adminToken)) return {Role::admin, "root.admin"};
    if (!config.viewerToken.empty() && tokenEqual(token, config.viewerToken)) return {Role::viewer, "root.viewer"};
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto &item : sessions)
        if (item.second.accessExpires > Time::now() && tokenEqual(token, item.second.access))
            return {item.second.role, item.first};
    return {};
}
bool Security::alive(const std::string &owner) const
{
    if (owner == "root.admin" || owner == "root.viewer") return true;
    auto found = sessions.find(owner);
    return found != sessions.end() && found->second.refreshExpires > Time::now();
}
bool Security::ownerAlive(const std::string &owner) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return alive(owner);
}
void Security::cleanup()
{
    const auto now = Time::now();
    for (auto it = sessions.begin(); it != sessions.end();)
        if (it->second.refreshExpires <= now) it = sessions.erase(it); else ++it;
    for (auto it = grants.begin(); it != grants.end();)
        if (it->second.expires <= now || !alive(it->second.owner)) it = grants.erase(it); else ++it;
    for (auto it = attempts.begin(); it != attempts.end();)
        if (it->second.until <= now) it = attempts.erase(it); else ++it;
}
bool Security::limited(const std::string &peer)
{
    cleanup();
    if (!attempts.count(peer) && attempts.size() >= 256) return true;
    auto &entry = attempts[peer];
    if (entry.count == 0) entry.until = Time::now() + std::chrono::seconds(60);
    return ++entry.count > 20;
}
json::object Security::sessionJson(const std::string &id, const Session &session) const
{
    return {{"session_id", id}, {"role", roleName(session.role)}, {"token_type", "Bearer"},
        {"access_token", session.access}, {"refresh_token", session.refresh},
        {"expires_in_seconds", remaining(session.accessExpires)},
        {"refresh_expires_in_seconds", remaining(session.refreshExpires)}};
}
SecurityReply Security::login(const std::string &token, const std::string &peer)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (limited(peer)) return {429, {}, "login_rate_limited"};
    Role role = Role::none;
    if (!token.empty() && tokenEqual(token, config.adminToken)) role = Role::admin;
    else if (!token.empty() && !config.viewerToken.empty() && tokenEqual(token, config.viewerToken)) role = Role::viewer;
    if (role == Role::none) return {401, {}, "invalid_credentials"};
    if (sessions.size() >= 64) return {429, {}, "session_limit"};
    const auto id = randomId();
    Session session{role, randomId(), randomId(), Time::now() + config.accessLifetime, Time::now() + config.refreshLifetime};
    auto body = sessionJson(id, session);
    sessions.emplace(id, std::move(session));
    return {201, std::move(body), {}};
}
SecurityReply Security::refresh(const std::string &token, const std::string &peer)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (limited(peer)) return {429, {}, "login_rate_limited"};
    for (auto &item : sessions)
        if (!token.empty() && tokenEqual(token, item.second.refresh))
        {
            item.second.access = randomId();
            item.second.refresh = randomId(); // 旧刷新凭据立即作废，最长会话时间不延长
            item.second.accessExpires = std::min(Time::now() + config.accessLifetime, item.second.refreshExpires);
            return {200, sessionJson(item.first, item.second), {}};
        }
    return {401, {}, "invalid_refresh_token"};
}
void Security::logout(const Principal &principal)
{
    std::lock_guard<std::mutex> lock(mutex);
    sessions.erase(principal.owner);
    for (auto it = grants.begin(); it != grants.end();)
        if (it->second.owner == principal.owner) it = grants.erase(it); else ++it;
}
json::object Security::grantJson(const std::string &id, const Grant &grant, bool includeToken) const
{
    json::object value{{"session_id", id}, {"stream_id", grant.stream}, {"protocol", grant.protocol},
        {"expires_in_seconds", remaining(grant.expires)}};
    if (includeToken) value["media_token"] = grant.token;
    return value;
}
SecurityReply Security::grant(const Principal &principal, const std::string &stream, const std::string &protocol)
{
    std::lock_guard<std::mutex> lock(mutex);
    cleanup();
    if (!principal || !alive(principal.owner)) return {401, {}, "unauthorized"};
    if (grants.size() >= 128) return {429, {}, "playback_limit"};
    auto expires = Time::now() + config.playbackLifetime;
    auto session = sessions.find(principal.owner);
    if (session != sessions.end()) expires = std::min(expires, session->second.refreshExpires);
    const auto id = randomId();
    Grant grant{principal.owner, stream, protocol, randomId(), expires};
    auto body = grantJson(id, grant, true);
    grants.emplace(id, std::move(grant));
    return {201, std::move(body), {}};
}
SecurityReply Security::playback(const Principal &principal, const std::string &id, bool remove, bool renew)
{
    std::lock_guard<std::mutex> lock(mutex);
    cleanup();
    auto found = grants.find(id);
    if (found == grants.end() || (principal.role != Role::admin && found->second.owner != principal.owner))
        return {404, {}, "playback_not_found"};
    if (remove) { grants.erase(found); return {200, {{"deleted", id}}, {}}; }
    if (renew)
    {
        auto expires = Time::now() + config.playbackLifetime;
        auto session = sessions.find(found->second.owner);
        if (session != sessions.end()) expires = std::min(expires, session->second.refreshExpires);
        found->second.expires = expires;
        found->second.token = randomId();
    }
    return {200, grantJson(id, found->second, renew), {}};
}
bool Security::validate(const std::string &token, const std::string &stream, const std::string &protocol) const
{ return grantId(token, stream, protocol).has_value(); }
std::optional<std::string> Security::grantId(const std::string &token, const std::string &stream, const std::string &protocol) const
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto &item : grants)
        if (item.second.stream == stream && item.second.protocol == protocol && item.second.expires > Time::now() &&
            alive(item.second.owner) && tokenEqual(token, item.second.token)) return item.first;
    return {};
}
bool Security::validateGrant(const std::string &id, const std::string &stream, const std::string &protocol) const
{
    std::lock_guard<std::mutex> lock(mutex); const auto item = grants.find(id);
    return item != grants.end() && item->second.stream == stream && item->second.protocol == protocol &&
        item->second.expires > Time::now() && alive(item->second.owner);
}
void Security::expire() { std::lock_guard<std::mutex> lock(mutex); cleanup(); }
void Security::clear()
{
    std::lock_guard<std::mutex> lock(mutex);
    sessions.clear(); grants.clear(); attempts.clear();
}
} // namespace uav::gateway::detail
