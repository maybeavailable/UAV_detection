#include"encode/venc_encoder.h"
#include"encode/ascend_video_frame.h"
#include"media/ascend_media_pipeline.h"
#include"media/ascend_media_device.h"
#include"moq/video_source.h"
#include"acl.h"
#include<boost/asio.hpp>
#include<atomic>
#include<cstdlib>
#include<cstring>
#include<iostream>
#include<map>
#include<mutex>
#include<stdexcept>
#include<thread>

using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
struct Channel { hi_venc_chn_attr attr{}; hi_video_frame_info input{}; bool started{false}, key{false}; std::vector<std::uint8_t> bytes; };
std::map<int, Channel> channels; std::mutex sdkMutex;
std::atomic<unsigned> allocations{0}, frees{0}, releases{0}, copies{0};
bool noResult{false}, badPacket{false}; int failCreate{-1}; aclrtRunMode mode = ACL_DEVICE;
void* context = reinterpret_cast<void*>(0x1234);
thread_local void* currentContext = nullptr;
}
extern "C"
{
aclError aclrtSetCurrentContext(aclrtContext c) { currentContext = c; return 0; }
aclError aclrtGetRunMode(aclrtRunMode* value) { *value = mode; return 0; }
aclError aclrtMemcpy(void* dst, std::size_t cap, const void* src, std::size_t bytes, aclrtMemcpyKind)
{ if (cap < bytes) return 1; std::memcpy(dst, src, bytes); ++copies; return 0; }
aclError aclrtMemcpy2d(void* dst, std::size_t dpitch, const void* src, std::size_t spitch, std::size_t width, std::size_t height, aclrtMemcpyKind)
{ for (std::size_t i = 0; i < height; ++i) std::memcpy(static_cast<char*>(dst) + i * dpitch, static_cast<const char*>(src) + i * spitch, width); return 0; }
hi_s32 hi_mpi_dvpp_malloc(hi_u32, hi_void** memory, hi_u64 bytes) { *memory = std::malloc(bytes); ++allocations; return *memory ? 0 : -1; }
hi_s32 hi_mpi_dvpp_free(hi_void* memory) { std::free(memory); ++frees; return 0; }
hi_s32 hi_mpi_venc_create_chn(hi_venc_chn ch, const hi_venc_chn_attr* attr)
{
    std::lock_guard<std::mutex> lock(sdkMutex); CHECK(currentContext == context);
    if (ch == failCreate || channels.count(ch)) return -1;
    channels[ch].attr = *attr; return 0;
}
hi_s32 hi_mpi_venc_destroy_chn(hi_venc_chn ch) { std::lock_guard<std::mutex> lock(sdkMutex); channels.erase(ch); return 0; }
hi_s32 hi_mpi_venc_start_chn(hi_venc_chn ch, const hi_venc_start_param*) { std::lock_guard<std::mutex> lock(sdkMutex); channels.at(ch).started = true; return 0; }
hi_s32 hi_mpi_venc_stop_chn(hi_venc_chn ch) { std::lock_guard<std::mutex> lock(sdkMutex); channels.at(ch).started = false; return 0; }
hi_s32 hi_mpi_venc_get_chn_attr(hi_venc_chn ch, hi_venc_chn_attr* attr) { std::lock_guard<std::mutex> lock(sdkMutex); *attr = channels.at(ch).attr; return 0; }
hi_s32 hi_mpi_venc_set_chn_attr(hi_venc_chn ch, const hi_venc_chn_attr* attr) { std::lock_guard<std::mutex> lock(sdkMutex); channels.at(ch).attr = *attr; return 0; }
hi_s32 hi_mpi_venc_request_idr(hi_venc_chn ch, hi_bool) { std::lock_guard<std::mutex> lock(sdkMutex); channels.at(ch).key = true; return 0; }
hi_s32 hi_mpi_venc_send_frame(hi_venc_chn ch, const hi_video_frame_info* frame, hi_s32)
{ std::lock_guard<std::mutex> lock(sdkMutex); auto& c = channels.at(ch); CHECK(c.started && frame->v_frame.virt_addr[0]); c.input = *frame; return 0; }
hi_s32 hi_mpi_venc_query_status(hi_venc_chn, hi_venc_chn_status* status) { *status = {}; status->cur_packs = noResult ? 0 : 1; return 0; }
hi_s32 hi_mpi_venc_get_stream(hi_venc_chn ch, hi_venc_stream* stream, hi_s32)
{
    std::lock_guard<std::mutex> lock(sdkMutex); auto& c = channels.at(ch); CHECK(stream->pack_cnt >= 1);
    c.bytes = {0xaa,0xbb,0xcc,0,0,0,1,0x67,0x42,0,0x1f,0xe9,0,0,0,1,0x68,0xce,6,0xe2,0,0,0,1,0x65,0x88,0x84};
    auto& pack = stream->pack[0]; pack = {}; pack.addr = c.bytes.data(); pack.len = c.bytes.size();
    pack.offset = badPacket ? pack.len + 1 : 3; pack.is_frame_end = HI_TRUE; pack.pts = c.input.v_frame.pts; stream->pack_cnt = 1; return 0;
}
hi_s32 hi_mpi_venc_release_stream(hi_venc_chn ch, hi_venc_stream*)
{ std::lock_guard<std::mutex> lock(sdkMutex); auto& bytes = channels.at(ch).bytes; std::fill(bytes.begin(), bytes.end(), 0xdd); ++releases; return 0; }
}
namespace
{
struct Input
{
    std::vector<std::uint8_t> y = std::vector<std::uint8_t>(80 * 64, 0x11), uv = std::vector<std::uint8_t>(80 * 32, 0x22);
    hi_video_frame_info frame{};
    Input(unsigned width = 64, unsigned height = 64)
    {
        const auto stride = width + 16; y.assign(stride * height, 0x11); uv.assign(stride * height / 2, 0x22);
        auto& f = frame.v_frame; f.width = width; f.height = height; f.pts = 0; f.pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
        f.compress_mode = HI_COMPRESS_MODE_NONE; f.video_format = HI_VIDEO_FORMAT_LINEAR;
        f.width_stride[0] = f.width_stride[1] = stride; f.virt_addr[0] = y.data(); f.virt_addr[1] = uv.data();
    }
};
encode::EncoderConfig config() { encode::EncoderConfig c; c.width = c.height = 64; return c; }
void boundedPoolAndThreeChannels()
{
    Input input; auto initialAllocations = allocations.load();
    {
        encode::AscendFramePool pool({64,64,0,1,context}); auto frame = pool.tryCopy(input.frame, 0, 1); CHECK(frame && frame->ptsUs == 0);
        CHECK(!pool.tryCopy(input.frame, 0, 2) && pool.snapshot().dropped == 1);
        input.y.assign(input.y.size(), 0xff);
        const auto buffer = std::dynamic_pointer_cast<const encode::AscendFrameBuffer>(frame->buffer);
        CHECK(*static_cast<std::uint8_t*>(buffer->frame.v_frame.virt_addr[0]) == 0x11);
        CHECK(*static_cast<std::uint8_t*>(buffer->frame.v_frame.virt_addr[1]) == 0x22);
        auto factory = encode::vencFactory(10, context); std::vector<std::unique_ptr<encode::VideoEncoder>> encoders;
        for (unsigned i = 0; i < 3; ++i)
        {
            auto encoder = factory(static_cast<media::Output>(i)); encoder->start(config()); encoder->requestKeyFrame();
            encoder->setBitrate(500999); CHECK(channels.at(10 + i).attr.rc_attr.h264_cbr.bit_rate == 500);
            auto encoded = encoder->encode(frame); CHECK(encoded.data && encoded.data->at(0) == 0 && media::isRandomAccess(media::VideoCodec::H264, *encoded.data));
            CHECK(channels.at(10 + i).input.v_frame.virt_addr[0] == buffer->frame.v_frame.virt_addr[0]);
            encoders.push_back(std::move(encoder));
        }
        CHECK(channels.size() == 3 && allocations == initialAllocations + 1);
        encoders[1]->setBitrate(1000000); CHECK(channels.at(11).attr.rc_attr.h264_cbr.bit_rate == 1000 && channels.at(10).attr.rc_attr.h264_cbr.bit_rate == 500);
        for (auto& e : encoders) e->stop();
        CHECK(channels.empty());
    }
    CHECK(allocations == frees);
}
void errorsAndHostCopies()
{
    Input input; encode::AscendFramePool pool({64,64,0,1,context}); auto frame = pool.tryCopy(input.frame, 0, 1);
    mode = ACL_HOST; encode::VencEncoder encoder({20,context,5ms}); encoder.start(config());
    const auto before = copies.load(); auto output = encoder.encode(frame); CHECK(copies == before + 1 && output.data->at(0) == 0);
    const auto released = releases.load(); badPacket = true; bool failed = false;
    try { (void)encoder.encode(frame); } catch (const std::runtime_error&) { failed = true; }
    CHECK(failed && releases == released + 1); badPacket = false; encoder.stop();
    encoder.start(config()); noResult = true; failed = false;
    try { (void)encoder.encode(frame); } catch (const std::runtime_error&) { failed = true; }
    frame.reset(); CHECK(failed && pool.snapshot().inUse == 1); encoder.stop(); CHECK(pool.snapshot().inUse == 0); noResult = false;
    failCreate = 20; failed = false;
    try { encoder.start(config()); } catch (const std::runtime_error&) { failed = true; }
    CHECK(failed && channels.empty()); failCreate = -1; mode = ACL_DEVICE;
}
void capturePipelineAndRtsp()
{
    Input input; media::AscendMediaPipeline::Config c; c.context = context; c.firstVencChannel = 30; c.rtspPort = 0;
    c.media = media::MediaService::Config::forVideo(64, 64, 25);
    c.media.encoders[1].width = c.media.encoders[1].height = 32;
    media::AscendMediaPipeline pipeline(c); pipeline.start(); CHECK(pipeline.rtspPort() > 0);
    auto rtc = pipeline.service().subscribe(media::Output::WebRtc); moq::VideoSource a(pipeline.service()), b(pipeline.service());
    CHECK(pipeline.submitBorrowed(input.frame));
    Input sub(32, 32); CHECK(pipeline.submitBorrowed(sub.frame));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((!rtc->snapshot().queuedFrames || !a.snapshot().queuedFrames || !b.snapshot().queuedFrames) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(rtc->tryPop() && a.next() && b.next());
    { std::lock_guard<std::mutex> lock(sdkMutex);
        CHECK(channels.at(30).input.v_frame.width == 64 && channels.at(31).input.v_frame.width == 32);
        CHECK(channels.at(30).input.v_frame.virt_addr[0] == channels.at(32).input.v_frame.virt_addr[0]);
        CHECK(channels.at(30).input.v_frame.virt_addr[0] != channels.at(31).input.v_frame.virt_addr[0]); }
    boost::asio::io_context io; boost::asio::ip::tcp::socket socket(io);
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), pipeline.rtspPort()});
    const std::string request = "DESCRIBE rtsp://127.0.0.1/live/rgb RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request)); boost::asio::streambuf response;
    boost::asio::read_until(socket, response, "\r\n\r\n");
    std::istream stream(&response); std::string status; std::getline(stream, status); CHECK(status.find("200 OK") != std::string::npos);
    pipeline.stop(); CHECK(channels.empty() && pipeline.framePoolSnapshot().inUse == 0 && rtc->snapshot().closed);
    pipeline.start(); CHECK(pipeline.rtspPort() > 0); pipeline.stop();
}
void threeSourceDevice()
{
    media::AscendMediaDevice::Config c; c.context = context; c.firstVencChannel = 40; c.rtspPort = 0;
    CHECK(c.sources[0].width == 2566 && c.sources[0].height == 1520);
    CHECK(c.sources[1].width == 1920 && c.sources[1].height == 1080 && c.sources[2].width == 640 && c.sources[2].height == 512);
    for (unsigned i = 0; i < 3; ++i) c.sources[i].width = c.sources[i].height = 64 >> i;
    media::AscendMediaDevice device(c); device.start();
    std::array<std::shared_ptr<media::MediaSubscription>, 3> rtc, moq;
    for (unsigned i = 0; i < 3; ++i)
    {
        rtc[i] = device.service(i).subscribe(media::Output::WebRtc); moq[i] = device.service(i).subscribe(media::Output::Moq);
        Input input(64 >> i, 64 >> i); CHECK(device.submitBorrowed(i, input.frame));
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    for (unsigned i = 0; i < 3; ++i)
    {
        while ((!rtc[i]->snapshot().queuedFrames || !moq[i]->snapshot().queuedFrames) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        CHECK(rtc[i]->tryPop() && moq[i]->tryPop());
        boost::asio::io_context io; boost::asio::ip::tcp::socket socket(io);
        socket.connect({boost::asio::ip::make_address("127.0.0.1"), device.rtspPort()});
        const auto url = "rtsp://127.0.0.1" + c.sources[i].path;
        const auto describe = "DESCRIBE " + url + " RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(describe)); boost::asio::streambuf response;
        boost::asio::read_until(socket, response, "\r\n\r\n");
        std::istream stream(&response); std::string status; std::getline(stream, status); CHECK(status.find("200 OK") != std::string::npos);
    }
    { std::lock_guard<std::mutex> lock(sdkMutex); CHECK(channels.size() == 9);
        for (unsigned i = 0; i < 3; ++i) for (unsigned output = 0; output < 3; ++output)
            CHECK(channels.at(40 + 3 * i + output).input.v_frame.width == (64u >> i)); }
    device.stop(); CHECK(channels.empty());
    for (const auto& sub : rtc) CHECK(sub->snapshot().closed);
    device.start(); device.stop();
    failCreate = 48; bool failed = false;
    try { device.start(); } catch (const std::runtime_error&) { failed = true; }
    failCreate = -1; CHECK(failed && channels.empty()); // Last of nine channel allocations rolls the whole device back.
    device.start(); device.stop();
}
}
int main()
{
    try { boundedPoolAndThreeChannels(); errorsAndHostCopies(); capturePipelineAndRtsp(); threeSourceDevice(); CHECK(channels.empty() && allocations == frees);
        std::cout << "PASS: real CANN headers, bounded shared input pool, three VENC channels, kbps conversion, output ownership and failure cleanup\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
