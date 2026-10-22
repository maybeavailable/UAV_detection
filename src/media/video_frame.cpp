#include"media/video_frame.h"
#include<array>
#include<stdexcept>

namespace media
{
const char* outputName(Output output)
{
    switch (output) { case Output::Rtsp: return "rtsp"; case Output::WebRtc: return "webrtc"; case Output::Moq: return "moq"; }
    throw std::invalid_argument("unknown media output");
}
std::vector<NalView> annexBNals(const Bytes& b)
{
    std::vector<NalView> result;
    std::size_t start = 0, zeros = 0; bool found = false;
    for (std::size_t i = 0; i < b.size(); ++i)
    {
        if (!b[i]) { ++zeros; continue; }
        if (b[i] == 1 && zeros >= 2)
        {
            if (found && i - zeros > start) result.push_back({b.data() + start, i - zeros - start});
            start = i + 1; found = true;
        }
        zeros = 0;
    }
    if (found && b.size() - zeros > start) result.push_back({b.data() + start, b.size() - zeros - start});
    return result;
}
bool isRandomAccess(VideoCodec codec, const Bytes& bytes)
{
    for (const auto& n : annexBNals(bytes))
    {
        const auto type = codec == VideoCodec::H264 ? n.data[0] & 31 : (n.data[0] >> 1) & 63;
        if ((codec == VideoCodec::H264 && type == 5) || (codec == VideoCodec::H265 && type >= 16 && type <= 21)) return true;
    }
    return false;
}
SharedBytes updateParameterSets(VideoCodec codec, const Bytes& bytes, const SharedBytes& previous)
{
    std::array<Bytes, 3> sets;
    const auto collect = [&](const Bytes& data) {
        for (const auto& n : annexBNals(data))
        {
            const int type = codec == VideoCodec::H264 ? n.data[0] & 31 : (n.data[0] >> 1) & 63;
            const int index = codec == VideoCodec::H264 ? type - 7 : type - 32;
            const int count = codec == VideoCodec::H264 ? 2 : 3;
            if (index >= 0 && index < count) sets[index].assign(n.data, n.data + n.size);
        }
    };
    if (previous) collect(*previous);
    collect(bytes); Bytes merged;
    for (const auto& s : sets) if (!s.empty()) { merged.insert(merged.end(), {0, 0, 0, 1}); merged.insert(merged.end(), s.begin(), s.end()); }
    if (merged.empty()) return {};
    if (previous && *previous == merged) return previous;
    return std::make_shared<const Bytes>(std::move(merged));
}
bool completeParameterSets(VideoCodec codec, const SharedBytes& sets)
{
    if (!sets) return false;
    bool a = false, b = false, c = codec == VideoCodec::H264;
    for (const auto& n : annexBNals(*sets))
    {
        const auto type = codec == VideoCodec::H264 ? n.data[0] & 31 : (n.data[0] >> 1) & 63;
        if (codec == VideoCodec::H264) { a |= type == 7; b |= type == 8; }
        else { a |= type == 32; b |= type == 33; c |= type == 34; }
    }
    return a && b && c;
}
} // namespace media
