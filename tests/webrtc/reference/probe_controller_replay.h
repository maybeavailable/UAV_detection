// Shared deterministic configuration/events, not a probe controller model.
#pragma once
#include<cstdint>
#include<optional>

namespace probe_replay
{
inline bool rapidRecovery(int scenario) { return scenario == 3 || scenario == 7; }
template<class Config, class Duration, class Unbounded>
void configure(int s, Config& c, Duration duration, Unbounded unbounded)
{
    if (s == 1)
    {
        c.second_exponential_probe_scale = std::nullopt; c.first_exponential_probe_scale = 2.5;
        c.further_exponential_probe_scale = 1.6; c.abort_further_probe_if_max_lower_than_current = true;
        c.repeated_initial_probing_time_period = duration(3000000); c.initial_probe_duration = duration(120000);
        c.min_probe_packets_sent = 7; c.min_probe_delta = duration(2500);
    }
    if (s == 2)
    {
        c.alr_probing_interval = duration(1500000); c.first_allocation_probe_scale = 1.5; c.second_allocation_probe_scale = 2.5;
        c.allocation_probe_limit_by_current_scale = 3.0; c.network_state_estimate_probing_interval = duration(2000000);
        c.network_state_probe_duration = duration(30000); c.network_state_min_probe_delta = duration(10000);
    }
    if (s == 3)
    {
        c.probe_if_estimate_lower_than_network_state_estimate_ratio = .7;
        c.network_state_estimate_probing_interval = unbounded();
        c.estimate_lower_than_network_state_estimate_probing_interval = duration(1200000);
        c.first_allocation_probe_scale = std::nullopt; c.second_allocation_probe_scale = std::nullopt;
    }
    if (s == 4)
    {
        c.network_state_estimate_probing_interval = duration(1400000); c.network_state_probe_scale = .9;
        c.further_probe_threshold = .6; c.skip_if_estimate_larger_than_fraction_of_max = .9;
        c.skip_probe_max_allocated_scale = 1.5; c.loss_limited_probe_scale = 1.8;
    }
    if (s == 5)
    {
        c.second_exponential_probe_scale = 0.0; c.second_allocation_probe_scale = 0.0;
        c.probe_on_max_allocated_bitrate_change_without_alr = false;
        c.loss_limited_probe_scale = 1.25; c.repeated_initial_probing_time_period = duration(0);
    }
    if (s == 6)
    {
        c.probe_if_estimate_lower_than_network_state_estimate_ratio = 1.4;
        c.estimate_lower_than_network_state_estimate_probing_interval = unbounded();
        c.network_state_estimate_probing_interval = duration(750000); c.probe_on_max_allocated_bitrate_change = false;
        c.min_probe_duration = duration(20000); c.min_probe_delta = duration(7000);
    }
    if (s == 7)
    {
        c.network_state_estimate_probing_interval = duration(5000000);
        c.skip_if_estimate_larger_than_fraction_of_max = .8; c.skip_probe_max_allocated_scale = .75;
        c.network_state_probe_duration = duration(50000); c.initial_probe_duration = duration(200000);
    }
}
enum class Kind { Reset, Availability, Bitrates, Allocation, Estimate, Periodic, Repeated, AlrStart, AlrEnd, Request, NetworkEstimate, Process };
struct Event
{
    Kind kind{Kind::Process};
    std::int64_t at_us{0};
    std::uint64_t rate{0}, maximum{0};
    bool flag{false};
    std::optional<std::uint64_t> upper;
    int cause{2};
};
struct Input
{
    std::uint32_t random;
    explicit Input(int s) : random(0x70726f62U + s) {}
    std::uint32_t next() { random = random * 1664525U + 1013904223U; return random; }
    Event event(int i, int s)
    {
        Event e; e.at_us = 100000000 + i * 100000LL;
        const auto noise = next() % 100000;
        const auto cycle = i / 100;
        switch (i % 100)
        {
        case 0: e.kind = Kind::Reset; break;
        case 1: case 35: case 95: e.kind = Kind::Availability; e.flag = true; break;
        case 2: e.kind = Kind::Bitrates; e.rate = 300000 + noise; e.maximum = cycle % 3 ? 5000000 : 0; break;
        case 3: e.kind = Kind::Repeated; e.flag = cycle % 2 == 0 || s == 2; break;
        case 4: e.kind = Kind::Periodic; e.flag = s != 5 || cycle % 2; break;
        case 5: case 30: e.kind = Kind::AlrStart; e.flag = true; break;
        case 6: e.kind = Kind::Estimate; e.rate = 800000 + noise; break;
        case 8: case 70:
            e.kind = Kind::NetworkEstimate;
            if (cycle % 3) e.upper = (i % 100 == 70 ? 20000000 : 2000000) + noise;
            break;
        case 10: e.kind = Kind::Estimate; e.rate = 1500000 + noise; e.cause = cycle % 5; break;
        case 12: e.kind = Kind::Allocation; e.rate = cycle % 4 ? 1000000 + noise : 0; break;
        case 13: e.kind = Kind::Estimate; e.rate = 600000 + noise; e.cause = cycle % 5; break;
        case 14: case 55: e.kind = Kind::AlrEnd; break;
        case 15: case 56: e.kind = Kind::AlrStart; e.flag = false; break;
        case 16: case 52: case 53: case 57: e.kind = Kind::Request; break;
        case 20: case 90: e.kind = Kind::Availability; e.flag = false; break;
        case 25: e.kind = Kind::Bitrates; e.maximum = 8000000 + noise; break;
        case 50: e.kind = Kind::Estimate; e.rate = 3000000 + noise; break;
        case 51: e.kind = Kind::Estimate; e.rate = 1000000 + noise; break;
        case 60: e.kind = Kind::NetworkEstimate; e.upper = 0; break;
        case 65: e.kind = Kind::AlrStart; e.flag = true; break;
        case 80: e.kind = Kind::Allocation; e.rate = 2000000 + noise; break;
        case 82: e.kind = Kind::Estimate; e.rate = 2000000 + noise; e.cause = cycle % 5; break;
        default: break;
        }
        return e;
    }
};
struct Hash
{
    std::uint64_t value{14695981039346656037ULL};
    void mix(std::uint64_t v) { value = (value ^ v) * 1099511628211ULL; }
    void optional(std::optional<std::uint64_t> v) { mix(v.has_value()); if (v) mix(*v); }
};
} // namespace probe_replay
