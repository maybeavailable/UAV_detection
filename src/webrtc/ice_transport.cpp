#include"webrtc/ice_transport.h"
#include"log/log.h"

#include<boost/asio/post.hpp>
#include<boost/asio/strand.hpp>

#include<algorithm>
#include<atomic>
#include<deque>
#include<map>
#include<mutex>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace net = boost::asio;
namespace
{
const char* stateName(IceTransportState state)
{
    switch (state)
    {
    case IceTransportState::New: return "New"; case IceTransportState::Checking: return "Checking";
    case IceTransportState::Connected: return "Connected"; case IceTransportState::Completed: return "Completed";
    case IceTransportState::Disconnected: return "Disconnected"; case IceTransportState::Failed: return "Failed"; case IceTransportState::Closed: return "Closed";
    }
    return "Unknown";
}
bool applicationPacket(IcePacketType type)
{
    return type == IcePacketType::Dtls || type == IcePacketType::Rtp || type == IcePacketType::Rtcp;
}
bool sameCredentials(const IceCredentials& a, const IceCredentials& b)
{
    return a.ufrag == b.ufrag && a.password == b.password;
}
std::string lowerAscii(std::string text)
{
    for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return text;
}
bool sameDtls(const IceDtlsParameters& a, const IceDtlsParameters& b)
{
    if (a.transportMid != b.transportMid || a.localSetup != b.localSetup || a.remoteSetup != b.remoteSetup) return false;
    auto sorted = [](const auto& fingerprints) {
        std::vector<std::pair<std::string, std::string>> values;
        for (const auto& value : fingerprints) values.emplace_back(lowerAscii(value.algorithm), lowerAscii(value.value));
        std::sort(values.begin(), values.end());
        return values;
    };
    return sorted(a.remoteFingerprints) == sorted(b.remoteFingerprints);
}
void complete(const IceTransport::Completion& done, UdpSocket::ErrorCode ec = {}) { if (done) done(ec); }
} // namespace

IcePacketType classifyIcePacket(const void* data, std::size_t size)
{
    if (!data || size == 0) return IcePacketType::Unknown;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto first = bytes[0];
    if (first <= 3) return IcePacketType::Stun;
    if (first >= 16 && first <= 19) return IcePacketType::Zrtp;
    if (first >= 20 && first <= 63) return IcePacketType::Dtls;
    if (first >= 64 && first <= 79) return IcePacketType::TurnChannelData;
    if (first >= 128 && first <= 191 && size >= 2)
        return bytes[1] >= 192 && bytes[1] <= 223 ? IcePacketType::Rtcp : IcePacketType::Rtp;
    return IcePacketType::Unknown;
}
IcePacketType classifyIcePacket(const UdpSocket::Bytes& bytes) { return classifyIcePacket(bytes.data(), bytes.size()); }

struct IceTransport::Impl : std::enable_shared_from_this<IceTransport::Impl>
{
    using AgentSnapshot = IceAgent::Snapshot;
    using Action = std::function<void(Impl&, Completion)>;
    using Finish = std::function<void(Impl&, ErrorCode, const AgentSnapshot&)>;
    struct Command { Action action; Completion done; Finish finish; };

    net::strand<net::io_context::executor_type> strand;
    Config config;
    Callbacks callbacks;
    std::unique_ptr<IceAgent> agent;
    std::atomic<std::uint64_t> epoch{0}; // 仅供 agent strand 给跨 strand 通知标记代次
    Snapshot current;
    mutable std::mutex snapshotMutex;
    Snapshot published;
    std::map<std::uint16_t, std::uint64_t> dtlsNotified;
    IceGatheringState notifiedGathering{IceGatheringState::New};
    std::deque<Command> commands;
    Completion activeDone;
    Finish activeFinish;
    std::uint64_t commandToken{0};
    bool busy{false}, restarting{false}, closing{false}, closeConfirmed{false}, closed{false};
    std::size_t pendingSends{0};
    std::vector<Completion> closeCallbacks;

    Impl(net::io_context& io, Config options, Callbacks handlers)
        : strand(net::make_strand(io)), config(std::move(options)), callbacks(std::move(handlers))
    {
        if (config.maxDatagramSize == 0 || config.maxDatagramSize > 65507 ||
            config.maxPendingOperations == 0 || config.maxPendingOperations > 65536)
            throw std::invalid_argument("invalid ICE transport configuration");
    }
    void initialize(net::io_context& io)
    {
        const auto weak = weak_from_this();
        IceAgent::Callbacks handlers;
        handlers.onState = [weak](auto) {
            if (const auto self = weak.lock()) self->agentEvent([](Impl&) {});
        };
        handlers.onGatheringState = [weak](auto) {
            if (const auto self = weak.lock()) self->agentEvent([](Impl&) {});
        };
        handlers.onLocalCandidate = [weak](auto candidate) {
            if (const auto self = weak.lock()) self->agentEvent([candidate = std::move(candidate)](Impl& value) mutable {
                if (value.callbacks.onLocalCandidate) value.callbacks.onLocalCandidate(std::move(candidate));
            });
        };
        handlers.onSelectedPair = [weak](auto pair) {
            if (const auto self = weak.lock()) self->agentEvent([pair = std::move(pair)](Impl& value) mutable {
                if (value.callbacks.onSelectedPair) value.callbacks.onSelectedPair(std::move(pair));
            });
        };
        handlers.onData = [weak](auto component, auto datagram) {
            if (const auto self = weak.lock()) self->agentEvent([component, datagram = std::move(datagram)](Impl& value) mutable {
                value.received(component, std::move(datagram));
            });
        };
        handlers.onError = [weak](auto ec, auto message) {
            if (const auto self = weak.lock()) self->agentEvent([ec, message = std::move(message)](Impl& value) {
                if (value.callbacks.onError) value.callbacks.onError(ec, message);
            });
        };
        agent = std::make_unique<IceAgent>(io, config.ice, std::move(handlers));
        current.ice = agent->snapshot();
        publish();
    }
    void publish()
    {
        current.generation = epoch.load(std::memory_order_relaxed);
        current.restarting = restarting;
        std::lock_guard<std::mutex> lock(snapshotMutex);
        published = current;
    }
    bool usableState() const
    {
        return !closing && !restarting && current.state != IceTransportState::Failed && current.state != IceTransportState::Closed;
    }
    const IceCandidatePair* pair(std::uint16_t component) const
    {
        if (!usableState()) return nullptr;
        for (const auto& value : current.ice.selectedPairs)
            if (value.local.component == component && value.valid && value.nominated) return &value;
        return nullptr;
    }
    IceTransportState deriveState(const AgentSnapshot& source) const
    {
        if (closing || source.state == IceAgentState::Closed) return IceTransportState::Closed;
        if (restarting) return IceTransportState::New;
        if (source.state == IceAgentState::Failed) return IceTransportState::Failed;
        if (source.state == IceAgentState::Disconnected) return IceTransportState::Disconnected;
        if (source.state == IceAgentState::Completed) return IceTransportState::Completed;
        bool selected = true;
        for (auto component : config.ice.components)
            if (std::none_of(source.selectedPairs.begin(), source.selectedPairs.end(), [component](const auto& value) {
                return value.local.component == component && value.valid && value.nominated;
            })) selected = false;
        if (selected) return IceTransportState::Connected;
        return source.state == IceAgentState::New ? IceTransportState::New : IceTransportState::Checking;
    }
    void synchronize(AgentSnapshot source)
    {
        const auto previous = current.state;
        current.ice = std::move(source);
        current.state = deriveState(current.ice);
        publish();
        if (previous != current.state) UAV_LOG_INFO << "[WebRTC][ICE transport " << this << "][g=" << current.generation << "] state "
            << stateName(previous) << " -> " << stateName(current.state) << " selected=" << current.ice.selectedPairs.size();
        if (previous != current.state && callbacks.onState) callbacks.onState(current.state);
        if (notifiedGathering != current.ice.gatheringState)
        {
            notifiedGathering = current.ice.gatheringState;
            if (callbacks.onGatheringState) callbacks.onGatheringState(notifiedGathering);
        }
    }
    void dtlsReady()
    {
        if (!usableState() || !current.dtls) return;
        for (const auto& choice : current.ice.selectedPairs)
        {
            if (!choice.valid || !choice.nominated) continue;
            auto& notified = dtlsNotified[choice.local.component];
            if (notified == choice.id) continue;
            notified = choice.id;
            UAV_LOG_INFO << "[WebRTC][ICE transport " << this << "][g=" << current.generation << "] DTLS ready component=" << choice.local.component
                << " pair=" << choice.id << " role=" << (current.dtls->localSetup == SdpSetupRole::Active ? "client" : "server") << " remote=" << choice.remoteEndpoint;
            if (callbacks.onDtlsReady) callbacks.onDtlsReady({current.generation, choice, *current.dtls});
        }
    }
    template<class Function> void agentEvent(Function function)
    {
        // 本函数只从 agent strand 调用；其快照与事件在源 strand 上同时取出。
        auto source = agent->snapshot();
        const auto generation = epoch.load(std::memory_order_relaxed);
        const auto self = shared_from_this();
        net::post(strand, [self, generation, source = std::move(source), function = std::move(function)]() mutable {
            if (self->closing || self->restarting || generation != self->epoch.load(std::memory_order_relaxed)) return;
            self->synchronize(std::move(source));
            function(*self);
            self->dtlsReady();
        });
    }
    void received(std::uint16_t component, UdpSocket::Datagram datagram)
    {
        const auto type = classifyIcePacket(datagram.payload);
        const auto* selected = pair(component);
        if (!selected || selected->remoteEndpoint != datagram.remoteEndpoint ||
            datagram.payload.size() > config.maxDatagramSize || !applicationPacket(type))
        { ++current.stats.discardedPackets; publish(); return; }
        ++current.stats.receivedPackets; current.stats.receivedBytes += datagram.payload.size();
        publish();
        if (callbacks.onPacket) callbacks.onPacket({current.generation, component, type, std::move(datagram)});
    }
    void enqueue(Action action, Completion done, Finish finish = {})
    {
        if (closing) { complete(done, net::error::operation_aborted); return; }
        if (commands.size() + (busy ? 1 : 0) >= config.maxPendingOperations)
        { complete(done, net::error::no_buffer_space); return; }
        commands.push_back({std::move(action), std::move(done), std::move(finish)});
        pump();
    }
    void pump()
    {
        if (busy || restarting || closing || commands.empty()) return;
        auto command = std::move(commands.front()); commands.pop_front();
        busy = true;
        const auto token = ++commandToken;
        activeDone = std::move(command.done); activeFinish = std::move(command.finish);
        const auto weak = weak_from_this();
        Completion finished = [weak, token](ErrorCode ec) {
            if (const auto self = weak.lock())
            {
                auto source = self->agent->snapshot();
                net::post(self->strand, [self, token, ec, source = std::move(source)]() mutable {
                    if (!self->busy || token != self->commandToken) return;
                    auto done = std::move(self->activeDone); auto finish = std::move(self->activeFinish);
                    self->busy = false;
                    if (finish) finish(*self, ec, source);
                    self->synchronize(std::move(source)); self->dtlsReady();
                    complete(done, ec); self->pump();
                });
            }
        };
        try { command.action(*this, finished); }
        catch (const std::invalid_argument&) { finished(net::error::invalid_argument); }
        catch (const std::exception&) { finished(net::error::fault); }
    }
    void cancelCommands()
    {
        ++commandToken;
        auto active = std::move(activeDone); activeFinish = {}; busy = false;
        auto canceled = std::move(commands); commands.clear();
        complete(active, net::error::operation_aborted);
        for (const auto& command : canceled) complete(command.done, net::error::operation_aborted);
    }
    void send(UdpSocket::Bytes payload, std::uint16_t component, IcePacketType expected, SendHandler done)
    {
        ErrorCode ec;
        const auto type = classifyIcePacket(payload);
        if (closing || restarting || current.state == IceTransportState::Failed) ec = net::error::operation_aborted;
        else if (payload.size() > config.maxDatagramSize) ec = net::error::message_size;
        else if (!applicationPacket(type) || (expected != IcePacketType::Unknown && type != expected)) ec = net::error::invalid_argument;
        else if (!pair(component)) ec = net::error::not_connected;
        if (ec)
        { ++current.stats.failedSends; publish(); if (done) done(ec, 0); return; }
        ++pendingSends;
        const auto self = shared_from_this();
        agent->asyncSend(std::move(payload), component, [self, done = std::move(done)](ErrorCode result, std::size_t size) {
            net::post(self->strand, [self, done, result, size]() mutable {
                // 成功计数以实际交给网络栈的结果为准；restart/close 不伪造已完成发送的状态。
                if (result) ++self->current.stats.failedSends;
                else { ++self->current.stats.sentPackets; self->current.stats.sentBytes += size; }
                --self->pendingSends;
                self->publish();
                if (done) done(result, size);
                self->finishClose();
            });
        });
    }
    void restart(IceCredentials credentials, StunIceRole role, Completion done)
    {
        if (closing) { complete(done, net::error::operation_aborted); return; }
        if (restarting) { complete(done, net::error::already_started); return; }
        try
        {
            if (credentials.ufrag.empty() && credentials.password.empty()) credentials = IceCredentials::generate();
        }
        catch (const std::exception&) { complete(done, net::error::fault); return; }
        if (!credentials.validate() || credentials.ufrag == current.ice.localCredentials.ufrag ||
            credentials.password == current.ice.localCredentials.password ||
            (role != StunIceRole::Controlling && role != StunIceRole::Controlled))
        { complete(done, net::error::invalid_argument); return; }
        restarting = true;
        const auto generation = epoch.fetch_add(1, std::memory_order_relaxed) + 1;
        UAV_LOG_INFO << "[WebRTC][ICE transport " << this << "][g=" << generation << "] restart";
        dtlsNotified.clear(); current.dtls.reset();
        current.ice.selectedPairs.clear();
        synchronize(current.ice);
        cancelCommands();
        const auto self = shared_from_this();
        agent->restart(std::move(credentials), role, [self, generation, done = std::move(done)](ErrorCode ec) {
            auto source = self->agent->snapshot();
            net::post(self->strand, [self, generation, done, ec, source = std::move(source)]() mutable {
                if (generation != self->epoch.load(std::memory_order_relaxed) || self->closing)
                { complete(done, net::error::operation_aborted); return; }
                self->restarting = false;
                self->synchronize(std::move(source)); complete(done, ec); self->pump();
            });
        });
    }
    void close(Completion done)
    {
        if (closed) { complete(done); return; }
        if (done) closeCallbacks.push_back(std::move(done));
        if (closing) return;
        closing = true; restarting = false; epoch.fetch_add(1, std::memory_order_relaxed);
        dtlsNotified.clear(); current.dtls.reset(); current.ice.selectedPairs.clear();
        synchronize(current.ice);
        cancelCommands();
        const auto self = shared_from_this();
        agent->close([self](ErrorCode) {
            auto source = self->agent->snapshot();
            net::post(self->strand, [self, source = std::move(source)]() mutable {
                self->closeConfirmed = true; self->synchronize(std::move(source)); self->finishClose();
            });
        });
    }
    void finishClose()
    {
        if (closed || !closing || !closeConfirmed || pendingSends) return;
        closed = true;
        UAV_LOG_INFO << "[WebRTC][ICE transport " << this << "][g=" << current.generation << "] closed sent=" << current.stats.sentPackets
            << " received=" << current.stats.receivedPackets << " discarded=" << current.stats.discardedPackets << " failed_sends=" << current.stats.failedSends;
        auto done = std::move(closeCallbacks); closeCallbacks.clear();
        for (const auto& callback : done) complete(callback);
    }
};

IceTransport::IceTransport(net::io_context& io) : IceTransport(io, Config{}) {}
IceTransport::IceTransport(net::io_context& io, Config config, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(io, std::move(config), std::move(callbacks))) { impl_->initialize(io); }
IceTransport::~IceTransport() { close(); }
IceTransport::Snapshot IceTransport::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->published;
}
std::optional<IceCandidatePair> IceTransport::selectedPair(std::uint16_t component) const
{
    const auto value = snapshot();
    if (value.restarting || value.state == IceTransportState::Failed || value.state == IceTransportState::Closed) return std::nullopt;
    for (const auto& pair : value.ice.selectedPairs)
        if (pair.local.component == component && pair.valid && pair.nominated) return pair;
    return std::nullopt;
}
bool IceTransport::isReady(std::uint16_t component) const { return selectedPair(component).has_value(); }
void IceTransport::gather(Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, done = std::move(done)]() mutable {
        self->enqueue([](Impl& value, Completion finished) { value.agent->gather(std::move(finished)); }, std::move(done));
    });
}
void IceTransport::addLocalTransport(std::shared_ptr<UdpSocket> socket, std::uint16_t component, unsigned preference, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, socket = std::move(socket), component, preference, done = std::move(done)]() mutable {
        self->enqueue([socket = std::move(socket), component, preference](Impl& value, Completion finished) {
            value.agent->addLocalTransport(socket, component, preference, std::move(finished));
        }, std::move(done));
    });
}
void IceTransport::setRemoteCredentials(IceCredentials credentials, bool iceLite, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, credentials = std::move(credentials), iceLite, done = std::move(done)]() mutable {
        self->enqueue([credentials = std::move(credentials), iceLite](Impl& value, Completion finished) {
            value.agent->setRemoteCredentials(credentials, iceLite, std::move(finished));
        }, std::move(done));
    });
}
void IceTransport::addRemoteCandidate(IceCandidate candidate, Completion done, std::optional<UdpSocket::Endpoint> endpoint, std::string ufrag)
{
    const auto self = impl_;
    net::post(self->strand, [self, candidate = std::move(candidate), done = std::move(done), endpoint, ufrag = std::move(ufrag)]() mutable {
        self->enqueue([candidate = std::move(candidate), endpoint, ufrag = std::move(ufrag)](Impl& value, Completion finished) {
            value.agent->addRemoteCandidate(candidate, std::move(finished), endpoint, ufrag);
        }, std::move(done));
    });
}
void IceTransport::endOfRemoteCandidates(std::string ufrag, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, ufrag = std::move(ufrag), done = std::move(done)]() mutable {
        self->enqueue([ufrag = std::move(ufrag)](Impl& value, Completion finished) {
            value.agent->endOfRemoteCandidates(ufrag, std::move(finished));
        }, std::move(done));
    });
}
void IceTransport::setRemoteDescription(SdpSession session, std::string mid, SdpDescriptionType type, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, session = std::move(session), mid = std::move(mid), type, done = std::move(done)]() mutable {
        if (self->closing) { complete(done, net::error::operation_aborted); return; }
        if ((type != SdpDescriptionType::Offer && type != SdpDescriptionType::Answer) || !session.validateWebRtc(type))
        { complete(done, net::error::invalid_argument); return; }
        const auto* section = session.findMedia(mid);
        if (!section || section->isRejected()) { complete(done, net::error::invalid_argument); return; }
        const auto& transport = session.transportMedia(*section, type);
        const auto attributes = session.effectiveAttributes(transport);
        if (!attributes.iceUfrag || !attributes.icePwd || !attributes.setup || attributes.fingerprints.empty())
        { complete(done, net::error::invalid_argument); return; }
        IceDtlsParameters parameters;
        parameters.transportMid = *transport.mid; parameters.remoteFingerprints = attributes.fingerprints;
        parameters.remoteSetup = *attributes.setup;
        parameters.localSetup = type == SdpDescriptionType::Offer ? answerSetupRole(*attributes.setup, self->config.preferDtlsClient) :
            *attributes.setup == SdpSetupRole::Active ? SdpSetupRole::Passive : SdpSetupRole::Active;
        const IceCredentials credentials{*attributes.iceUfrag, *attributes.icePwd};
        // 参数先校验再入队，在 action 执行时检查是否与前一个已完成的描述冲突。
        self->enqueue([session = std::move(session), mid = std::move(mid), type, parameters](Impl& value, Completion finished) {
            if (value.current.dtls && !sameDtls(*value.current.dtls, parameters))
            { complete(finished, net::error::already_started); return; }
            value.agent->setRemoteDescription(session, mid, type, std::move(finished));
        }, std::move(done), [parameters, credentials](Impl& value, ErrorCode, const Impl::AgentSnapshot& source) {
            // agent 可能接受凭据和部分候选后遇到 DNS 错误；保留已经接受的 SDP 身份信息。
            if (source.remoteCredentials && sameCredentials(*source.remoteCredentials, credentials) &&
                (!value.current.dtls || sameDtls(*value.current.dtls, parameters))) value.current.dtls = parameters;
        });
    });
}
void IceTransport::start(Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, done = std::move(done)]() mutable {
        self->enqueue([](Impl& value, Completion finished) { value.agent->start(std::move(finished)); }, std::move(done));
    });
}
void IceTransport::send(UdpSocket::Bytes payload, std::uint16_t component, IcePacketType expected, SendHandler done)
{
    const auto self = impl_;
    net::post(self->strand, [self, payload = std::move(payload), component, expected, done = std::move(done)]() mutable {
        self->send(std::move(payload), component, expected, std::move(done));
    });
}
void IceTransport::asyncSend(UdpSocket::Bytes payload, std::uint16_t component, SendHandler done)
{ send(std::move(payload), component, IcePacketType::Unknown, std::move(done)); }
void IceTransport::asyncSendDtls(UdpSocket::Bytes payload, std::uint16_t component, SendHandler done)
{ send(std::move(payload), component, IcePacketType::Dtls, std::move(done)); }
void IceTransport::asyncSendRtp(UdpSocket::Bytes payload, std::uint16_t component, SendHandler done)
{ send(std::move(payload), component, IcePacketType::Rtp, std::move(done)); }
void IceTransport::asyncSendRtcp(UdpSocket::Bytes payload, std::uint16_t component, SendHandler done)
{ send(std::move(payload), component, IcePacketType::Rtcp, std::move(done)); }
void IceTransport::restart(IceCredentials credentials, StunIceRole role, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, credentials = std::move(credentials), role, done = std::move(done)]() mutable {
        self->restart(std::move(credentials), role, std::move(done));
    });
}
void IceTransport::close(Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, done = std::move(done)]() mutable { self->close(std::move(done)); });
}
} // namespace webrtc
