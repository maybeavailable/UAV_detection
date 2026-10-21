// Internal checked conversions for the C++17 GoogCC adaptation.
#pragma once
#include<chrono>
#include<cmath>
#include<cstdint>
#include<limits>
#include<stdexcept>
namespace webrtc { namespace cc_detail {
inline std::uint64_t bounded(long double value)
{
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return value >= static_cast<long double>(max) ? max : (value > 0 ? static_cast<std::uint64_t>(value) : 0);
}
inline std::uint64_t scale(std::uint64_t rate, double factor)
{ return bounded(std::round(static_cast<double>(rate) * factor)); }
inline std::uint64_t bytesForRate(std::uint64_t rate, std::chrono::microseconds duration)
{
    if (duration.count() < 0) throw std::invalid_argument("negative rate window");
    const auto us = static_cast<std::uint64_t>(duration.count());
    if (!us) return 0;
    if (rate <= std::numeric_limits<std::uint64_t>::max() / us)
    {
        const auto microbits = rate * us;
        return microbits / 8000000 + (microbits % 8000000 >= 4000000);
    }
    return bounded(std::floor(static_cast<long double>(rate) * us / 8000000 + 0.5L));
}
inline std::uint64_t rateForBytes(std::uint64_t bytes, std::chrono::microseconds duration)
{
    if (duration.count() <= 0) throw std::invalid_argument("nonpositive rate window");
    const auto us = static_cast<std::uint64_t>(duration.count());
    if (bytes <= std::numeric_limits<std::uint64_t>::max() / 8000000) return bytes * 8000000 / us;
    return bounded(static_cast<long double>(bytes) * 8000000 / us);
}
} } // namespace webrtc::cc_detail
