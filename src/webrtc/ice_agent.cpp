#include"webrtc/ice_agent.h"
#include"webrtc/sdp.h"

#include<boost/asio/ip/udp.hpp>
#include<boost/asio/post.hpp>
#include<boost/asio/steady_timer.hpp>
#include<boost/asio/strand.hpp>
#include<openssl/rand.h>

#include<algorithm>
#include<deque>
#include<map>
#include<mutex>
#include<set>
#include<stdexcept>
#include<utility>

#if defined(__unix__) || defined(__APPLE__)
#include<ifaddrs.h>
#include<net/if.h>
#include<netinet/in.h>
#endif

namespace webrtc
{
namespace net = boost::asio;
using Type = StunAttributeType;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using Endpoint = UdpSocket::Endpoint;
using ErrorCode = UdpSocket::ErrorCode;
namespace
{
bool iceString(std::string_view text, std::size_t minimum)
{
    return text.size() >= minimum && text.size() <= 256 && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
    });
}
std::string randomText(std::size_t size)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    StunBytes bytes(size);
    if (RAND_bytes(bytes.data(), static_cast<int>(size)) != 1) throw std::runtime_error("ICE random generation failed");
    std::string text;
    for (auto byte : bytes) text += alphabet[byte & 63];
    return text;
}
std::uint64_t random64()
{
    const auto bytes = generateStunTransactionId();
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}
bool usable(const Endpoint& endpoint)
{
    return endpoint.port() != 0 && !endpoint.address().is_unspecified() && !endpoint.address().is_multicast() &&
        (!endpoint.address().is_v6() || endpoint.address().to_v6().scope_id() == 0) &&
        (!endpoint.address().is_v4() || endpoint.address().to_v4().to_uint() != 0xffffffff);
}
std::vector<net::ip::address> interfaceAddresses(bool loopback)
{
    std::set<net::ip::address> addresses;
#if defined(__unix__) || defined(__APPLE__)
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) throw std::runtime_error("cannot enumerate ICE interfaces");
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owner(list, freeifaddrs);
    for (auto* item = list; item; item = item->ifa_next)
    {
        if (!item->ifa_addr || !(item->ifa_flags & IFF_UP) || (!loopback && (item->ifa_flags & IFF_LOOPBACK))) continue;
        if (item->ifa_addr->sa_family == AF_INET)
        {
            const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
            net::ip::address_v4::bytes_type bytes{};
            std::copy_n(reinterpret_cast<const unsigned char*>(&address->sin_addr), 4, bytes.begin());
            const net::ip::address value = net::ip::address_v4(bytes);
            if (!value.is_unspecified() && !value.is_multicast()) addresses.insert(value);
        }
        else if (item->ifa_addr->sa_family == AF_INET6)
        {
            const auto* address = reinterpret_cast<const sockaddr_in6*>(item->ifa_addr);
            net::ip::address_v6::bytes_type bytes{};
            std::copy_n(reinterpret_cast<const unsigned char*>(&address->sin6_addr), 16, bytes.begin());
            const auto value = net::ip::address_v6(bytes);
            if (!value.is_unspecified() && !value.is_multicast() && !value.is_link_local() && address->sin6_scope_id == 0)
                addresses.insert(value);
        }
    }
#else
    if (loopback) addresses.insert(net::ip::address_v4::loopback());
#endif
    return {addresses.begin(), addresses.end()};
}
bool sameCredentials(const IceCredentials& a, const IceCredentials& b)
{
    return a.ufrag == b.ufrag && a.password == b.password;
}
void complete(const IceAgent::Completion& done, ErrorCode ec = {}) { if (done) done(ec); }
} // namespace

bool IceCredentials::validate(std::string* reason) const
{
    if (!iceString(ufrag, 4) || !iceString(password, 22))
    {
        if (reason) *reason = "invalid ICE credentials length or alphabet";
        return false;
    }
    if (reason) reason->clear();
    return true;
}
IceCredentials IceCredentials::generate() { return {randomText(16), randomText(32)}; }

struct IceAgent::Impl : std::enable_shared_from_this<IceAgent::Impl>
{
    struct Transport { std::shared_ptr<UdpSocket> socket; IceCandidate host; unsigned preference; };
    struct Remote { IceCandidate candidate; Endpoint endpoint; std::string ufrag; };
    struct Pair
    {
        IceCandidatePair info;
        std::size_t transport, remote;
        bool triggered{false}, pendingNomination{false}, nominating{false};
        Clock::time_point lastConsent{}, nextConsent{};
    };
    enum class Kind { Gather, Check, Nomination, Consent };
    struct Transaction
    {
        Kind kind;
        std::size_t transport;
        Endpoint destination;
        std::uint64_t pair{0};
        StunMessage request;
        StunBytes wire;
        unsigned sends{1};
        Ms rto{500};
        Clock::time_point next, expires;
        bool obsolete{false};
    };
    struct GatherJob { std::size_t transport; Endpoint server; };
    struct EarlyCheck { std::size_t transport; Endpoint source; std::string ufrag; std::uint32_t priority; bool nominate; };
    struct CachedResponse
    {
        std::size_t transport;
        Endpoint source;
        StunTransactionId id;
        StunBytes request, response;
        Clock::time_point expires;
    };
    struct Resolution { std::shared_ptr<net::ip::udp::resolver> resolver; Completion done; };

    net::io_context& io;
    net::strand<net::io_context::executor_type> strand;
    net::steady_timer timer;
    Config config;
    Callbacks callbacks;
    IceCredentials local;
    std::optional<IceCredentials> remote;
    StunIceRole role;
    std::uint64_t tieBreaker, generation{0}, nextPair{1};
    IceAgentState state{IceAgentState::New};
    IceGatheringState gathering{IceGatheringState::New};
    bool started{false}, gathered{false}, remoteEnd{false}, remoteLite{false}, ticking{false};
    bool closing{false}, closeFinished{false};
    Clock::time_point startedAt;
    std::vector<Transport> transports;
    std::vector<IceCandidate> locals;
    std::vector<Remote> remotes;
    std::vector<Pair> pairs;
    std::map<StunTransactionId, Transaction> transactions;
    std::deque<GatherJob> gatherJobs;
    std::deque<std::uint64_t> triggered;
    std::deque<EarlyCheck> earlyChecks;
    std::deque<CachedResponse> responseCache;
    std::vector<std::shared_ptr<Resolution>> resolutions;
    std::vector<Completion> closeCallbacks;
    std::size_t closingSockets{0};
    mutable std::mutex snapshotMutex;
    Snapshot published;

    Impl(net::io_context& context, Config options, Callbacks handlers)
        : io(context), strand(net::make_strand(context)), timer(strand), config(std::move(options)), callbacks(std::move(handlers)),
          local(config.credentials), role(config.role), tieBreaker(config.tieBreaker ? *config.tieBreaker : random64())
    {
        if (local.ufrag.empty() && local.password.empty()) local = IceCredentials::generate();
        if (!local.validate() || (role != StunIceRole::Controlled && role != StunIceRole::Controlling) ||
            config.components.empty() || config.components.size() > 256 || config.maxCandidates == 0 || config.maxCandidates > 4096 ||
            config.maxCandidatePairs == 0 || config.maxCandidatePairs > 4096 || config.maxTransmissions == 0 || config.maxTransmissions > 10 ||
            config.maxCandidates < config.components.size() || config.maxCandidatePairs < config.components.size() ||
            config.maxPendingTransactions == 0 || config.maxPendingTransactions > 16384 ||
            config.checkInterval < Ms(5) || config.checkInterval > Ms(1000) || config.initialRto < Ms(500) || config.initialRto > Ms(10000) ||
            config.checkTimeout < Ms(500) || config.checkTimeout > std::chrono::hours(24) ||
            config.consentInterval < Ms(5000) || config.consentInterval > Ms(10000) ||
            config.consentTimeout > Ms(30000) || config.consentTimeout < config.consentInterval * 2 ||
            config.disconnectedTimeout < config.consentInterval || config.disconnectedTimeout >= config.consentTimeout ||
            config.bindings.size() > config.maxCandidates || config.stunServers.size() > 16)
            throw std::invalid_argument("invalid ICE agent configuration");
        std::set<std::uint16_t> components;
        for (auto component : config.components)
            if (component == 0 || component > 256 || !components.insert(component).second)
                throw std::invalid_argument("invalid ICE components");
        for (const auto& binding : config.bindings)
            if (!componentKnown(binding.component) || binding.localPreference > 65535)
                throw std::invalid_argument("invalid ICE local binding");
        for (const auto& server : config.stunServers)
            if (!usable(server)) throw std::invalid_argument("invalid ICE STUN server");
        publish();
    }
    bool componentKnown(std::uint16_t component) const
    {
        return std::find(config.components.begin(), config.components.end(), component) != config.components.end();
    }
    bool terminal() const { return closing || state == IceAgentState::Failed; }
    void publish()
    {
        Snapshot next;
        next.state = state; next.gatheringState = gathering; next.role = role;
        next.localCredentials = local; next.remoteCredentials = remote;
        next.localCandidates = locals; next.remoteEndOfCandidates = remoteEnd;
        next.pendingTransactions = transactions.size();
        for (const auto& value : remotes) next.remoteCandidates.push_back(value.candidate);
        for (const auto& pair : pairs)
        {
            next.checklist.push_back(pair.info);
            if (pair.info.nominated) next.selectedPairs.push_back(pair.info);
        }
        std::sort(next.checklist.begin(), next.checklist.end(), [](const auto& a, const auto& b) { return a.priority > b.priority; });
        std::lock_guard<std::mutex> lock(snapshotMutex);
        published = std::move(next);
    }
    void error(ErrorCode ec, const char* message)
    {
        if (callbacks.onError) callbacks.onError(ec, message);
    }
    void setState(IceAgentState value)
    {
        if (state == value) return;
        state = value; publish();
        if (callbacks.onState) callbacks.onState(value);
    }
    void setGathering(IceGatheringState value)
    {
        if (gathering == value) return;
        gathering = value; publish();
        if (callbacks.onGatheringState) callbacks.onGatheringState(value);
    }
    void fail(const char* message, ErrorCode ec = net::error::timed_out)
    {
        transactions.clear(); triggered.clear(); gatherJobs.clear();
        for (auto& pair : pairs)
        {
            pair.info.valid = false; pair.info.nominated = false;
            if (pair.info.state != IcePairState::Succeeded) pair.info.state = IcePairState::Failed;
        }
        if (gathering == IceGatheringState::Gathering) setGathering(IceGatheringState::Complete);
        setState(IceAgentState::Failed);
        error(ec, message);
    }
    template<class Function> void guarded(Function function)
    {
        try { function(); }
        catch (const std::exception&) { if (!closing) fail("ICE internal operation failed", net::error::fault); }
    }
    Pair* findPair(std::uint64_t id)
    {
        const auto it = std::find_if(pairs.begin(), pairs.end(), [id](const Pair& pair) { return pair.info.id == id; });
        return it == pairs.end() ? nullptr : &*it;
    }
    Pair* selected(std::uint16_t component)
    {
        const auto it = std::find_if(pairs.begin(), pairs.end(), [component](const Pair& pair) {
            return pair.info.nominated && pair.info.local.component == component;
        });
        return it == pairs.end() ? nullptr : &*it;
    }
    void localCandidate(IceCandidate candidate)
    {
        for (const auto& value : locals)
            if (value.component == candidate.component && value.address == candidate.address && value.port == candidate.port &&
                value.relatedAddress == candidate.relatedAddress && value.relatedPort == candidate.relatedPort) return;
        if (locals.size() >= config.maxCandidates) return;
        locals.push_back(candidate); publish();
        if (callbacks.onLocalCandidate) callbacks.onLocalCandidate(std::move(candidate));
    }
    Ms consentDelay() const
    {
        return Ms(config.consentInterval.count() * (800 + random64() % 401) / 1000);
    }
    void schedule()
    {
        if (ticking || terminal()) return;
        ticking = true;
        timer.expires_after(config.checkInterval);
        const auto weak = weak_from_this();
        const auto currentGeneration = generation;
        timer.async_wait([weak, currentGeneration](ErrorCode ec) {
            if (const auto self = weak.lock())
            {
                if (self->generation != currentGeneration) return;
                self->ticking = false;
                if (!ec && !self->terminal()) self->guarded([&] { self->tick(); });
            }
        });
    }
    ErrorCode registerTransport(std::shared_ptr<UdpSocket> socket, std::uint16_t component, unsigned preference)
    {
        if (terminal()) return net::error::operation_aborted;
        if (!socket || !socket->isOpen() || !usable(socket->localEndpoint()) || socket->remoteEndpoint() ||
            !componentKnown(component) || preference > 65535) return net::error::invalid_argument;
        for (const auto& transport : transports)
            if (transport.socket == socket || transport.socket->localEndpoint() == socket->localEndpoint()) return net::error::already_started;
        if (transports.size() >= config.maxCandidates || locals.size() >= config.maxCandidates) return net::error::no_buffer_space;
        const auto index = transports.size();
        // 不同组件共享同一 IP 的 host foundation；不同接口有独立 local preference。
        std::string foundation = "h" + std::to_string(index + 1);
        for (const auto& transport : transports)
            if (transport.socket->localEndpoint().address() == socket->localEndpoint().address()) foundation = transport.host.foundation;
        const auto host = IceCandidate::host(socket->localEndpoint(), foundation, component, preference);
        transports.push_back({socket, host, preference});
        const auto weak = weak_from_this();
        socket->startReceive([weak, index](auto datagram) {
            if (const auto self = weak.lock()) net::post(self->strand, [self, index, datagram = std::move(datagram)]() mutable {
                if (!self->terminal()) self->guarded([&] { self->receive(index, std::move(datagram)); });
            });
        }, [weak](ErrorCode ec) {
            if (const auto self = weak.lock()) net::post(self->strand, [self, ec] {
                if (!self->terminal()) self->failSocket(ec);
            });
        });
        localCandidate(host);
        for (const auto& server : config.stunServers)
            if (server.protocol() == socket->localEndpoint().protocol()) gatherJobs.push_back({index, server});
        if (!gatherJobs.empty()) setGathering(IceGatheringState::Gathering);
        for (std::size_t i = 0; i < remotes.size(); ++i) makePair(index, i);
        schedule();
        return {};
    }
    void failSocket(ErrorCode ec)
    {
        fail("ICE local UDP transport failed", ec);
    }
    ErrorCode gather()
    {
        if (terminal()) return net::error::operation_aborted;
        if (gathered) return {};
        gathered = true; setGathering(IceGatheringState::Gathering);
        auto bindings = config.bindings;
        if (bindings.empty())
        {
            const auto addresses = interfaceAddresses(config.includeLoopback);
            unsigned index = 0;
            for (const auto& address : addresses)
            {
                for (auto component : config.components) bindings.push_back({{address, 0}, component, 65535 - index});
                ++index;
                if (bindings.size() >= config.maxCandidates) break;
            }
        }
        ErrorCode lastError;
        for (const auto& binding : bindings)
        {
            std::vector<net::ip::address> addresses{binding.endpoint.address()};
            if (binding.endpoint.address().is_unspecified())
            {
                addresses = interfaceAddresses(config.includeLoopback);
                addresses.erase(std::remove_if(addresses.begin(), addresses.end(), [&](const auto& address) {
                    return address.is_v4() != binding.endpoint.address().is_v4();
                }), addresses.end());
            }
            unsigned offset = 0;
            for (const auto& address : addresses)
            {
                UdpSocket::Config options;
                options.localEndpoint = {address, binding.endpoint.port()};
                auto socket = std::make_shared<UdpSocket>(io, options, lastError);
                if (!lastError) lastError = registerTransport(socket, binding.component,
                    binding.localPreference >= offset ? binding.localPreference - offset : 0);
                if (lastError) error(lastError, "ICE local candidate bind failed");
                ++offset;
            }
        }
        finishGathering(); schedule(); publish();
        return transports.empty() ? (lastError ? lastError : ErrorCode(net::error::address_family_not_supported)) : ErrorCode{};
    }
    void finishGathering()
    {
        if (gathering != IceGatheringState::Gathering || !gatherJobs.empty()) return;
        for (const auto& transaction : transactions) if (transaction.second.kind == Kind::Gather) return;
        setGathering(IceGatheringState::Complete);
    }
    Pair* makePair(std::size_t transport, std::size_t remoteIndex)
    {
        const auto& base = transports[transport];
        const auto& peer = remotes[remoteIndex];
        if (base.host.component != peer.candidate.component || base.socket->localEndpoint().protocol() != peer.endpoint.protocol() ||
            selected(base.host.component)) return nullptr;
        for (auto& pair : pairs) if (pair.transport == transport && pair.info.remoteEndpoint == peer.endpoint) return &pair;
        const auto priority = candidatePairPriority(base.host.priority, peer.candidate.priority, role == StunIceRole::Controlling);
        if (pairs.size() >= config.maxCandidatePairs)
        {
            auto worst = pairs.end();
            for (auto it = pairs.begin(); it != pairs.end(); ++it)
                if (!it->info.valid && it->info.state != IcePairState::InProgress && !it->triggered &&
                    (worst == pairs.end() || it->info.state == IcePairState::Failed || it->info.priority < worst->info.priority)) worst = it;
            if (worst == pairs.end() || (worst->info.state != IcePairState::Failed && worst->info.priority >= priority)) return nullptr;
            pairs.erase(worst);
        }
        Pair pair;
        pair.transport = transport; pair.remote = remoteIndex;
        pair.info.id = nextPair++; pair.info.priority = priority;
        pair.info.local = base.host; pair.info.remote = peer.candidate;
        pair.info.localEndpoint = base.socket->localEndpoint(); pair.info.remoteEndpoint = peer.endpoint;
        pairs.push_back(std::move(pair));
        thaw();
        return &pairs.back();
    }
    void thaw()
    {
        // 每个 foundation 首先解冻最低组件中的最高优先级候选对。
        std::map<std::pair<std::string, std::string>, Pair*> foundations;
        for (auto& pair : pairs)
        {
            const auto key = std::make_pair(transports[pair.transport].host.foundation, pair.info.remote.foundation);
            auto it = foundations.find(key);
            if (it == foundations.end() || pair.info.local.component < it->second->info.local.component ||
                (pair.info.local.component == it->second->info.local.component && pair.info.priority > it->second->info.priority))
                foundations[key] = &pair;
        }
        for (const auto& value : foundations)
            if (value.second->info.state == IcePairState::Frozen) value.second->info.state = IcePairState::Waiting;
    }
    void enqueue(Pair& pair)
    {
        if (!pair.triggered && !pair.info.nominated)
        {
            pair.triggered = true; triggered.push_back(pair.info.id);
        }
    }
    void changeRole(StunIceRole value)
    {
        if (role == value) return;
        role = value;
        for (auto& pair : pairs)
        {
            pair.info.priority = candidatePairPriority(pair.info.local.priority, pair.info.remote.priority, role == StunIceRole::Controlling);
            pair.nominating = false;
        }
        for (auto& transaction : transactions)
            if (transaction.second.kind == Kind::Check || transaction.second.kind == Kind::Nomination) transaction.second.obsolete = true;
        for (auto& pair : pairs)
            if (!pair.info.nominated && pair.info.state == IcePairState::InProgress)
            { pair.info.state = IcePairState::Waiting; enqueue(pair); }
        publish();
    }
    ErrorCode setRemote(IceCredentials value, bool lite)
    {
        if (terminal()) return net::error::operation_aborted;
        if (!value.validate() || local.ufrag.size() + value.ufrag.size() + 1 > 508) return net::error::invalid_argument;
        if (remote && !sameCredentials(*remote, value)) return net::error::already_started; // 先 restart，再交换新凭据
        if (!remote)
        {
            remotes.erase(std::remove_if(remotes.begin(), remotes.end(), [&](const Remote& peer) {
                return !peer.ufrag.empty() && peer.ufrag != value.ufrag;
            }), remotes.end());
            pairs.clear(); triggered.clear();
        }
        remote = std::move(value); remoteLite = lite;
        for (std::size_t i = 0; i < transports.size(); ++i)
            for (std::size_t j = 0; j < remotes.size(); ++j) makePair(i, j);
        if (remoteLite) changeRole(StunIceRole::Controlling);
        auto pending = std::move(earlyChecks); earlyChecks.clear();
        for (const auto& check : pending)
            if (check.ufrag == remote->ufrag) learnCheck(check);
        schedule(); publish();
        return {};
    }
    ErrorCode insertRemote(IceCandidate candidate, Endpoint endpoint, std::string ufrag)
    {
        if (terminal()) return net::error::operation_aborted;
        if (!candidate.validate() || candidate.transport != IceCandidateTransport::Udp || !componentKnown(candidate.component) ||
            !usable(endpoint) || endpoint.port() != candidate.port || (remote && !ufrag.empty() && ufrag != remote->ufrag))
            return net::error::invalid_argument;
        for (std::size_t i = 0; i < remotes.size(); ++i)
            if (remotes[i].candidate.component == candidate.component && remotes[i].endpoint == endpoint)
            {
                if (remotes[i].candidate.type == IceCandidateType::PeerReflexive && candidate.type != IceCandidateType::PeerReflexive)
                {
                    remotes[i] = {candidate, endpoint, std::move(ufrag)};
                    for (auto& pair : pairs) if (pair.remote == i)
                    {
                        pair.info.remote = candidate;
                        pair.info.priority = candidatePairPriority(pair.info.local.priority, candidate.priority, role == StunIceRole::Controlling);
                    }
                }
                publish(); return {};
            }
        if (remotes.size() >= config.maxCandidates) return net::error::no_buffer_space;
        remotes.push_back({std::move(candidate), endpoint, std::move(ufrag)});
        for (std::size_t i = 0; i < transports.size(); ++i) makePair(i, remotes.size() - 1);
        schedule(); publish();
        return {};
    }
    void addRemote(IceCandidate candidate, Completion done, std::optional<Endpoint> endpoint, std::string ufrag)
    {
        const auto extension = candidate.extension("ufrag");
        if (ufrag.empty() && extension) ufrag = *extension;
        if (terminal()) { complete(done, net::error::operation_aborted); return; }
        if (!candidate.validate() || candidate.transport != IceCandidateTransport::Udp || !componentKnown(candidate.component) ||
            (extension && !ufrag.empty() && *extension != ufrag) || (remote && !ufrag.empty() && remote->ufrag != ufrag) || remoteEnd)
        { complete(done, net::error::invalid_argument); return; }
        if (!endpoint) endpoint = candidate.udpEndpoint();
        if (endpoint) { complete(done, insertRemote(std::move(candidate), *endpoint, std::move(ufrag))); return; }
        if (resolutions.size() >= 32) { complete(done, net::error::no_buffer_space); return; }
        auto resolution = std::make_shared<Resolution>();
        resolution->resolver = std::make_shared<net::ip::udp::resolver>(strand); resolution->done = std::move(done);
        resolutions.push_back(resolution);
        const auto currentGeneration = generation;
        const auto weak = weak_from_this();
        const auto hostname = candidate.address, service = std::to_string(candidate.port);
        resolution->resolver->async_resolve(hostname, service,
            [weak, resolution, currentGeneration, candidate = std::move(candidate), ufrag = std::move(ufrag)]
            (ErrorCode ec, net::ip::udp::resolver::results_type results) mutable {
                const auto self = weak.lock(); if (!self) return;
                const auto it = std::find(self->resolutions.begin(), self->resolutions.end(), resolution);
                if (it == self->resolutions.end()) return; // 已由 close/restart 完成回调
                self->resolutions.erase(it);
                if (self->closing || self->generation != currentGeneration) ec = net::error::operation_aborted;
                if (!ec)
                {
                    ec = net::error::host_not_found;
                    for (const auto& result : results)
                        if (!self->insertRemote(candidate, result.endpoint(), ufrag)) ec.clear();
                }
                complete(resolution->done, ec); self->publish(); self->schedule();
            });
    }
    void sendWire(std::size_t transport, StunBytes wire, const Endpoint& destination,
                  std::optional<StunTransactionId> transaction = std::nullopt)
    {
        const auto weak = weak_from_this();
        const auto currentGeneration = generation;
        transports[transport].socket->asyncSendTo(std::move(wire), destination,
            [weak, transaction, currentGeneration](ErrorCode ec, std::size_t) {
                if (!ec) return;
                if (const auto self = weak.lock()) net::post(self->strand, [self, ec, transaction, currentGeneration] {
                    if (self->terminal() || self->generation != currentGeneration) return;
                    if (transaction)
                    {
                        const auto it = self->transactions.find(*transaction);
                        if (it != self->transactions.end())
                        {
                            auto failed = std::move(it->second); self->transactions.erase(it);
                            self->transactionFailed(failed);
                        }
                    }
                    self->error(ec, "ICE UDP send failed"); self->publish();
                });
            });
    }
    void beginTransaction(Kind kind, std::size_t transport, Endpoint destination, std::uint64_t pairId = 0)
    {
        StunMessage request;
        StunEncodeOptions options;
        if (kind == Kind::Gather) request = StunMessage::bindingRequest("UAV_detection");
        else
        {
            request = StunMessage::iceBindingRequest(local.ufrag, remote->ufrag,
                candidatePriority(IceCandidateType::PeerReflexive, transports[transport].preference, transports[transport].host.component),
                role, tieBreaker, kind == Kind::Nomination);
            options = {StunIntegrity::Sha1, stunShortTermKey(remote->password), true};
        }
        const auto now = Clock::now();
        Ms rto = config.initialRto;
        if (kind != Kind::Gather && kind != Kind::Consent)
        {
            std::size_t pending = 0;
            for (const auto& pair : pairs)
                if (pair.info.state == IcePairState::Waiting || pair.info.state == IcePairState::InProgress) ++pending;
            rto = std::max(rto, config.checkInterval * static_cast<Ms::rep>(std::max<std::size_t>(1, pending)));
        }
        Transaction value{kind, transport, destination, pairId, request, request.encode(options), 1, rto,
            now + rto, now + rto * ((1u << (config.maxTransmissions - 1)) - 1 + 16), false};
        if (kind == Kind::Consent) value.expires = value.next = now + config.consentInterval;
        const auto id = request.transactionId;
        const auto bytes = value.wire;
        transactions.emplace(id, std::move(value));
        if (auto* pair = findPair(pairId))
        {
            if (kind != Kind::Consent) pair->info.state = IcePairState::InProgress;
            if (kind == Kind::Nomination) pair->nominating = true;
        }
        sendWire(transport, bytes, destination, id);
    }
    void transactionFailed(const Transaction& transaction)
    {
        if (transaction.kind == Kind::Gather)
        {
            error(net::error::timed_out, "ICE STUN gathering transaction failed"); finishGathering(); return;
        }
        if (transaction.kind == Kind::Consent || transaction.obsolete) return;
        if (auto* pair = findPair(transaction.pair))
        {
            pair->info.state = IcePairState::Failed; pair->info.valid = false; pair->nominating = false;
            if (transaction.kind == Kind::Nomination || pair->pendingNomination)
            { fail("ICE nominated candidate check failed"); return; }
            thawAfter(*pair);
        }
    }
    void thawAfter(const Pair& successful)
    {
        const auto foundation = std::make_pair(transports[successful.transport].host.foundation, successful.info.remote.foundation);
        for (auto& pair : pairs)
            if (pair.info.state == IcePairState::Frozen &&
                std::make_pair(transports[pair.transport].host.foundation, pair.info.remote.foundation) == foundation)
                pair.info.state = IcePairState::Waiting;
    }
    void select(Pair& pair)
    {
        if (selected(pair.info.local.component) || !pair.info.valid) return;
        pair.info.nominated = true; pair.nominating = false;
        pair.nextConsent = Clock::now() + consentDelay();
        for (auto& other : pairs)
            if (other.info.local.component == pair.info.local.component && other.info.id != pair.info.id)
            { other.triggered = false; other.nominating = false; }
        for (auto& transaction : transactions)
            if (transaction.second.pair && transaction.second.pair != pair.info.id)
                if (const auto* other = findPair(transaction.second.pair))
                    if (other->info.local.component == pair.info.local.component) transaction.second.obsolete = true;
        publish();
        if (callbacks.onSelectedPair) callbacks.onSelectedPair(pair.info);
    }
    void updateState()
    {
        if (!started || terminal()) return;
        bool allValid = true, allSelected = true, disconnected = false;
        const auto now = Clock::now();
        for (auto component : config.components)
        {
            auto* choice = selected(component);
            if (!choice) allSelected = false;
            bool valid = false;
            for (const auto& pair : pairs) if (pair.info.local.component == component && pair.info.valid) valid = true;
            if (!valid) allValid = false;
            if (choice)
            {
                if (now - choice->lastConsent >= config.consentTimeout) { fail("ICE consent expired; restart required"); return; }
                if (now - choice->lastConsent >= config.disconnectedTimeout) disconnected = true;
            }
        }
        if (allSelected) { setState(disconnected ? IceAgentState::Disconnected : IceAgentState::Completed); return; }
        setState(allValid ? IceAgentState::Connected : IceAgentState::Checking);
        bool active = false;
        for (const auto& pair : pairs)
            if (!selected(pair.info.local.component) &&
                (pair.info.state == IcePairState::Frozen || pair.info.state == IcePairState::Waiting || pair.info.state == IcePairState::InProgress)) active = true;
        if ((!allValid && !active && remoteEnd && gathering == IceGatheringState::Complete && resolutions.empty()) ||
            now - startedAt >= config.checkTimeout) fail("ICE checklist exhausted or timed out");
    }
    void tick()
    {
        const auto now = Clock::now();
        while (!responseCache.empty() && responseCache.front().expires <= now) responseCache.pop_front();
        std::vector<Transaction> failed;
        for (auto it = transactions.begin(); it != transactions.end();)
        {
            auto& value = it->second;
            if (now >= value.expires)
            { failed.push_back(std::move(value)); it = transactions.erase(it); continue; }
            if (!value.obsolete && value.kind != Kind::Consent && now >= value.next && value.sends < config.maxTransmissions)
            {
                ++value.sends;
                sendWire(value.transport, value.wire, value.destination, it->first);
                value.rto *= 2;
                value.next = now + value.rto;
                if (value.sends == config.maxTransmissions) value.expires = now + config.initialRto * 16;
            }
            ++it;
        }
        for (const auto& value : failed) { transactionFailed(value); if (terminal()) return; }
        finishGathering(); updateState();
        if (terminal()) return;
        // 取消重传但保留等待响应的 triggered 旧事务也占额度，防止反复触发无限增长。
        if (transactions.size() >= config.maxPendingTransactions) { publish(); schedule(); return; }
        // 每个 Ta 最多创建一个新事务；提名、triggered checks 优先于常规检查。
        Pair* work = nullptr;
        Kind kind = Kind::Check;
        if (started && remote)
        {
            for (auto& pair : pairs)
                if (pair.info.nominated && now >= pair.nextConsent)
                { work = &pair; kind = Kind::Consent; break; }
            while (!work && !triggered.empty())
            {
                const auto id = triggered.front(); triggered.pop_front();
                auto* pair = findPair(id);
                if (!pair || !pair->triggered || selected(pair->info.local.component)) continue;
                pair->triggered = false; work = pair;
                if (pair->nominating && role == StunIceRole::Controlling) kind = Kind::Nomination;
            }
            if (!work && role == StunIceRole::Controlling)
                for (auto& pair : pairs)
                    if (pair.info.valid && !pair.nominating && !selected(pair.info.local.component) &&
                        std::none_of(pairs.begin(), pairs.end(), [&](const Pair& other) {
                            return other.info.local.component == pair.info.local.component && other.nominating;
                        }) &&
                        (!work || pair.info.priority > work->info.priority)) { work = &pair; kind = Kind::Nomination; }
            if (!work)
            {
                for (auto& pair : pairs)
                    if (pair.info.state == IcePairState::Waiting && !selected(pair.info.local.component) &&
                        (!work || pair.info.priority > work->info.priority)) work = &pair;
                if (!work)
                {
                    for (auto& pair : pairs)
                        if (pair.info.state == IcePairState::Frozen && !selected(pair.info.local.component) &&
                            (!work || pair.info.priority > work->info.priority)) work = &pair;
                }
            }
        }
        if (work)
        {
            if (kind == Kind::Consent) work->nextConsent = now + consentDelay();
            beginTransaction(kind, work->transport, work->info.remoteEndpoint, work->info.id);
        }
        else if (!gatherJobs.empty())
        {
            const auto job = gatherJobs.front(); gatherJobs.pop_front();
            beginTransaction(Kind::Gather, job.transport, job.server);
        }
        publish();
        if (started || !transactions.empty() || !gatherJobs.empty()) schedule();
    }
    void learnCheck(const EarlyCheck& check)
    {
        if (!remote || check.ufrag != remote->ufrag) return;
        const auto component = transports[check.transport].host.component;
        if (selected(component)) return;
        auto found = std::find_if(remotes.begin(), remotes.end(), [&](const Remote& peer) {
            return peer.candidate.component == component && peer.endpoint == check.source;
        });
        if (found == remotes.end())
        {
            const Endpoint unknownBase(check.source.protocol(), 9);
            auto candidate = IceCandidate::fromUdpEndpoint(check.source, IceCandidateType::PeerReflexive,
                "p" + std::to_string(nextPair++), component, 65535, unknownBase);
            candidate.priority = check.priority;
            if (insertRemote(std::move(candidate), check.source, check.ufrag)) return;
            found = std::find_if(remotes.begin(), remotes.end(), [&](const Remote& peer) {
                return peer.candidate.component == component && peer.endpoint == check.source;
            });
        }
        if (found == remotes.end()) return;
        auto* pair = makePair(check.transport, static_cast<std::size_t>(found - remotes.begin()));
        if (!pair) return;
        if (check.nominate && role == StunIceRole::Controlled) pair->pendingNomination = true;
        if (pair->info.valid && pair->info.state == IcePairState::Succeeded)
        {
            if (pair->pendingNomination && Clock::now() - pair->lastConsent < config.consentTimeout) select(*pair);
            if (Clock::now() - pair->lastConsent < config.consentTimeout) return;
        }
        for (auto& transaction : transactions)
            if (transaction.second.pair == pair->info.id && transaction.second.kind != Kind::Consent) transaction.second.obsolete = true;
        pair->info.state = IcePairState::Waiting; enqueue(*pair);
    }
    void reply(std::size_t transport, const Endpoint& source, const StunPacket& request,
               std::uint16_t code = 0, const std::vector<std::uint16_t>& unknown = {})
    {
        auto response = code ? StunMessage::bindingError(request, code,
            code == 487 ? "Role Conflict" : code == 420 ? "Unknown Attribute" : "Bad Request", unknown) :
            StunMessage::bindingSuccess(request, source);
        auto wire = response.encode({StunIntegrity::Sha1, stunShortTermKey(local.password), true});
        if (responseCache.size() >= config.maxCandidates) responseCache.pop_front();
        responseCache.push_back({transport, source, request.transactionId(), request.wire(), wire, Clock::now() + std::chrono::seconds(40)});
        sendWire(transport, std::move(wire), source);
    }
    void incomingRequest(std::size_t transport, const Endpoint& source, const StunPacket& packet)
    {
        if (!usable(source) || packet.method() != StunMethod::Binding || !packet.verifyFingerprint()) return;
        const auto* username = packet.find(Type::Username);
        if (!username) return;
        const auto text = username->asText();
        if (!text) return;
        const auto colon = text->find(':');
        if (colon == std::string::npos || text->substr(0, colon) != local.ufrag) return;
        const auto ufrag = text->substr(colon + 1);
        if (!iceString(ufrag, 4) || (remote && ufrag != remote->ufrag) ||
            !packet.verifyIntegrity(stunShortTermKey(local.password), StunIntegrity::Sha1)) return;
        for (const auto& cached : responseCache)
            if (cached.transport == transport && cached.source == source && cached.id == packet.transactionId())
            { if (cached.request == packet.wire()) sendWire(transport, cached.response, source); return; }
        const auto unknown = packet.unknownRequiredAttributes();
        if (!unknown.empty()) { reply(transport, source, packet, 420, unknown); return; }
        if (!packet.validateIceBindingRequest(local.ufrag, ufrag, stunShortTermKey(local.password)))
        { reply(transport, source, packet, 400); return; }
        const auto* controlling = packet.find(Type::IceControlling);
        const auto* controlled = packet.find(Type::IceControlled);
        if (role == StunIceRole::Controlling && controlling)
        {
            if (tieBreaker >= *controlling->asUint64()) { reply(transport, source, packet, 487); return; }
            changeRole(StunIceRole::Controlled);
        }
        else if (role == StunIceRole::Controlled && controlled)
        {
            if (tieBreaker < *controlled->asUint64()) { reply(transport, source, packet, 487); return; }
            changeRole(StunIceRole::Controlling);
        }
        reply(transport, source, packet);
        EarlyCheck check{transport, source, ufrag, *packet.find(Type::Priority)->asUint32(), packet.find(Type::UseCandidate) != nullptr};
        if (remote) learnCheck(check);
        else
        {
            if (earlyChecks.size() >= config.maxCandidates) earlyChecks.pop_front();
            earlyChecks.push_back(std::move(check));
        }
        updateState(); publish(); schedule();
    }
    void incomingResponse(std::size_t transport, const Endpoint& source, const StunPacket& packet)
    {
        const auto it = transactions.find(packet.transactionId());
        if (it == transactions.end()) return;
        const auto& pending = it->second;
        if (transport != pending.transport || source != pending.destination || !packet.isResponseTo(pending.request)) return;
        if (pending.kind == Kind::Gather)
        {
            if (packet.find(Type::Fingerprint) && !packet.verifyFingerprint()) return;
        }
        else if (!remote || !packet.verifyFingerprint() ||
                 !packet.verifyIntegrity(stunShortTermKey(remote->password), StunIntegrity::Sha1)) return;
        if (!packet.unknownRequiredAttributes().empty()) return;
        auto transaction = std::move(it->second); transactions.erase(it);
        if (packet.messageClass() == StunClass::ErrorResponse)
        {
            const auto* attribute = packet.find(Type::ErrorCode);
            if (!attribute) { transactionFailed(transaction); return; }
            const auto code = attribute->asError()->code;
            if (transaction.kind == Kind::Consent)
            {
                if (code == 403) fail("ICE consent revoked; restart required", net::error::access_denied);
                return;
            }
            if (code == 487 && transaction.kind != Kind::Gather && !transaction.obsolete)
            {
                // 依据发起该事务时的角色切换，避免迟到的旧响应再次反转角色。
                const bool sentControlling = std::any_of(transaction.request.attributes.begin(), transaction.request.attributes.end(),
                    [](const auto& attribute) { return attribute.type == Type::IceControlling; });
                changeRole(sentControlling ? StunIceRole::Controlled : StunIceRole::Controlling);
                tieBreaker = random64();
                if (auto* pair = findPair(transaction.pair)) { pair->info.state = IcePairState::Waiting; enqueue(*pair); }
            }
            else transactionFailed(transaction);
            finishGathering(); updateState(); publish(); return;
        }
        if (transaction.kind == Kind::Consent)
        {
            if (auto* pair = findPair(transaction.pair)) pair->lastConsent = Clock::now();
            updateState(); publish(); return;
        }
        const auto mapped = packet.mappedAddress();
        if (!mapped || !usable(*mapped)) { transactionFailed(transaction); updateState(); return; }
        if (transaction.kind == Kind::Gather)
        {
            auto candidate = IceCandidate::fromUdpEndpoint(*mapped, IceCandidateType::ServerReflexive,
                "s" + transports[transport].host.foundation + "x" + std::to_string(
                    std::find(config.stunServers.begin(), config.stunServers.end(), transaction.destination) - config.stunServers.begin()),
                transports[transport].host.component, transports[transport].preference, transports[transport].socket->localEndpoint());
            if (*mapped != transports[transport].socket->localEndpoint()) localCandidate(std::move(candidate));
            finishGathering(); publish(); return;
        }
        auto* pair = findPair(transaction.pair);
        if (!pair || (selected(pair->info.local.component) && !pair->info.nominated)) return;
        auto candidate = std::find_if(locals.begin(), locals.end(), [&](const auto& value) {
            return value.component == pair->info.local.component && value.udpEndpoint() == mapped;
        });
        if (candidate == locals.end())
        {
            auto reflexive = IceCandidate::fromUdpEndpoint(*mapped, IceCandidateType::PeerReflexive,
                "r" + transports[transport].host.foundation, pair->info.local.component, transports[transport].preference,
                transports[transport].socket->localEndpoint());
            reflexive.priority = candidatePriority(IceCandidateType::PeerReflexive, transports[transport].preference, reflexive.component);
            // peer-reflexive 候选属于本次检查的 valid pair，不需要额外配对。
            pair->info.local = reflexive; localCandidate(std::move(reflexive));
        }
        else pair->info.local = *candidate;
        pair->info.priority = candidatePairPriority(pair->info.local.priority, pair->info.remote.priority, role == StunIceRole::Controlling);
        pair->info.state = IcePairState::Succeeded; pair->info.valid = true; pair->lastConsent = Clock::now();
        thawAfter(*pair);
        if ((!transaction.obsolete && transaction.kind == Kind::Nomination && role == StunIceRole::Controlling) ||
            (pair->pendingNomination && role == StunIceRole::Controlled)) select(*pair);
        updateState(); publish();
    }
    void receive(std::size_t transport, UdpSocket::Datagram datagram)
    {
        // 回调可能先于下一次 timer tick 到达；过期响应不能恢复已失效的 consent。
        updateState();
        if (terminal()) return;
        // RFC 7983 的 STUN 首字节范围；损坏的 STUN 不交给 DTLS/媒体回调。
        if (!datagram.payload.empty() && datagram.payload.front() <= 3)
        {
            StunPacket packet;
            if (!StunPacket::parse(datagram.payload, packet)) return;
            if (packet.messageClass() == StunClass::Request) incomingRequest(transport, datagram.remoteEndpoint, packet);
            else if (packet.messageClass() == StunClass::SuccessResponse || packet.messageClass() == StunClass::ErrorResponse)
                incomingResponse(transport, datagram.remoteEndpoint, packet);
            return;
        }
        if (const auto* pair = selected(transports[transport].host.component))
            if (pair->transport == transport && pair->info.remoteEndpoint == datagram.remoteEndpoint && callbacks.onData)
                callbacks.onData(pair->info.local.component, std::move(datagram));
    }
    ErrorCode start()
    {
        if (terminal()) return net::error::operation_aborted;
        if (started) return {};
        if (!gathered && transports.empty())
        {
            const auto ec = gather(); if (ec) return ec;
        }
        else if (!gathered)
        {
            gathered = true;
            if (gathering == IceGatheringState::New) setGathering(IceGatheringState::Gathering);
            finishGathering();
        }
        started = true; startedAt = Clock::now(); setState(IceAgentState::Checking);
        updateState(); schedule(); publish(); return {};
    }
    void cancelResolutions()
    {
        auto pending = std::move(resolutions); resolutions.clear();
        for (const auto& resolution : pending)
        { resolution->resolver->cancel(); complete(resolution->done, net::error::operation_aborted); }
    }
    ErrorCode restart(IceCredentials value, StunIceRole newRole)
    {
        if (closing) return net::error::operation_aborted;
        if (value.ufrag.empty() && value.password.empty()) value = IceCredentials::generate();
        if (!value.validate() || (newRole != StunIceRole::Controlled && newRole != StunIceRole::Controlling) ||
            value.ufrag == local.ufrag || value.password == local.password) return net::error::invalid_argument;
        ++generation;
        timer.cancel(); ticking = false; cancelResolutions();
        transactions.clear(); gatherJobs.clear(); pairs.clear(); remotes.clear();
        triggered.clear(); earlyChecks.clear(); responseCache.clear();
        remote.reset(); local = std::move(value); role = newRole; tieBreaker = random64();
        remoteEnd = false; remoteLite = false; started = false;
        // prflx 属于旧的 ICE generation；host / srflx Socket 映射仍可复用。
        locals.erase(std::remove_if(locals.begin(), locals.end(), [](const auto& candidate) {
            return candidate.type == IceCandidateType::PeerReflexive;
        }), locals.end());
        if (gathering == IceGatheringState::Gathering) setGathering(IceGatheringState::Complete);
        setState(IceAgentState::New); publish(); return {};
    }
    void close(Completion done)
    {
        if (closeFinished) { complete(done); return; }
        if (done) closeCallbacks.push_back(std::move(done));
        if (closing) return;
        closing = true; ++generation;
        timer.cancel(); ticking = false; cancelResolutions();
        transactions.clear(); gatherJobs.clear(); triggered.clear(); earlyChecks.clear(); responseCache.clear();
        for (auto& pair : pairs) { pair.info.nominated = false; pair.info.valid = false; }
        setState(IceAgentState::Closed); publish();
        closingSockets = transports.size();
        if (closingSockets == 0) { finishClose(); return; }
        const auto self = shared_from_this();
        for (auto& transport : transports)
            transport.socket->close([self](ErrorCode) {
                net::post(self->strand, [self] { if (--self->closingSockets == 0) self->finishClose(); });
            });
    }
    void finishClose()
    {
        closeFinished = true;
        auto pending = std::move(closeCallbacks); closeCallbacks.clear();
        for (const auto& done : pending) complete(done);
    }
    // 所有公开操作统一检查生命周期并报告异常；用户回调不得抛异常。
    template<class Function> void operation(Function function, Completion done)
    {
        ErrorCode ec;
        try { ec = function(); }
        catch (const std::invalid_argument&) { ec = net::error::invalid_argument; }
        catch (const std::exception&) { ec = net::error::fault; }
        publish(); complete(done, ec);
    }
};

IceAgent::IceAgent(net::io_context& io) : IceAgent(io, Config{}) {}
IceAgent::IceAgent(net::io_context& io, Config config, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(io, std::move(config), std::move(callbacks))) {}
IceAgent::~IceAgent() { close(); }
IceAgent::Snapshot IceAgent::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->published;
}
void IceAgent::gather(Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, done = std::move(done)] { self->operation([&] { return self->gather(); }, done); });
}
void IceAgent::addLocalTransport(std::shared_ptr<UdpSocket> socket, std::uint16_t component, unsigned preference, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, socket = std::move(socket), component, preference, done = std::move(done)]() mutable {
        self->operation([&] { return self->registerTransport(std::move(socket), component, preference); }, done);
    });
}
void IceAgent::setRemoteCredentials(IceCredentials credentials, bool iceLite, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, credentials = std::move(credentials), iceLite, done = std::move(done)]() mutable {
        self->operation([&] { return self->setRemote(std::move(credentials), iceLite); }, done);
    });
}
void IceAgent::addRemoteCandidate(IceCandidate candidate, Completion done, std::optional<Endpoint> endpoint, std::string ufrag)
{
    const auto self = impl_;
    net::post(self->strand, [self, candidate = std::move(candidate), done = std::move(done), endpoint, ufrag = std::move(ufrag)]() mutable {
        self->addRemote(std::move(candidate), std::move(done), endpoint, std::move(ufrag));
    });
}
void IceAgent::endOfRemoteCandidates(std::string ufrag, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, ufrag = std::move(ufrag), done = std::move(done)] {
        self->operation([&]() -> ErrorCode {
            if (self->terminal()) return net::error::operation_aborted;
            if (!ufrag.empty() && (!self->remote || self->remote->ufrag != ufrag)) return net::error::invalid_argument;
            self->remoteEnd = true; self->updateState(); return {};
        }, done);
    });
}
void IceAgent::setRemoteDescription(SdpSession session, std::string mid, SdpDescriptionType type, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, session = std::move(session), mid = std::move(mid), type, done = std::move(done)]() mutable {
        if (self->terminal()) { complete(done, net::error::operation_aborted); return; }
        if (!session.validateWebRtc(type)) { complete(done, net::error::invalid_argument); return; }
        const auto* section = session.findMedia(mid);
        if (!section || section->isRejected()) { complete(done, net::error::invalid_argument); return; }
        const auto& transport = session.transportMedia(*section, type);
        const auto attributes = session.effectiveAttributes(transport);
        if (!attributes.iceUfrag || !attributes.icePwd) { complete(done, net::error::invalid_argument); return; }
        const auto ec = self->setRemote({*attributes.iceUfrag, *attributes.icePwd}, session.iceLite);
        if (ec) { complete(done, ec); return; }
        // 将候选追加到和普通 trickle 相同的队列；最后的 barrier 等待域名解析完成。
        if (transport.candidates.empty())
        {
            if (transport.endOfCandidates) self->remoteEnd = true;
            self->publish(); complete(done); return;
        }
        struct Batch { std::size_t remaining; ErrorCode error; };
        auto batch = std::make_shared<Batch>(Batch{transport.candidates.size(), {}});
        for (auto candidate : transport.candidates)
            self->addRemote(std::move(candidate), [self, batch, end = transport.endOfCandidates, done](ErrorCode result) {
                if (result && !batch->error) batch->error = result;
                if (--batch->remaining == 0)
                {
                    if (end && !self->terminal()) self->remoteEnd = true;
                    self->publish(); complete(done, batch->error); self->schedule();
                }
            }, std::nullopt, *attributes.iceUfrag);
    });
}
void IceAgent::start(Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, done = std::move(done)] { self->operation([&] { return self->start(); }, done); });
}
void IceAgent::asyncSend(UdpSocket::Bytes payload, std::uint16_t component, UdpSocket::SendHandler done)
{
    const auto self = impl_;
    net::post(self->strand, [self, payload = std::move(payload), component, done = std::move(done)]() mutable {
        auto* pair = self->selected(component);
        ErrorCode ec;
        if (self->terminal()) ec = net::error::operation_aborted;
        else if (!pair) ec = net::error::not_connected;
        else if (Clock::now() - pair->lastConsent >= self->config.consentTimeout)
        { self->fail("ICE consent expired; restart required"); ec = net::error::timed_out; }
        if (ec) { if (done) done(ec, 0); return; }
        const auto weak = self->weak_from_this();
        self->transports[pair->transport].socket->asyncSendTo(std::move(payload), pair->info.remoteEndpoint,
            [weak, done = std::move(done)](ErrorCode result, std::size_t size) {
                if (const auto agent = weak.lock()) net::post(agent->strand, [done, result, size] { if (done) done(result, size); });
            });
    });
}
void IceAgent::restart(IceCredentials credentials, StunIceRole role, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, credentials = std::move(credentials), role, done = std::move(done)]() mutable {
        self->operation([&] { return self->restart(std::move(credentials), role); }, done);
    });
}
void IceAgent::close(Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, done = std::move(done)]() mutable { self->close(std::move(done)); });
}
} // namespace webrtc
