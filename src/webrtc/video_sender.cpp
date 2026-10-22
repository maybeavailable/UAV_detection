#include"webrtc/video_sender.h"
#include"log/log.h"
#include<boost/asio/post.hpp>
#include<boost/asio/steady_timer.hpp>
#include<boost/asio/strand.hpp>
#include<deque>

namespace webrtc
{
namespace net = boost::asio;
struct VideoSender::Impl : std::enable_shared_from_this<Impl>
{
    net::strand<net::io_context::executor_type> strand; net::steady_timer timer;
    std::shared_ptr<media::MediaSubscription> subscription; Sender sender; Config config; media::H264Packetizer packetizer;
    std::deque<media::Bytes> packets; bool closed{false}, connected{false}, sending{false}; std::uint64_t version{0};
    Impl(net::io_context& io, std::shared_ptr<media::MediaSubscription> s, Sender send, Config c)
        : strand(net::make_strand(io)), timer(strand), subscription(std::move(s)), sender(std::move(send)), config(c), packetizer(c.rtp)
    { if (!subscription || !sender || c.pollInterval.count() < 1 || c.pollInterval > std::chrono::milliseconds(100)) throw std::invalid_argument("invalid WebRTC video sender"); }
    void pump()
    {
        if (closed || !connected || sending) return;
        if (subscription->snapshot().closed) { close(); return; }
        if (packets.empty())
        {
            if (auto frame = subscription->tryPop())
            {
                try { auto output = packetizer.packetize(*frame); for (auto& p : output) packets.push_back(std::move(p)); }
                catch (const std::exception& e) { UAV_LOG_ERROR << "[WebRTC][VideoSender] " << e.what(); close(); return; }
            }
        }
        if (packets.empty())
        {
            timer.expires_after(config.pollInterval); const auto weak = weak_from_this(); const auto ticket = version;
            timer.async_wait([weak, ticket](auto ec) { if (auto s = weak.lock(); s && !ec && s->version == ticket) s->pump(); }); return;
        }
        auto packet = std::move(packets.front()); packets.pop_front(); sending = true;
        const auto ticket = version; const auto weak = weak_from_this(); const auto expected = packet.size();
        auto done = [weak, ticket, expected](auto ec, std::size_t size) {
            if (auto s = weak.lock()) net::post(s->strand, [s, ticket, ec, size, expected] {
                if (ticket != s->version || s->closed) return;
                s->sending = false;
                if (ec || size != expected) { s->packets.clear(); s->subscription->resync(); }
                s->pump();
            });
        };
        try { sender(std::move(packet), done); } catch (const std::exception&) { done(boost::asio::error::fault, 0); }
    }
    void connection(bool available)
    {
        if (closed || connected == available) return;
        connected = available; ++version; timer.cancel(); packets.clear(); sending = false; subscription->resync();
        if (connected) pump();
    }
    void close()
    { if (closed) return; closed = true; ++version; timer.cancel(); packets.clear(); subscription->close(); }
};
VideoSender::VideoSender(net::io_context& io, std::shared_ptr<media::MediaSubscription> subscription, Sender sender)
    : VideoSender(io, std::move(subscription), std::move(sender), Config{}) {}
VideoSender::VideoSender(net::io_context& io, std::shared_ptr<media::MediaSubscription> subscription, Sender sender, Config c)
    : impl_(std::make_shared<Impl>(io, std::move(subscription), std::move(sender), c)) {}
VideoSender::~VideoSender() { close(); }
std::uint32_t VideoSender::ssrc() const { return impl_->packetizer.ssrc(); }
void VideoSender::close()
{
    const auto s = impl_; s->subscription->close(); net::post(s->strand, [s] { s->close(); });
}
void VideoSender::bindCallbacks(PeerConnection::Callbacks& cb)
{
    const auto weak = std::weak_ptr<Impl>(impl_);
    cb.onTargetTransferRate = [weak, previous = std::move(cb.onTargetTransferRate)](auto event) {
        if (auto s = weak.lock()) s->subscription->updateBudget({event.generation, event.rate.target_rate, s->config.overheadReserveBps, event.rate.cwnd_reduce_ratio});
        if (previous) previous(std::move(event));
    };
    cb.onKeyFrameRequest = [weak, previous = std::move(cb.onKeyFrameRequest)](auto event) {
        if (auto s = weak.lock(); s && s->packetizer.ssrc() == event.feedback.mediaSsrc) s->subscription->requestKeyFrame();
        if (previous) previous(std::move(event));
    };
    cb.onConnectionState = [weak, previous = std::move(cb.onConnectionState)](auto state) {
        if (auto s = weak.lock())
        {
            if (state == PeerConnectionState::Closed) s->subscription->close();
            net::post(s->strand, [s, state] { if (state == PeerConnectionState::Closed) s->close(); else s->connection(state == PeerConnectionState::Connected); });
        }
        if (previous) previous(state);
    };
}
} // namespace webrtc
