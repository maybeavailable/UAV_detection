// Shared deterministic INPUTS for the project test and the upstream oracle.
// This file contains no bandwidth estimator implementation.
#pragma once
#include<cstdint>
#include<optional>
#include<vector>

namespace loss_bwe_replay
{
template<class Config, class MakeRate, class MakeDuration>
void configure(int scenario, Config& c, MakeRate rate, MakeDuration duration)
{
    if (scenario == 1)
    {
        c.use_byte_loss_rate = false; c.observation_window_size = 8; c.min_num_observations = 1;
        c.hold_duration_factor = 0; c.padding_duration = duration(0); c.lower_bound_by_acked_rate_factor = 0;
        c.bound_best_candidate = false; c.append_delay_based_estimate_candidate = false;
        c.candidate_factors = {1.1, 1.0, 0.9}; c.newton_iterations = 2;
    }
    if (scenario == 2)
    {
        c.observation_window_size = 5; c.min_num_observations = 1;
        c.sending_rate_smoothing_factor = .15; c.instant_upper_bound_temporal_weight_factor = .97;
        c.temporal_weight_factor = .98; c.not_increase_if_inherent_loss_less_than_average_loss = false;
        c.hold_duration_factor = 3; c.padding_duration = duration(1000000);
    }
    if (scenario == 3)
    {
        c.append_upper_bound_candidate_in_alr = true; c.lower_bound_by_acked_rate_factor = 0;
        c.rampup_acceleration_max_factor = .4; c.rampup_acceleration_maxout_time = duration(3000000);
        c.bandwidth_rampup_upper_bound_factor_in_hold = 1.4;
    }
    if (scenario == 4)
    {
        c.candidate_factors = {1.05, .99}; c.append_acknowledged_rate_candidate = false;
        c.append_delay_based_estimate_candidate = false; c.higher_bandwidth_bias_factor = 0;
        c.higher_log_bandwidth_bias_factor = 0; c.inherent_loss_lower_bound = .01;
        c.initial_inherent_loss_estimate = .1; c.newton_step_size = .4; c.newton_iterations = 3;
        c.use_byte_loss_rate = false; c.observation_window_size = 2; c.min_num_observations = 2;
    }
    if (scenario == 5)
    {
        c.bandwidth_rampup_upper_bound_factor = 1.2; c.candidate_factors = {1.1, 1.0, .95};
        c.higher_bandwidth_bias_factor = .01; c.inherent_loss_upper_bound_bandwidth_balance = rate(14000);
        c.inherent_loss_upper_bound_offset = .9; c.newton_iterations = 2; c.newton_step_size = .4;
        c.sending_rate_smoothing_factor = .01; c.instant_upper_bound_temporal_weight_factor = .97;
        c.instant_upper_bound_bandwidth_balance = rate(90000); c.instant_upper_bound_loss_offset = .1;
        c.temporal_weight_factor = .98; c.min_num_observations = 1; c.max_increase_factor = 1.5;
        c.use_byte_loss_rate = false; c.use_in_start_phase = false;
    }
    if (scenario == 6)
    {
        c.lower_bound_by_acked_rate_factor = 1.25; c.median_sending_rate_factor = 0;
        c.sending_rate_smoothing_factor = .4; c.hold_duration_factor = 0;
        c.observation_duration_lower_bound = duration(150000);
    }
    if (scenario == 7)
    {
        c.lower_bound_by_acked_rate_factor = 0; c.padding_duration = duration(500000);
        c.not_use_acked_rate_in_alr = false; c.observation_window_size = 3;
        c.bandwidth_backoff_lower_bound_factor = .85;
    }
}
struct Packet
{
    std::int64_t sequence, send_us;
    std::uint64_t size;
    bool received;
};
struct Input
{
    std::uint32_t random;
    std::int64_t next_sequence{0};
    std::optional<Packet> last_lost;
    explicit Input(int scenario) : random(0x6c6f7373U + scenario) {}
    std::uint32_t next() { random = random * 1664525U + 1013904223U; return random; }
    std::vector<Packet> packets(int i)
    {
        std::vector<Packet> result;
        const int loss_percent = i < 60 ? 0 : (i < 160 ? 20 : (i < 220 ? 70 : (i < 260 ? 100 : (i < 420 ? 0 : (i < 500 ? 10 : 0)))));
        for (int j = 0; j < 12; ++j)
        {
            Packet packet{next_sequence++, 1000000 + i * 100000LL + j * 9000LL, std::uint64_t(j % 4 ? 1200 : 2000), int(next() % 100) >= loss_percent};
            if (!packet.received && i % 61 == 0) packet.size = 35000;
            if (!packet.received) last_lost = packet;
            result.push_back(packet);
        }
        if (last_lost && i % 23 == 22)
        {
            auto recovered = *last_lost; recovered.received = true; result.push_back(recovered); last_lost.reset();
        }
        else if (last_lost && i % 17 == 16) result.push_back(*last_lost);
        return result;
    }
    std::uint64_t ack(int i)
    { return (i < 170 ? 500000 : (i < 250 ? 160000 : (i < 400 ? 800000 : 650000))) + next() % 20000; }
    std::optional<std::uint64_t> delay(int i, int scenario) const
    {
        if (i % 13 == 0 || (scenario == 3 && i % 100 < 30)) return std::nullopt;
        return i < 70 ? 1000000 : (i < 250 ? 700000 : 1200000);
    }
};
struct Hash
{
    std::uint64_t value{14695981039346656037ULL};
    void mix(std::uint64_t v) { value = (value ^ v) * 1099511628211ULL; }
    void rate(std::optional<std::uint64_t> v) { mix(v.has_value()); if (v) mix(*v); }
};
} // namespace loss_bwe_replay
