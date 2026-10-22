#pragma once
#include"media/media_service.h"

namespace moq
{
struct VideoObject
{
    std::uint64_t epoch, groupId, objectId;
    media::EncodedFrame frame;
};
// Media-facing seam for a future MoQ transport. No QUIC sockets/wire protocol here.
// One per direct subscriber, or one per publisher->relay uplink. All share one encoder.
class VideoSource
{
public:
    explicit VideoSource(media::MediaService& service);
    std::optional<VideoObject> next();
    bool updateBudget(media::NetworkBudget budget); // QUIC-derived VIDEO budget in bit/s, after connection allocation.
    void requestKeyFrame();
    void resync(); // A failed/canceled media group must resume at a random-access frame.
    void close();
    media::MediaSubscription::Snapshot snapshot() const;
private:
    std::shared_ptr<media::MediaSubscription> subscription_;
};
} // namespace moq
