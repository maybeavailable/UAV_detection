#pragma once
#include<chrono>
#include<cstdint>
#include<memory>
#include<string>
#include<vector>

namespace media
{
enum class Output { Rtsp, WebRtc, Moq };
enum class VideoCodec { H264, H265 };
enum class PixelFormat { Nv12, Nv21 };
using Bytes = std::vector<std::uint8_t>;
using SharedBytes = std::shared_ptr<const Bytes>;
// A backend-specific, owning buffer. Its destructor returns the frame lease.
// Copying metadata without owning the underlying pixels is not sufficient.
struct FrameBuffer { virtual ~FrameBuffer() = default; };
struct RawVideoFrame
{
    std::uint64_t sourceEpoch{0}, sequence{0}, ptsUs{0}; // Zero is a valid PTS.
    std::uint32_t width{0}, height{0};
    PixelFormat format{PixelFormat::Nv21};
    std::shared_ptr<const FrameBuffer> buffer;
};
using RawFrame = std::shared_ptr<const RawVideoFrame>;
struct EncodedVideoFrame
{
    Output output{Output::Rtsp};
    VideoCodec codec{VideoCodec::H264};
    std::uint64_t epoch{0}, sourceEpoch{0}, sourceSequence{0}, ptsUs{0}, dtsUs{0}, configVersion{0};
    std::uint64_t gopSequence{0}, frameInGop{0}; // Stable identities shared by every MoQ subscriber.
    bool keyFrame{false};
    SharedBytes data; // One complete Annex-B access unit, owned independently of VENC.
    SharedBytes parameterSets; // Annex-B SPS/PPS, or VPS/SPS/PPS; retained for late subscribers.
};
using EncodedFrame = std::shared_ptr<const EncodedVideoFrame>;
struct NalView { const std::uint8_t* data; std::size_t size; };
std::vector<NalView> annexBNals(const Bytes& bytes);
bool isRandomAccess(VideoCodec codec, const Bytes& bytes);
SharedBytes updateParameterSets(VideoCodec codec, const Bytes& bytes, const SharedBytes& previous);
bool completeParameterSets(VideoCodec codec, const SharedBytes& sets);
const char* outputName(Output output);
} // namespace media
