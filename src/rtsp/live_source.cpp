#include"rtsp/live_source.h"
#include<stdexcept>

LiveRtpSource::LiveRtpSource(std::string path, std::shared_ptr<media::MediaSubscription> subscription)
    : path_(std::move(path)), subscription_(std::move(subscription))
{ if (path_.empty() || path_[0] != '/' || !subscription_) throw std::invalid_argument("invalid live RTSP source"); }
std::string LiveRtpSource::makeSdp(const std::string& host, int) const
{
    return "v=0\r\no=- 0 1 IN IP4 " + host + "\r\ns=UAV live video\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\na=control:track1\r\n";
}
void LiveRtpSource::requestKeyFrame() { subscription_->requestKeyFrame(); }
std::size_t LiveRtpSource::pump(std::size_t maxFrames)
{
    std::size_t count = 0;
    while (count < maxFrames)
    {
        const auto frame = subscription_->tryPop(); if (!frame) break;
        if (frame->codec != media::VideoCodec::H264) throw std::invalid_argument("RTSP live source requires H264");
        if (!epoch_ || *epoch_ != frame->epoch)
        {
            ptsBaseUs_ = epoch_ ? lastPtsUs_ + 40000 : 0;
            ptsOriginUs_ = frame->ptsUs; epoch_ = frame->epoch;
        }
        // Keep the shared RTP clock continuous when a replacement capture source restarts its PTS.
        lastPtsUs_ = ptsBaseUs_ + frame->ptsUs - ptsOriginUs_;
        if (frame->keyFrame && frame->parameterSets)
        {
            auto bytes = *frame->parameterSets; bytes.insert(bytes.end(), frame->data->begin(), frame->data->end());
            pushFrame(bytes.data(), bytes.size(), lastPtsUs_);
        }
        else pushFrame(frame->data->data(), frame->data->size(), lastPtsUs_);
        ++count;
    }
    return count;
}
