#include"moq/video_source.h"

namespace moq
{
VideoSource::VideoSource(media::MediaService& service) : subscription_(service.subscribe(media::Output::Moq)) {}
std::optional<VideoObject> VideoSource::next()
{
    auto frame = subscription_->tryPop(); if (!frame) return {};
    return VideoObject{frame->epoch, frame->gopSequence, frame->frameInGop, std::move(frame)};
}
bool VideoSource::updateBudget(media::NetworkBudget budget) { return subscription_->updateBudget(budget); }
void VideoSource::requestKeyFrame() { subscription_->requestKeyFrame(); }
void VideoSource::resync() { subscription_->resync(); }
void VideoSource::close() { subscription_->close(); }
media::MediaSubscription::Snapshot VideoSource::snapshot() const { return subscription_->snapshot(); }
} // namespace moq
