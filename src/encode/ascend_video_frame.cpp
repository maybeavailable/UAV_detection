#include"encode/ascend_video_frame.h"
#include"acl.h"
#include"log/log.h"
#include<cstring>
#include<mutex>
#include<stdexcept>

namespace encode
{
namespace
{
void check(int code, const char* operation) { if (code) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(code)); }
}
struct AscendFramePool::Impl
{
    struct Slot { void* data{nullptr}; bool used{false}; };
    Config config; std::uint32_t pitch, rows; std::size_t bytes;
    mutable std::mutex mutex; std::vector<Slot> slots; std::uint64_t dropped{0};
    explicit Impl(Config c) : config(c), pitch((c.width + 63) & ~63u), rows((c.height + 15) & ~15u), bytes(std::size_t(pitch) * rows * 3 / 2)
    {
        if (!c.context || !c.width || !c.height || c.width > 8192 || c.height > 8192 || (c.width & 1) || (c.height & 1) || !c.maxBuffers || c.maxBuffers > 64)
            throw std::invalid_argument("invalid Ascend frame pool");
        slots.resize(c.maxBuffers);
    }
    ~Impl()
    {
        (void)aclrtSetCurrentContext(config.context);
        for (const auto& s : slots) if (s.data && hi_mpi_dvpp_free(s.data)) UAV_LOG_ERROR << "[FramePool] failed to release DVPP buffer";
    }
};
AscendFramePool::AscendFramePool(Config c) : impl_(std::make_shared<Impl>(c)) {}
AscendFramePool::~AscendFramePool() = default;
media::RawFrame AscendFramePool::tryCopy(const hi_video_frame_info& source, std::uint64_t epoch, std::uint64_t sequence)
{
    const auto s = impl_; const auto& f = source.v_frame;
    if (f.width != s->config.width || f.height != s->config.height || f.compress_mode != HI_COMPRESS_MODE_NONE ||
        f.video_format != HI_VIDEO_FORMAT_LINEAR || (f.pixel_format != HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420 && f.pixel_format != HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420) ||
        !f.virt_addr[0] || !f.virt_addr[1] || f.width_stride[0] < f.width || f.width_stride[1] < f.width)
        throw std::invalid_argument("frame pool requires matching linear NV12/NV21 planes");
    check(aclrtSetCurrentContext(s->config.context), "aclrtSetCurrentContext");
    std::size_t slot = 0; void* data = nullptr;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        while (slot < s->slots.size() && s->slots[slot].used) ++slot;
        if (slot == s->slots.size()) { ++s->dropped; return {}; }
        auto& entry = s->slots[slot];
        if (!entry.data) check(hi_mpi_dvpp_malloc(s->config.device, &entry.data, s->bytes), "hi_mpi_dvpp_malloc");
        entry.used = true; data = entry.data;
    }
    auto owner = std::shared_ptr<void>(data, [s, slot](void*) { std::lock_guard<std::mutex> lock(s->mutex); s->slots[slot].used = false; });
    auto buffer = std::make_shared<AscendFrameBuffer>(); buffer->owner = std::move(owner);
    auto& destination = buffer->frame; destination.mod_id = HI_ID_VENC; destination.pool_id = static_cast<hi_u32>(HI_VB_INVALID_POOLID);
    auto& d = destination.v_frame; d.width = f.width; d.height = f.height; d.pixel_format = f.pixel_format;
    d.field = HI_VIDEO_FIELD_FRAME; d.video_format = HI_VIDEO_FORMAT_LINEAR; d.compress_mode = HI_COMPRESS_MODE_NONE;
    d.dynamic_range = f.dynamic_range; d.color_gamut = f.color_gamut; d.pts = f.pts;
    d.virt_addr[0] = data; d.virt_addr[1] = static_cast<std::uint8_t*>(data) + std::size_t(s->pitch) * s->rows;
    d.width_stride[0] = d.width_stride[1] = s->pitch; d.height_stride[0] = s->rows; d.height_stride[1] = s->rows / 2;
    aclrtRunMode mode; check(aclrtGetRunMode(&mode), "get frame-copy run mode");
    // aclrtMemcpy2d does not support D2D on this SDK. Board DEVICE mode exposes
    // these mapped DVPP addresses to the CPU, as the existing VPSS save path does.
    for (unsigned plane = 0; plane < 2; ++plane)
    {
        const auto height = plane ? f.height / 2 : f.height;
        if (s->pitch == f.width_stride[plane])
        {
            const auto bytes = std::size_t(s->pitch) * height;
            if (mode == ACL_DEVICE) std::memcpy(d.virt_addr[plane], f.virt_addr[plane], bytes);
            else check(aclrtMemcpy(d.virt_addr[plane], bytes, f.virt_addr[plane], bytes, ACL_MEMCPY_DEVICE_TO_DEVICE), "copy VPSS plane");
        }
        else for (unsigned row = 0; row < height; ++row)
        {
            auto* dst = static_cast<std::uint8_t*>(d.virt_addr[plane]) + std::size_t(row) * s->pitch;
            const auto* src = static_cast<const std::uint8_t*>(f.virt_addr[plane]) + std::size_t(row) * f.width_stride[plane];
            if (mode == ACL_DEVICE) std::memcpy(dst, src, f.width);
            else check(aclrtMemcpy(dst, s->pitch, src, f.width, ACL_MEMCPY_DEVICE_TO_DEVICE), "copy VPSS row");
        }
    }
    auto result = std::make_shared<media::RawVideoFrame>(); result->width = f.width; result->height = f.height;
    result->format = f.pixel_format == HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420 ? media::PixelFormat::Nv12 : media::PixelFormat::Nv21;
    result->sourceEpoch = epoch; result->sequence = sequence; result->ptsUs = f.pts; result->buffer = std::move(buffer); return result;
}
AscendFramePool::Snapshot AscendFramePool::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex); Snapshot result; result.dropped = impl_->dropped;
    for (const auto& s : impl_->slots) { result.allocated += s.data != nullptr; result.inUse += s.used; } return result;
}
} // namespace encode
