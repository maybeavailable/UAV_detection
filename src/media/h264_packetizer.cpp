#include"media/h264_packetizer.h"
#include<algorithm>
#include<random>
#include<stdexcept>

namespace media
{
H264Packetizer::H264Packetizer(Config c) : config_(c)
{
    if (c.payloadType < 96 || c.payloadType > 127 || c.maxPayloadBytes < 3 || c.maxPayloadBytes > 60000)
        throw std::invalid_argument("invalid H264 RTP packetizer configuration");
    std::random_device random; if (!config_.ssrc) do { config_.ssrc = random(); } while (!config_.ssrc);
    sequence_ = static_cast<std::uint16_t>(random()); baseTimestamp_ = random();
}
std::vector<Bytes> H264Packetizer::packetize(const EncodedVideoFrame& f)
{
    if (f.codec != VideoCodec::H264 || !f.data) throw std::invalid_argument("H264 access unit required");
    std::vector<NalView> nals;
    if (f.keyFrame && f.parameterSets) nals = annexBNals(*f.parameterSets);
    const auto picture = annexBNals(*f.data); nals.insert(nals.end(), picture.begin(), picture.end());
    if (picture.empty()) throw std::invalid_argument("empty Annex-B access unit");
    for (const auto& n : nals) if (!(n.data[0] & 31) || (n.data[0] & 31) >= 24 || (n.data[0] & 0x80))
        throw std::invalid_argument("invalid elementary H264 NAL");
    if (!epoch_ || *epoch_ != f.epoch || !basePts_ || f.ptsUs < *basePts_)
    { if (epoch_) baseTimestamp_ = lastTimestamp_ + 3000; epoch_ = f.epoch; basePts_ = f.ptsUs; }
    const auto delta = f.ptsUs - *basePts_;
    const auto stamp = baseTimestamp_ + static_cast<std::uint32_t>((delta / 1000000) * 90000 + (delta % 1000000) * 90000 / 1000000);
    std::vector<Bytes> packets;
    const auto emit = [&](const std::uint8_t* data, std::size_t size, bool marker) {
        Bytes p(12 + size); p[0] = 0x80; p[1] = static_cast<std::uint8_t>(config_.payloadType | (marker ? 0x80 : 0));
        p[2] = static_cast<std::uint8_t>(sequence_ >> 8); p[3] = static_cast<std::uint8_t>(sequence_++);
        for (unsigned i = 0; i < 4; ++i) { p[4 + i] = static_cast<std::uint8_t>(stamp >> (24 - i * 8)); p[8 + i] = static_cast<std::uint8_t>(config_.ssrc >> (24 - i * 8)); }
        std::copy(data, data + size, p.begin() + 12); packets.push_back(std::move(p));
    };
    for (std::size_t i = 0; i < nals.size(); ++i)
    {
        const auto n = nals[i];
        if (n.size <= config_.maxPayloadBytes) { emit(n.data, n.size, i + 1 == nals.size()); continue; }
        std::size_t offset = 1;
        while (offset < n.size)
        {
            const auto bytes = std::min(n.size - offset, config_.maxPayloadBytes - 2);
            Bytes fragment(bytes + 2); fragment[0] = static_cast<std::uint8_t>((n.data[0] & 0xe0) | 28);
            fragment[1] = static_cast<std::uint8_t>((n.data[0] & 31) | (offset == 1 ? 0x80 : 0) | (offset + bytes == n.size ? 0x40 : 0));
            std::copy(n.data + offset, n.data + offset + bytes, fragment.begin() + 2);
            offset += bytes; emit(fragment.data(), fragment.size(), i + 1 == nals.size() && offset == n.size);
        }
    }
    lastTimestamp_ = stamp; return packets;
}
} // namespace media
