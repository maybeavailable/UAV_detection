// Shared deterministic probe feedback INPUTS, not a bandwidth estimator.
#pragma once
#include<cstdint>
#include<optional>

namespace probe_bitrate_replay
{
struct Sample
{
    int cluster_id{0}, min_probes{5}, min_bytes{5000};
    std::uint64_t size{1000};
    std::int64_t send_us{0}, receive_us{0};
    bool fetch{false};
};
struct Input
{
    std::uint32_t random;
    explicit Input(int scenario) : random(0x70726573U + scenario) {}
    std::uint32_t next() { random = random * 1664525U + 1013904223U; return random; }
    Sample sample(int i, int scenario)
    {
        const int group = i / 20, parity = i % 2, step = i % 20 / 2;
        Sample p; p.cluster_id = group * 2 + parity;
        std::int64_t send_base = 1000000 + group * 250000LL + parity * 1500;
        std::int64_t receive_base = 3000000 + group * 250000LL + parity * 5000;
        std::int64_t send_delta = 10000, receive_delta = 10000;
        int send_index = step, receive_index = step;
        const auto noise = next();
        if (scenario == 1)
        {
            const std::uint64_t sizes[] = {200, 800, 1000, 1200, 1400, 2000};
            p.size = sizes[(noise >> 16) % 6]; send_delta = (1 + p.cluster_id % 4) * 5000;
            switch (p.cluster_id % 5)
            {
            case 0: receive_delta = send_delta; break;
            case 1: receive_delta = send_delta * 5 / 4; break;
            case 2: receive_delta = send_delta * 3; break;
            case 3: receive_delta = send_delta * 3 / 4; break;
            case 4: receive_delta = send_delta / 3; break;
            }
            p.min_probes = 5 + (noise >> 24) % 4; p.min_bytes = 4000 + (p.cluster_id % 5) * 1000;
        }
        if (scenario == 2)
        {
            const int order[] = {2, 0, 1, 5, 3, 4, 8, 6, 7, 9};
            send_index = order[step]; receive_index = order[(step + 3) % 10];
            if (send_index % 4 == 3) --send_index;
            if (receive_index % 5 == 4) --receive_index;
            send_delta = 5000; receive_delta = 7000; p.size = 100 + (noise >> 16) % 2000;
            p.min_bytes = 3000;
        }
        if (scenario == 3)
        {
            switch (p.cluster_id % 4)
            {
            case 0: send_delta = 0; break;
            case 1: receive_delta = 0; break;
            case 2: send_delta = 150000; break;
            case 3: receive_delta = 150000; break;
            }
        }
        if (scenario == 4)
        {
            send_delta = 1000 + (p.cluster_id % 7) * 137; receive_delta = send_delta * 5 / 4;
            p.size = step % 3 ? 12500 : 12879; p.min_probes = 2; p.min_bytes = 7;
        }
        if (scenario == 5)
        {
            p.cluster_id = parity;
            send_base = 1000000 + group * 1090000LL + group / 2 + parity * 1500;
            receive_base = 3000000 + group * 1090000LL + group / 2 + parity * 5000;
        }
        p.send_us = send_base + send_index * send_delta;
        p.receive_us = receive_base + receive_index * receive_delta;
        p.fetch = i % 13 == 12 || (step == 9 && parity == 1);
        return p;
    }
};
struct Hash
{
    std::uint64_t value{14695981039346656037ULL};
    void mix(std::uint64_t v) { value = (value ^ v) * 1099511628211ULL; }
    void rate(std::optional<std::uint64_t> v) { mix(v.has_value()); if (v) mix(*v); }
};
} // namespace probe_bitrate_replay
