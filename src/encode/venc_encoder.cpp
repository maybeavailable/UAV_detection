#include"encode/venc_encoder.h"
#include"encode/ascend_video_frame.h"
#include"acl.h"
#include"log/log.h"
#include<algorithm>
#include<cstring>
#include<stdexcept>
#include<thread>

namespace encode
{
namespace
{
void check(int result, const char* operation)
{ if (result) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result)); }
std::uint32_t kbps(std::uint64_t bps) { return static_cast<std::uint32_t>(std::max<std::uint64_t>(1, bps / 1000)); }
}
struct VencEncoder::Impl
{
    VencOptions options; EncoderConfig config; hi_venc_chn_attr attr{}; bool created{false}, started{false};
    media::RawFrame inFlight; aclrtRunMode mode{ACL_HOST};
    explicit Impl(VencOptions o) : options(o)
    { if (o.channel < 0 || o.channel >= 256 || !o.context || o.timeout.count() < 1 || o.timeout > std::chrono::seconds(10)) throw std::invalid_argument("invalid VENC options"); }
    void context() { check(aclrtSetCurrentContext(options.context), "set encoder ACL context"); }
};
VencEncoder::VencEncoder(VencOptions o) : impl_(std::make_unique<Impl>(o)) {}
VencEncoder::~VencEncoder() { stop(); }
void VencEncoder::start(const EncoderConfig& config)
{
    auto& s = *impl_; if (s.created) throw std::logic_error("VENC already started");
    config.validate(); s.context(); check(aclrtGetRunMode(&s.mode), "aclrtGetRunMode"); s.config = config; s.attr = {};
    auto& a = s.attr; a.venc_attr.type = config.codec == media::VideoCodec::H264 ? HI_PT_H264 : HI_PT_H265;
    a.venc_attr.max_pic_width = a.venc_attr.pic_width = config.width;
    a.venc_attr.max_pic_height = a.venc_attr.pic_height = config.height;
    a.venc_attr.profile = config.profile; a.venc_attr.is_by_frame = HI_TRUE;
    a.venc_attr.buf_size = (config.width * config.height * 3 / 2 + 63) & ~63u;
    a.gop_attr.gop_mode = HI_VENC_GOP_MODE_NORMAL_P; a.gop_attr.normal_p.ip_qp_delta = 2;
    if (config.codec == media::VideoCodec::H264)
    {
        a.venc_attr.h264_attr.frame_buf_ratio = 70; a.rc_attr.rc_mode = HI_VENC_RC_MODE_H264_CBR;
        auto& c = a.rc_attr.h264_cbr; c.gop = config.gop; c.stats_time = 1; c.src_frame_rate = c.dst_frame_rate = config.fps; c.bit_rate = kbps(config.startBitrateBps);
    }
    else
    {
        a.venc_attr.h265_attr.frame_buf_ratio = 70; a.rc_attr.rc_mode = HI_VENC_RC_MODE_H265_CBR;
        auto& c = a.rc_attr.h265_cbr; c.gop = config.gop; c.stats_time = 1; c.src_frame_rate = c.dst_frame_rate = config.fps; c.bit_rate = kbps(config.startBitrateBps);
    }
    check(hi_mpi_venc_create_chn(s.options.channel, &a), "create VENC channel"); s.created = true;
    try
    { hi_venc_start_param p{}; p.recv_pic_num = -1; check(hi_mpi_venc_start_chn(s.options.channel, &p), "start VENC channel"); s.started = true; }
    catch (...) { stop(); throw; }
}
void VencEncoder::setBitrate(std::uint64_t rate)
{
    auto& s = *impl_; if (!s.started || !rate) throw std::logic_error("VENC is stopped or zero rate");
    rate = std::clamp(rate, s.config.minBitrateBps, s.config.maxBitrateBps); s.context();
    hi_venc_chn_attr attr{}; check(hi_mpi_venc_get_chn_attr(s.options.channel, &attr), "get VENC attributes");
    if (s.config.codec == media::VideoCodec::H264) attr.rc_attr.h264_cbr.bit_rate = kbps(rate);
    else attr.rc_attr.h265_cbr.bit_rate = kbps(rate);
    check(hi_mpi_venc_set_chn_attr(s.options.channel, &attr), "set VENC bitrate");
    UAV_LOG_DEBUG << "[VENC][ch=" << s.options.channel << "] target_bps=" << rate << " applied_kbps=" << kbps(rate);
}
void VencEncoder::requestKeyFrame()
{ auto& s = *impl_; s.context(); check(hi_mpi_venc_request_idr(s.options.channel, HI_TRUE), "request VENC IDR"); }
media::EncodedVideoFrame VencEncoder::encode(const media::RawFrame& frame)
{
    auto& s = *impl_; if (!s.started || !frame) throw std::logic_error("VENC not ready");
    auto buffer = std::dynamic_pointer_cast<const AscendFrameBuffer>(frame->buffer);
    if (!buffer || !buffer->owner || frame->width != s.config.width || frame->height != s.config.height) throw std::invalid_argument("VENC needs an owning Ascend frame");
    s.context(); s.inFlight = frame; auto input = buffer->frame; input.v_frame.pts = frame->ptsUs;
    check(hi_mpi_venc_send_frame(s.options.channel, &input, static_cast<int>(s.options.timeout.count())), "send VENC frame");
    hi_venc_chn_status status{}; const auto deadline = std::chrono::steady_clock::now() + s.options.timeout;
    do
    {
        check(hi_mpi_venc_query_status(s.options.channel, &status), "query VENC status");
        if (status.cur_packs) break;
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("VENC encode timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
    if (status.cur_packs > 4096) throw std::runtime_error("VENC packet count exceeds access-unit limit");
    std::vector<hi_venc_pack> packs(status.cur_packs); hi_venc_stream stream{}; stream.pack = packs.data(); stream.pack_cnt = status.cur_packs;
    check(hi_mpi_venc_get_stream(s.options.channel, &stream, static_cast<int>(s.options.timeout.count())), "get VENC stream");
    struct Release
    {
        int ch; hi_venc_stream& stream;
        ~Release() { if (hi_mpi_venc_release_stream(ch, &stream)) UAV_LOG_ERROR << "[VENC] release stream failed ch=" << ch; }
    } release{s.options.channel, stream};
    if (!stream.pack_cnt || stream.pack_cnt > packs.size()) throw std::runtime_error("invalid VENC stream packet count");
    auto output = std::make_shared<media::Bytes>();
    for (std::size_t i = 0; i < stream.pack_cnt; ++i)
    {
        const auto& p = stream.pack[i];
        if (!p.addr || p.offset > p.len || p.len - p.offset > 8 * 1024 * 1024 - output->size()) throw std::runtime_error("invalid VENC stream packet bounds");
        const auto offset = output->size(); const auto bytes = p.len - p.offset; output->resize(offset + bytes);
        if (s.mode == ACL_HOST) check(aclrtMemcpy(output->data() + offset, bytes, p.addr + p.offset, bytes, ACL_MEMCPY_DEVICE_TO_HOST), "copy VENC stream to host");
        else if (bytes) std::memcpy(output->data() + offset, p.addr + p.offset, bytes);
    }
    if (!stream.pack[stream.pack_cnt - 1].is_frame_end) throw std::runtime_error("VENC did not return a complete frame");
    s.inFlight.reset(); media::EncodedVideoFrame result; result.codec = s.config.codec; result.data = std::move(output); return result;
}
void VencEncoder::stop() noexcept
{
    if (!impl_) return;
    auto& s = *impl_; if (!s.created) return;
    (void)aclrtSetCurrentContext(s.options.context);
    if (s.started && hi_mpi_venc_stop_chn(s.options.channel)) UAV_LOG_ERROR << "[VENC] stop failed ch=" << s.options.channel;
    if (hi_mpi_venc_destroy_chn(s.options.channel)) UAV_LOG_ERROR << "[VENC] destroy failed ch=" << s.options.channel;
    s.started = s.created = false; s.inFlight.reset();
}
EncoderFactory vencFactory(int first, void* context)
{
    if (first < 0 || first > 253 || !context) throw std::invalid_argument("three VENC channels and an ACL context are required");
    return [first, context](media::Output output) { return std::make_unique<VencEncoder>(VencOptions{first + static_cast<int>(output), context, std::chrono::milliseconds(1000)}); };
}
} // namespace encode
