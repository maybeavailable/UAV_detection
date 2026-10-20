/*
 * Copyright 2021 The WebRTC project authors. All rights reserved.
 * Adapted from modules/congestion_controller/goog_cc/loss_based_bwe_v2.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/loss_based_bwe_v2.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<iterator>
#include<limits>
#include<stdexcept>
#include<string>
#include<type_traits>
#include<unordered_map>
#include<utility>

namespace webrtc
{
// Private integer units preserve the upstream rate/byte/us rounding rules.
// They deliberately stay in this .cpp; the public API uses standard C++ types.
namespace
{
constexpr std::int64_t kPositiveInfinity = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kNegativeInfinity = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMaxFinite = kPositiveInfinity - 1;
std::int64_t finiteTicks(long double value)
{
    if (std::isnan(value)) throw std::overflow_error("non-finite loss BWE arithmetic");
    if (value >= static_cast<long double>(kMaxFinite)) return kMaxFinite;
    if (value <= static_cast<long double>(kNegativeInfinity + 1)) return kNegativeInfinity + 1;
    return static_cast<std::int64_t>(value);
}
std::int64_t roundedFraction(std::int64_t value, std::int64_t denominator)
{
    return value / denominator + (value % denominator >= denominator / 2) -
        (value % denominator <= -denominator / 2);
}
template<class Derived> class Unit
{
public:
    static constexpr Derived Zero() { return Derived(0); }
    static constexpr Derived PlusInfinity() { return Derived(kPositiveInfinity); }
    static constexpr Derived MinusInfinity() { return Derived(kNegativeInfinity); }
    bool IsFinite() const { return ticks_ != kPositiveInfinity && ticks_ != kNegativeInfinity; }
    bool IsInfinite() const { return !IsFinite(); }
    bool IsZero() const { return ticks_ == 0; }
    std::int64_t raw() const { return ticks_; }
    double asDouble() const
    {
        return IsFinite() ? static_cast<double>(ticks_) :
            (ticks_ > 0 ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity());
    }
    bool operator==(Derived other) const { return ticks_ == other.raw(); }
    bool operator!=(Derived other) const { return !(*this == other); }
    bool operator<(Derived other) const { return ticks_ < other.raw(); }
    bool operator>(Derived other) const { return other < static_cast<const Derived&>(*this); }
    bool operator<=(Derived other) const { return !(*this > other); }
    bool operator>=(Derived other) const { return !(*this < other); }
    Derived operator+(Derived other) const
    {
        if (IsFinite() && other.IsFinite()) return Derived(finiteTicks(static_cast<long double>(ticks_) + other.raw()));
        if (!IsFinite() && !other.IsFinite() && ticks_ != other.raw())
            throw std::overflow_error("opposite infinities in loss BWE addition");
        return IsFinite() ? other : static_cast<const Derived&>(*this);
    }
    Derived operator-(Derived other) const
    {
        if (IsFinite() && other.IsFinite()) return Derived(finiteTicks(static_cast<long double>(ticks_) - other.raw()));
        if (!IsFinite() && !other.IsFinite() && ticks_ == other.raw())
            throw std::overflow_error("equal infinities in loss BWE subtraction");
        return IsFinite() ? (other.raw() > 0 ? MinusInfinity() : PlusInfinity()) : static_cast<const Derived&>(*this);
    }
    Derived& operator+=(Derived other) { return static_cast<Derived&>(*this) = *this + other; }
    Derived& operator-=(Derived other) { return static_cast<Derived&>(*this) = *this - other; }
    double operator/(Derived other) const
    {
        // Define an empty byte observation as zero loss instead of 0/0.
        return IsZero() && other.IsZero() ? 0.0 : asDouble() / other.asDouble();
    }
    Derived operator*(double scale) const
    {
        if (!IsFinite()) return scale < 0 ? (ticks_ > 0 ? MinusInfinity() : PlusInfinity()) : static_cast<const Derived&>(*this);
        return Derived(finiteTicks(std::round(static_cast<double>(ticks_) * scale)));
    }
    friend Derived operator*(double scale, Derived value) { return value * scale; }
    Derived operator/(double scale) const
    {
        if (!IsFinite()) return scale < 0 ? (ticks_ > 0 ? MinusInfinity() : PlusInfinity()) : static_cast<const Derived&>(*this);
        return Derived(finiteTicks(std::round(static_cast<double>(ticks_) / scale)));
    }
    template<class T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
    Derived operator/(T scale) const { return IsFinite() ? Derived(ticks_ / scale) : static_cast<const Derived&>(*this); }
protected:
    explicit constexpr Unit(std::int64_t ticks) : ticks_(ticks) {}
    std::int64_t ticks_;
};
class DataRate : public Unit<DataRate>
{
public:
    explicit constexpr DataRate(std::int64_t value = 0) : Unit(value) {}
    static constexpr DataRate BitsPerSec(std::int64_t value) { return DataRate(value); }
    static DataRate KilobitsPerSec(double value) { return DataRate(finiteTicks(value * 1000)); }
    std::int64_t bps() const { return raw(); }
    std::int64_t kbps() const { return roundedFraction(raw(), 1000); }
};
class DataSize : public Unit<DataSize>
{
public:
    explicit constexpr DataSize(std::int64_t value = 0) : Unit(value) {}
    static constexpr DataSize Bytes(std::int64_t value) { return DataSize(value); }
    std::int64_t bytes() const { return raw(); }
};
class TimeDelta : public Unit<TimeDelta>
{
public:
    explicit constexpr TimeDelta(std::int64_t value = 0) : Unit(value) {}
    static constexpr TimeDelta Micros(std::int64_t value) { return TimeDelta(value); }
    static constexpr TimeDelta Millis(std::int64_t value) { return TimeDelta(value * 1000); }
    static constexpr TimeDelta Seconds(std::int64_t value) { return TimeDelta(value * 1000000); }
    std::int64_t us() const { return raw(); }
    std::int64_t ms() const { return roundedFraction(raw(), 1000); }
    std::int64_t seconds() const { return roundedFraction(raw(), 1000000); }
};
class Timestamp : public Unit<Timestamp>
{
public:
    explicit constexpr Timestamp(std::int64_t value = 0) : Unit(value) {}
    static constexpr Timestamp Micros(std::int64_t value) { return Timestamp(value); }
    Timestamp operator+(TimeDelta delta) const
    {
        return Timestamp((TimeDelta(raw()) + delta).raw());
    }
    TimeDelta operator-(Timestamp other) const { return TimeDelta(raw()) - TimeDelta(other.raw()); }
};
DataRate operator/(DataSize size, TimeDelta duration)
{
    if (size.bytes() <= kMaxFinite / 8000000)
        return DataRate::BitsPerSec(size.bytes() * 8000000 / duration.us());
    return DataRate::BitsPerSec(finiteTicks(static_cast<long double>(size.bytes()) * 8000000 / duration.us()));
}
template<class T> std::string ToString(const T& value)
{
    return value.IsFinite() ? std::to_string(value.raw()) : (value.raw() > 0 ? "+inf" : "-inf");
}
struct ModelPacket
{
    struct Sent { Timestamp send_time; DataSize size; std::int64_t sequence_number; } sent_packet;
    bool received;
    bool IsReceived() const { return received; }
};
constexpr DataRate kCongestionControllerMinBitrate = DataRate::BitsPerSec(5000);
DataRate checkedRate(std::uint64_t bps)
{
    if (bps > static_cast<std::uint64_t>(kMaxFinite)) throw std::invalid_argument("loss BWE rate exceeds finite int64 bit/s range");
    return DataRate::BitsPerSec(static_cast<std::int64_t>(bps));
}
std::optional<std::uint64_t> publicRate(DataRate rate)
{
    return rate.IsFinite() ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(std::max(std::int64_t(0), rate.bps()))) : std::nullopt;
}
} // namespace

namespace {

constexpr TimeDelta kInitHoldDuration = TimeDelta::Millis(300);
constexpr TimeDelta kMaxHoldDuration = TimeDelta::Seconds(60);

bool IsValid(DataRate datarate) {
  return datarate.IsFinite();
}

bool IsValid(std::optional<DataRate> datarate) {
  return datarate.has_value() && IsValid(datarate.value());
}

bool IsValid(Timestamp timestamp) {
  return timestamp.IsFinite();
}

double ToKiloBytes(DataSize datasize) {
  return datasize.bytes() / 1000.0;
}

double GetLossProbability(double inherent_loss,
                          DataRate loss_limited_bandwidth,
                          DataRate sending_rate) {
  if (inherent_loss < 0.0 || inherent_loss > 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The inherent loss must be in [0,1]: "
                        << inherent_loss;
    inherent_loss = std::min(std::max(inherent_loss, 0.0), 1.0);
  }
  if (!sending_rate.IsFinite()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The sending rate must be finite: "
                        << ToString(sending_rate);
  }
  if (!loss_limited_bandwidth.IsFinite()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The loss limited bandwidth must be finite: "
                        << ToString(loss_limited_bandwidth);
  }

  double loss_probability = inherent_loss;
  if (IsValid(sending_rate) && IsValid(loss_limited_bandwidth) &&
      (sending_rate > loss_limited_bandwidth)) {
    loss_probability += (1 - inherent_loss) *
                        (sending_rate - loss_limited_bandwidth) / sending_rate;
  }
  return std::min(std::max(loss_probability, 1.0e-6), 1.0 - 1.0e-6);
}

}  // namespace

struct LossBasedBweV2::Impl
{
  using Config = LossBasedBweV2::Config;
  struct Result { DataRate bandwidth_estimate = DataRate::Zero(); LossBasedState state = LossBasedState::kDelayBasedEstimate; };
  explicit Impl(Config config);
  bool IsEnabled() const;
  bool IsReady() const;
  bool ReadyToUseInStartPhase() const;
  bool UseInStartPhase() const;
  Result GetLossBasedResult() const;
  void SetAcknowledgedBitrate(DataRate acknowledged_bitrate);
  void SetMinMaxBitrate(DataRate min_bitrate, DataRate max_bitrate);
  void SetBandwidthEstimate(DataRate bandwidth_estimate);
  void UpdateBandwidthEstimate(const std::vector<ModelPacket>& packets, DataRate delay_based_estimate, bool in_alr);
  struct ChannelParameters {
    double inherent_loss = 0.0;
    DataRate loss_limited_bandwidth = DataRate::MinusInfinity();
  };

  struct Derivatives {
    double first = 0.0;
    double second = 0.0;
  };

  struct Observation {
    bool IsInitialized() const { return id != -1; }

    std::int64_t num_packets = 0;
    std::int64_t num_lost_packets = 0;
    std::int64_t num_received_packets = 0;
    DataRate sending_rate = DataRate::MinusInfinity();
    DataSize size = DataSize::Zero();
    DataSize lost_size = DataSize::Zero();
    std::int64_t id = -1;
  };

  struct PartialObservation {
    std::int64_t num_packets = 0;
    std::unordered_map<int64_t, DataSize> lost_packets;
    DataSize size = DataSize::Zero();
  };

  struct PaddingInfo {
    DataRate padding_rate = DataRate::MinusInfinity();
    Timestamp padding_timestamp = Timestamp::MinusInfinity();
  };

  struct HoldInfo {
    Timestamp timestamp = Timestamp::MinusInfinity();
    TimeDelta duration = TimeDelta::Zero();
    DataRate rate = DataRate::PlusInfinity();
  };

  bool IsConfigValid() const;

  // Returns `0.0` if not enough loss statistics have been received.
  void UpdateAverageReportedLossRatio();
  double CalculateAverageReportedPacketLossRatio() const;
  // Calculates the average loss ratio over the last `observation_window_size`
  // observations but skips the observation with min and max loss ratio in order
  // to filter out loss spikes.
  double CalculateAverageReportedByteLossRatio() const;
  std::vector<ChannelParameters> GetCandidates(bool in_alr) const;
  DataRate GetCandidateBandwidthUpperBound() const;
  Derivatives GetDerivatives(const ChannelParameters& channel_parameters) const;
  double GetFeasibleInherentLoss(
      const ChannelParameters& channel_parameters) const;
  double GetInherentLossUpperBound(DataRate bandwidth) const;
  double AdjustBiasFactor(double loss_rate, double bias_factor) const;
  double GetHighBandwidthBias(DataRate bandwidth) const;
  double GetObjective(const ChannelParameters& channel_parameters) const;
  DataRate GetSendingRate(DataRate instantaneous_sending_rate) const;
  DataRate GetInstantUpperBound() const;
  void CalculateInstantUpperBound();
  DataRate GetInstantLowerBound() const;
  void CalculateInstantLowerBound();

  void CalculateTemporalWeights();
  void NewtonsMethodUpdate(ChannelParameters& channel_parameters) const;

  // Returns false if no observation was created.
  bool PushBackObservation(const std::vector<ModelPacket>& packet_results);
  bool IsEstimateIncreasingWhenLossLimited(DataRate old_estimate,
                                           DataRate new_estimate);
  bool IsInLossLimitedState() const;
  bool CanKeepIncreasingState(DataRate estimate) const;
  DataRate GetMedianSendingRate() const;

  std::optional<DataRate> acknowledged_bitrate_;
  std::optional<Config> config_;
  ChannelParameters current_best_estimate_;
  std::int64_t num_observations_ = 0;
  std::vector<Observation> observations_;
  PartialObservation partial_observation_;
  Timestamp last_send_time_most_recent_observation_ = Timestamp::PlusInfinity();
  Timestamp last_time_estimate_reduced_ = Timestamp::MinusInfinity();
  std::optional<DataRate> cached_instant_upper_bound_;
  std::optional<DataRate> cached_instant_lower_bound_;
  std::vector<double> instant_upper_bound_temporal_weights_;
  std::vector<double> temporal_weights_;
  Timestamp recovering_after_loss_timestamp_ = Timestamp::MinusInfinity();
  DataRate bandwidth_limit_in_current_window_ = DataRate::PlusInfinity();
  DataRate min_bitrate_ = DataRate::KilobitsPerSec(1);
  DataRate max_bitrate_ = DataRate::PlusInfinity();
  DataRate delay_based_estimate_ = DataRate::PlusInfinity();
  Result loss_based_result_ = Result();
  HoldInfo last_hold_info_ = HoldInfo();
  PaddingInfo last_padding_info_ = PaddingInfo();
  double average_reported_loss_ratio_ = 0.0;
};

LossBasedBweV2::Impl::Impl(Config config)
    : config_(config.enabled ? std::optional<Config>(std::move(config)) : std::nullopt) {
  if (!config_.has_value()) {
    UAV_LOG_DEBUG << "[WebRTC][LossBasedBweV2] " << "The configuration does not specify that the "
                           "estimator should be enabled, disabling it.";
    return;
  }
  if (!IsConfigValid()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The configuration is not valid, disabling the estimator.";
    config_.reset();
    return;
  }

  current_best_estimate_.inherent_loss =
      config_->initial_inherent_loss_estimate;
  observations_.resize(config_->observation_window_size);
  temporal_weights_.resize(config_->observation_window_size);
  instant_upper_bound_temporal_weights_.resize(
      config_->observation_window_size);
  CalculateTemporalWeights();
  last_hold_info_.duration = kInitHoldDuration;
  UAV_LOG_INFO << "[WebRTC][LossBasedBweV2] initialized byte_loss=" << config_->use_byte_loss_rate
      << " observation_window=" << config_->observation_window_size;
}

bool LossBasedBweV2::Impl::IsEnabled() const {
  return config_.has_value();
}

bool LossBasedBweV2::Impl::IsReady() const {
  return IsEnabled() &&
         IsValid(current_best_estimate_.loss_limited_bandwidth) &&
         num_observations_ >= config_->min_num_observations;
}

bool LossBasedBweV2::Impl::ReadyToUseInStartPhase() const {
  return IsReady() && config_->use_in_start_phase;
}

bool LossBasedBweV2::Impl::UseInStartPhase() const {
  return config_ && config_->use_in_start_phase;
}

LossBasedBweV2::Impl::Result LossBasedBweV2::Impl::GetLossBasedResult() const {
  if (!IsReady()) {
    if (!IsEnabled()) {
      UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
          << "The estimator must be enabled before it can be used.";
    } else {
      if (!IsValid(current_best_estimate_.loss_limited_bandwidth)) {
        UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
            << "The estimator must be initialized before it can be used.";
      }
      if (num_observations_ <= config_->min_num_observations) {
        UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The estimator must receive enough loss "
                               "statistics before it can be used.";
      }
    }
    return {IsValid(delay_based_estimate_)
                                      ? delay_based_estimate_
                                      : DataRate::PlusInfinity(), LossBasedState::kDelayBasedEstimate};
  }
  return loss_based_result_;
}

void LossBasedBweV2::Impl::SetAcknowledgedBitrate(DataRate acknowledged_bitrate) {
  if (IsValid(acknowledged_bitrate)) {
    acknowledged_bitrate_ = acknowledged_bitrate;
    CalculateInstantLowerBound();
  } else {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The acknowledged bitrate must be finite: "
                        << ToString(acknowledged_bitrate);
  }
}

void LossBasedBweV2::Impl::SetBandwidthEstimate(DataRate bandwidth_estimate) {
  if (IsValid(bandwidth_estimate)) {
    current_best_estimate_.loss_limited_bandwidth = bandwidth_estimate;
    loss_based_result_ = {bandwidth_estimate, LossBasedState::kDelayBasedEstimate};
  } else {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The bandwidth estimate must be finite: "
                        << ToString(bandwidth_estimate);
  }
}

void LossBasedBweV2::Impl::SetMinMaxBitrate(DataRate min_bitrate,
                                      DataRate max_bitrate) {
  if (IsValid(min_bitrate)) {
    min_bitrate_ = min_bitrate;
    CalculateInstantLowerBound();
  } else {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The min bitrate must be finite: "
                        << ToString(min_bitrate);
  }

  if (IsValid(max_bitrate) || max_bitrate.raw() == kPositiveInfinity) {
    max_bitrate_ = max_bitrate;
  } else {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The max bitrate must be finite: "
                        << ToString(max_bitrate);
  }
}

void LossBasedBweV2::Impl::UpdateBandwidthEstimate(
    const std::vector<ModelPacket>& packet_results,
    DataRate delay_based_estimate,
    bool in_alr) {
  delay_based_estimate_ = delay_based_estimate;
  if (!IsEnabled()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The estimator must be enabled before it can be used.";
    return;
  }

  if (packet_results.empty()) {
    UAV_LOG_DEBUG << "[WebRTC][LossBasedBweV2] "
        << "The estimate cannot be updated without any loss statistics.";
    return;
  }

  if (!PushBackObservation(packet_results)) {
    return;
  }

  if (!IsValid(current_best_estimate_.loss_limited_bandwidth)) {
    if (!IsValid(delay_based_estimate)) {
      UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The delay based estimate must be finite: "
                          << ToString(delay_based_estimate);
      return;
    }
    current_best_estimate_.loss_limited_bandwidth = delay_based_estimate;
    loss_based_result_ = {delay_based_estimate, LossBasedState::kDelayBasedEstimate};
  }

  ChannelParameters best_candidate = current_best_estimate_;
  double objective_max = std::numeric_limits<double>::lowest();
  for (ChannelParameters candidate : GetCandidates(in_alr)) {
    NewtonsMethodUpdate(candidate);

    const double candidate_objective = GetObjective(candidate);
    if (candidate_objective > objective_max ||
        (candidate_objective == objective_max &&
         candidate.loss_limited_bandwidth >
             best_candidate.loss_limited_bandwidth)) {
      // Select the candidate with the highest objective or the one with the
      // same objective but higher bandwidth if they are tied.
      objective_max = candidate_objective;
      best_candidate = candidate;
    }
  }
  if (best_candidate.loss_limited_bandwidth <
      current_best_estimate_.loss_limited_bandwidth) {
    last_time_estimate_reduced_ = last_send_time_most_recent_observation_;
  }

  // Do not increase the estimate if the average loss is greater than current
  // inherent loss.
  if (average_reported_loss_ratio_ > best_candidate.inherent_loss &&
      config_->not_increase_if_inherent_loss_less_than_average_loss &&
      current_best_estimate_.loss_limited_bandwidth <
          best_candidate.loss_limited_bandwidth) {
    best_candidate.loss_limited_bandwidth =
        current_best_estimate_.loss_limited_bandwidth;
  }

  if (IsInLossLimitedState()) {
    // Bound the estimate increase if:
    // 1. The estimate has been increased for less than
    // `delayed_increase_window` ago, and
    // 2. The best candidate is greater than bandwidth_limit_in_current_window.
    if (recovering_after_loss_timestamp_.IsFinite() &&
        recovering_after_loss_timestamp_ + TimeDelta::Micros(config_->delayed_increase_window.count()) >
            last_send_time_most_recent_observation_ &&
        best_candidate.loss_limited_bandwidth >
            bandwidth_limit_in_current_window_) {
      best_candidate.loss_limited_bandwidth =
          bandwidth_limit_in_current_window_;
    }

    bool increasing_when_loss_limited = IsEstimateIncreasingWhenLossLimited(
        /*old_estimate=*/current_best_estimate_.loss_limited_bandwidth,
        /*new_estimate=*/best_candidate.loss_limited_bandwidth);
    // Bound the best candidate by the acked bitrate.
    if (increasing_when_loss_limited && IsValid(acknowledged_bitrate_)) {
      double rampup_factor = config_->bandwidth_rampup_upper_bound_factor;
      if (IsValid(last_hold_info_.rate) &&
          acknowledged_bitrate_ <
              config_->bandwidth_rampup_hold_threshold * last_hold_info_.rate) {
        rampup_factor = config_->bandwidth_rampup_upper_bound_factor_in_hold;
      }

      best_candidate.loss_limited_bandwidth =
          std::max(current_best_estimate_.loss_limited_bandwidth,
                   std::min(best_candidate.loss_limited_bandwidth,
                            rampup_factor * (*acknowledged_bitrate_)));
      // Increase current estimate by at least 1 bit/s to make sure that the state
      // will be switched to kIncreasing, thus padding is triggered.
      if (loss_based_result_.state == LossBasedState::kDecreasing &&
          best_candidate.loss_limited_bandwidth ==
              current_best_estimate_.loss_limited_bandwidth) {
        best_candidate.loss_limited_bandwidth =
            current_best_estimate_.loss_limited_bandwidth +
            DataRate::BitsPerSec(1);
      }
    }
  }

  DataRate bounded_bandwidth_estimate = DataRate::PlusInfinity();
  if (IsValid(delay_based_estimate_)) {
    bounded_bandwidth_estimate =
        std::max(GetInstantLowerBound(),
                 std::min({best_candidate.loss_limited_bandwidth,
                           GetInstantUpperBound(), delay_based_estimate_}));
  } else {
    bounded_bandwidth_estimate = std::max(
        GetInstantLowerBound(), std::min(best_candidate.loss_limited_bandwidth,
                                         GetInstantUpperBound()));
  }
  if (config_->bound_best_candidate &&
      bounded_bandwidth_estimate < best_candidate.loss_limited_bandwidth) {
    // If network is lossy, cap the best estimate by the instant upper bound,
    // e.g. 450kbps if loss rate is 50%.
    // Otherwise, cap the estimate by the delay-based estimate or max_bitrate.
    current_best_estimate_.loss_limited_bandwidth = bounded_bandwidth_estimate;
    current_best_estimate_.inherent_loss = 0;
  } else {
    current_best_estimate_ = best_candidate;
    if (config_->lower_bound_by_acked_rate_factor > 0.0) {
      current_best_estimate_.loss_limited_bandwidth =
          std::max(current_best_estimate_.loss_limited_bandwidth,
                   GetInstantLowerBound());
    }
  }

  if (loss_based_result_.state == LossBasedState::kDecreasing &&
      last_hold_info_.timestamp > last_send_time_most_recent_observation_ &&
      bounded_bandwidth_estimate < delay_based_estimate_) {
    // Ensure that acked rate is the lower bound of HOLD rate.
    if (config_->lower_bound_by_acked_rate_factor > 0.0) {
      last_hold_info_.rate =
          std::max(GetInstantLowerBound(), last_hold_info_.rate);
    }
    // BWE is not allowed to increase above the HOLD rate. The purpose of
    // HOLD is to not immediately ramp up BWE to a rate that may cause loss.
    loss_based_result_.bandwidth_estimate =
        std::min(last_hold_info_.rate, bounded_bandwidth_estimate);
    return;
  }

  if (IsEstimateIncreasingWhenLossLimited(
          /*old_estimate=*/loss_based_result_.bandwidth_estimate,
          /*new_estimate=*/bounded_bandwidth_estimate) &&
      CanKeepIncreasingState(bounded_bandwidth_estimate) &&
      bounded_bandwidth_estimate < delay_based_estimate_ &&
      bounded_bandwidth_estimate < max_bitrate_) {
    if (TimeDelta::Micros(config_->padding_duration.count()) > TimeDelta::Zero() &&
        bounded_bandwidth_estimate > last_padding_info_.padding_rate) {
      // Start a new padding duration.
      last_padding_info_.padding_rate = bounded_bandwidth_estimate;
      last_padding_info_.padding_timestamp =
          last_send_time_most_recent_observation_;
    }
    loss_based_result_.state = TimeDelta::Micros(config_->padding_duration.count()) > TimeDelta::Zero()
                                   ? LossBasedState::kIncreaseUsingPadding
                                   : LossBasedState::kIncreasing;
  } else if (bounded_bandwidth_estimate < delay_based_estimate_ &&
             bounded_bandwidth_estimate < max_bitrate_) {
    if (loss_based_result_.state != LossBasedState::kDecreasing &&
        config_->hold_duration_factor > 0) {
      UAV_LOG_INFO << "[WebRTC][LossBasedBweV2] " << this << " " << "Switch to HOLD. Bounded BWE: "
                       << bounded_bandwidth_estimate.kbps()
                       << ", duration: " << last_hold_info_.duration.ms()
                       << ", avg loss rate: " << average_reported_loss_ratio_;
      last_hold_info_ = {last_send_time_most_recent_observation_ +
                       last_hold_info_.duration, std::min(kMaxHoldDuration, last_hold_info_.duration *
                                             config_->hold_duration_factor), bounded_bandwidth_estimate};
    }
    last_padding_info_ = PaddingInfo();
    loss_based_result_.state = LossBasedState::kDecreasing;
  } else {
    // Reset the HOLD info if delay based estimate works to avoid getting
    // stuck in low bitrate.
    last_hold_info_ = {Timestamp::MinusInfinity(), kInitHoldDuration, DataRate::PlusInfinity()};
    last_padding_info_ = PaddingInfo();
    loss_based_result_.state = LossBasedState::kDelayBasedEstimate;
  }
  loss_based_result_.bandwidth_estimate = bounded_bandwidth_estimate;

  if (IsInLossLimitedState() &&
      (recovering_after_loss_timestamp_.IsInfinite() ||
       recovering_after_loss_timestamp_ + TimeDelta::Micros(config_->delayed_increase_window.count()) <
           last_send_time_most_recent_observation_)) {
    bandwidth_limit_in_current_window_ =
        std::max(kCongestionControllerMinBitrate,
                 current_best_estimate_.loss_limited_bandwidth *
                     config_->max_increase_factor);
    recovering_after_loss_timestamp_ = last_send_time_most_recent_observation_;
  }
}

bool LossBasedBweV2::Impl::IsEstimateIncreasingWhenLossLimited(
    DataRate old_estimate,
    DataRate new_estimate) {
  return (old_estimate < new_estimate ||
          (old_estimate == new_estimate &&
           (loss_based_result_.state == LossBasedState::kIncreasing ||
            loss_based_result_.state ==
                LossBasedState::kIncreaseUsingPadding))) &&
         IsInLossLimitedState();
}

bool LossBasedBweV2::Impl::IsConfigValid() const {
  if (!config_.has_value()) {
    return false;
  }


  const double numeric[] = {
    config_->bandwidth_rampup_upper_bound_factor, config_->bandwidth_rampup_upper_bound_factor_in_hold,
    config_->bandwidth_rampup_hold_threshold, config_->rampup_acceleration_max_factor,
    config_->higher_bandwidth_bias_factor, config_->higher_log_bandwidth_bias_factor,
    config_->inherent_loss_lower_bound, config_->loss_threshold_of_high_bandwidth_preference,
    config_->bandwidth_preference_smoothing_factor, config_->inherent_loss_upper_bound_offset,
    config_->initial_inherent_loss_estimate, config_->newton_step_size, config_->sending_rate_smoothing_factor,
    config_->instant_upper_bound_temporal_weight_factor, config_->instant_upper_bound_loss_offset,
    config_->temporal_weight_factor, config_->bandwidth_backoff_lower_bound_factor,
    config_->max_increase_factor, config_->lower_bound_by_acked_rate_factor,
    config_->hold_duration_factor, config_->median_sending_rate_factor
  };
  if (!std::all_of(std::begin(numeric), std::end(numeric), [](double v) { return std::isfinite(v); }) ||
      !std::all_of(config_->candidate_factors.begin(), config_->candidate_factors.end(), [](double v) { return std::isfinite(v); }) ||
      config_->inherent_loss_upper_bound_bandwidth_balance > static_cast<std::uint64_t>(kMaxFinite) ||
      config_->instant_upper_bound_bandwidth_balance > static_cast<std::uint64_t>(kMaxFinite) ||
      config_->bandwidth_backoff_lower_bound_factor < 0 || config_->hold_duration_factor < 0 ||
      config_->median_sending_rate_factor < 0 || config_->padding_duration.count() < 0 ||
      config_->padding_duration.count() == kPositiveInfinity ||
      config_->rampup_acceleration_maxout_time.count() == kPositiveInfinity ||
      config_->observation_duration_lower_bound.count() == kPositiveInfinity ||
      config_->delayed_increase_window.count() == kPositiveInfinity)
    return false;

  bool valid = true;

  if (config_->bandwidth_rampup_upper_bound_factor <= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The bandwidth rampup upper bound factor must be greater than 1: "
        << config_->bandwidth_rampup_upper_bound_factor;
    valid = false;
  }
  if (config_->bandwidth_rampup_upper_bound_factor_in_hold <= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The bandwidth rampup upper bound factor in hold "
                           "must be greater than 1: "
                        << config_->bandwidth_rampup_upper_bound_factor_in_hold;
    valid = false;
  }
  if (config_->bandwidth_rampup_hold_threshold < 0.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The bandwidth rampup hold threshold must"
                           "must be non-negative.: "
                        << config_->bandwidth_rampup_hold_threshold;
    valid = false;
  }
  if (config_->rampup_acceleration_max_factor < 0.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The rampup acceleration max factor must be non-negative.: "
        << config_->rampup_acceleration_max_factor;
    valid = false;
  }
  if (TimeDelta::Micros(config_->rampup_acceleration_maxout_time.count()) <= TimeDelta::Zero()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The rampup acceleration maxout time must be above zero: "
        << TimeDelta::Micros(config_->rampup_acceleration_maxout_time.count()).seconds();
    valid = false;
  }
  for (double candidate_factor : config_->candidate_factors) {
    if (candidate_factor <= 0.0) {
      UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "All candidate factors must be greater than zero: "
                          << candidate_factor;
      valid = false;
    }
  }

  // Ensure that the configuration allows generation of at least one candidate
  // other than the current estimate.
  if (!config_->append_acknowledged_rate_candidate &&
      !config_->append_delay_based_estimate_candidate &&
      !std::any_of(config_->candidate_factors.begin(), config_->candidate_factors.end(),
                      [](double cf) { return cf != 1.0; })) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The configuration does not allow generating candidates. Specify "
           "a candidate factor other than 1.0, allow the acknowledged rate "
           "to be a candidate, and/or allow the delay based estimate to be a "
           "candidate.";
    valid = false;
  }

  if (config_->higher_bandwidth_bias_factor < 0.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The higher bandwidth bias factor must be non-negative: "
        << config_->higher_bandwidth_bias_factor;
    valid = false;
  }
  if (config_->inherent_loss_lower_bound < 0.0 ||
      config_->inherent_loss_lower_bound >= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The inherent loss lower bound must be in [0, 1): "
                        << config_->inherent_loss_lower_bound;
    valid = false;
  }
  if (config_->loss_threshold_of_high_bandwidth_preference < 0.0 ||
      config_->loss_threshold_of_high_bandwidth_preference >= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The loss threshold of high bandwidth preference must be in [0, 1): "
        << config_->loss_threshold_of_high_bandwidth_preference;
    valid = false;
  }
  if (config_->bandwidth_preference_smoothing_factor <= 0.0 ||
      config_->bandwidth_preference_smoothing_factor > 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The bandwidth preference smoothing factor must be in (0, 1]: "
        << config_->bandwidth_preference_smoothing_factor;
    valid = false;
  }
  if (DataRate::BitsPerSec(config_->inherent_loss_upper_bound_bandwidth_balance) <=
      DataRate::Zero()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The inherent loss upper bound bandwidth balance "
           "must be positive: "
        << ToString(DataRate::BitsPerSec(config_->inherent_loss_upper_bound_bandwidth_balance));
    valid = false;
  }
  if (config_->inherent_loss_upper_bound_offset <
          config_->inherent_loss_lower_bound ||
      config_->inherent_loss_upper_bound_offset >= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The inherent loss upper bound must be greater "
                           "than or equal to the inherent "
                           "loss lower bound, which is "
                        << config_->inherent_loss_lower_bound
                        << ", and less than 1: "
                        << config_->inherent_loss_upper_bound_offset;
    valid = false;
  }
  if (config_->initial_inherent_loss_estimate < 0.0 ||
      config_->initial_inherent_loss_estimate >= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The initial inherent loss estimate must be in [0, 1): "
        << config_->initial_inherent_loss_estimate;
    valid = false;
  }
  if (config_->newton_iterations <= 0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The number of Newton iterations must be positive: "
                        << config_->newton_iterations;
    valid = false;
  }
  if (config_->newton_step_size <= 0.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The Newton step size must be positive: "
                        << config_->newton_step_size;
    valid = false;
  }
  if (TimeDelta::Micros(config_->observation_duration_lower_bound.count()) <= TimeDelta::Zero()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The observation duration lower bound must be positive: "
        << ToString(TimeDelta::Micros(config_->observation_duration_lower_bound.count()));
    valid = false;
  }
  if (config_->observation_window_size < 2) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The observation window size must be at least 2: "
                        << config_->observation_window_size;
    valid = false;
  }
  if (config_->sending_rate_smoothing_factor < 0.0 ||
      config_->sending_rate_smoothing_factor >= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The sending rate smoothing factor must be in [0, 1): "
        << config_->sending_rate_smoothing_factor;
    valid = false;
  }
  if (config_->instant_upper_bound_temporal_weight_factor <= 0.0 ||
      config_->instant_upper_bound_temporal_weight_factor > 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The instant upper bound temporal weight factor must be in (0, 1]"
        << config_->instant_upper_bound_temporal_weight_factor;
    valid = false;
  }
  if (DataRate::BitsPerSec(config_->instant_upper_bound_bandwidth_balance) <= DataRate::Zero()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The instant upper bound bandwidth balance must be positive: "
        << ToString(DataRate::BitsPerSec(config_->instant_upper_bound_bandwidth_balance));
    valid = false;
  }
  if (config_->instant_upper_bound_loss_offset < 0.0 ||
      config_->instant_upper_bound_loss_offset >= 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The instant upper bound loss offset must be in [0, 1): "
        << config_->instant_upper_bound_loss_offset;
    valid = false;
  }
  if (config_->temporal_weight_factor <= 0.0 ||
      config_->temporal_weight_factor > 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The temporal weight factor must be in (0, 1]: "
                        << config_->temporal_weight_factor;
    valid = false;
  }
  if (config_->bandwidth_backoff_lower_bound_factor > 1.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The bandwidth backoff lower bound factor must not be greater than "
           "1: "
        << config_->bandwidth_backoff_lower_bound_factor;
    valid = false;
  }
  if (config_->max_increase_factor <= 0.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The maximum increase factor must be positive: "
                        << config_->max_increase_factor;
    valid = false;
  }
  if (TimeDelta::Micros(config_->delayed_increase_window.count()) <= TimeDelta::Zero()) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The delayed increase window must be positive: "
                        << TimeDelta::Micros(config_->delayed_increase_window.count()).ms();
    valid = false;
  }
  if (config_->min_num_observations <= 0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] " << "The min number of observations must be positive: "
                        << config_->min_num_observations;
    valid = false;
  }
  if (config_->lower_bound_by_acked_rate_factor < 0.0) {
    UAV_LOG_WARN << "[WebRTC][LossBasedBweV2] "
        << "The estimate lower bound by acknowledged rate factor must be "
           "non-negative: "
        << config_->lower_bound_by_acked_rate_factor;
    valid = false;
  }
  return valid;
}

void LossBasedBweV2::Impl::UpdateAverageReportedLossRatio() {
  average_reported_loss_ratio_ =
      (config_->use_byte_loss_rate ? CalculateAverageReportedByteLossRatio()
                                   : CalculateAverageReportedPacketLossRatio());
}

double LossBasedBweV2::Impl::CalculateAverageReportedPacketLossRatio() const {
  if (num_observations_ <= 0) {
    return 0.0;
  }

  double num_packets = 0;
  double num_lost_packets = 0;
  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double instant_temporal_weight =
        instant_upper_bound_temporal_weights_[(num_observations_ - 1) -
                                              observation.id];
    num_packets += instant_temporal_weight * observation.num_packets;
    num_lost_packets += instant_temporal_weight * observation.num_lost_packets;
  }

  return num_lost_packets / num_packets;
}

double LossBasedBweV2::Impl::CalculateAverageReportedByteLossRatio() const {
  if (num_observations_ <= 0) {
    return 0.0;
  }

  DataSize total_bytes = DataSize::Zero();
  DataSize lost_bytes = DataSize::Zero();
  double min_loss_rate = 1.0;
  double max_loss_rate = 0.0;
  DataSize min_lost_bytes = DataSize::Zero();
  DataSize max_lost_bytes = DataSize::Zero();
  DataSize min_bytes_received = DataSize::Zero();
  DataSize max_bytes_received = DataSize::Zero();
  DataRate send_rate_of_max_loss_observation = DataRate::Zero();
  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double instant_temporal_weight =
        instant_upper_bound_temporal_weights_[(num_observations_ - 1) -
                                              observation.id];
    total_bytes += instant_temporal_weight * observation.size;
    lost_bytes += instant_temporal_weight * observation.lost_size;

    double loss_rate = !observation.size.IsZero()
                           ? observation.lost_size / observation.size
                           : 0.0;
    if (num_observations_ > 3) {
      if (loss_rate > max_loss_rate) {
        max_loss_rate = loss_rate;
        max_lost_bytes = instant_temporal_weight * observation.lost_size;
        max_bytes_received = instant_temporal_weight * observation.size;
        send_rate_of_max_loss_observation = observation.sending_rate;
      }
      if (loss_rate < min_loss_rate) {
        min_loss_rate = loss_rate;
        min_lost_bytes = instant_temporal_weight * observation.lost_size;
        min_bytes_received = instant_temporal_weight * observation.size;
      }
    }
  }
  if (GetMedianSendingRate() * config_->median_sending_rate_factor <=
      send_rate_of_max_loss_observation) {
    // If the median sending rate is less than half of the sending rate of the
    // observation with max loss rate, i.e. we suddenly send a lot of data, then
    // the loss rate might not be due to a spike.
    return lost_bytes / total_bytes;
  }

  if (total_bytes == max_bytes_received + min_bytes_received) {
    // It could happen if the observation window was 2.
    return lost_bytes / total_bytes;
  }

  return (lost_bytes - min_lost_bytes - max_lost_bytes) /
         (total_bytes - max_bytes_received - min_bytes_received);
}

DataRate LossBasedBweV2::Impl::GetCandidateBandwidthUpperBound() const {
  DataRate candidate_bandwidth_upper_bound = max_bitrate_;
  if (IsInLossLimitedState() && IsValid(bandwidth_limit_in_current_window_)) {
    candidate_bandwidth_upper_bound = bandwidth_limit_in_current_window_;
  }

  if (!acknowledged_bitrate_.has_value())
    return candidate_bandwidth_upper_bound;

  if (config_->rampup_acceleration_max_factor > 0.0) {
    const TimeDelta time_since_bandwidth_reduced = std::min(
        TimeDelta::Micros(config_->rampup_acceleration_maxout_time.count()),
        std::max(TimeDelta::Zero(), last_send_time_most_recent_observation_ -
                                        last_time_estimate_reduced_));
    const double rampup_acceleration = config_->rampup_acceleration_max_factor *
                                       time_since_bandwidth_reduced /
                                       TimeDelta::Micros(config_->rampup_acceleration_maxout_time.count());

    candidate_bandwidth_upper_bound +=
        rampup_acceleration * (*acknowledged_bitrate_);
  }
  return candidate_bandwidth_upper_bound;
}

std::vector<LossBasedBweV2::Impl::ChannelParameters> LossBasedBweV2::Impl::GetCandidates(
    bool in_alr) const {
  ChannelParameters best_estimate = current_best_estimate_;
  std::vector<DataRate> bandwidths;
  for (double candidate_factor : config_->candidate_factors) {
    bandwidths.push_back(candidate_factor *
                         best_estimate.loss_limited_bandwidth);
  }

  if (acknowledged_bitrate_.has_value() &&
      config_->append_acknowledged_rate_candidate) {
    if (!(config_->not_use_acked_rate_in_alr && in_alr) ||
        (TimeDelta::Micros(config_->padding_duration.count()) > TimeDelta::Zero() &&
         last_padding_info_.padding_timestamp + TimeDelta::Micros(config_->padding_duration.count()) >=
             last_send_time_most_recent_observation_)) {
      bandwidths.push_back(*acknowledged_bitrate_ *
                           config_->bandwidth_backoff_lower_bound_factor);
    }
  }

  if (IsValid(delay_based_estimate_) &&
      config_->append_delay_based_estimate_candidate) {
    if (delay_based_estimate_ > best_estimate.loss_limited_bandwidth) {
      bandwidths.push_back(delay_based_estimate_);
    }
  }

  if (in_alr && config_->append_upper_bound_candidate_in_alr &&
      best_estimate.loss_limited_bandwidth > GetInstantUpperBound()) {
    bandwidths.push_back(GetInstantUpperBound());
  }

  const DataRate candidate_bandwidth_upper_bound =
      GetCandidateBandwidthUpperBound();

  std::vector<ChannelParameters> candidates;
  candidates.resize(bandwidths.size());
  for (size_t i = 0; i < bandwidths.size(); ++i) {
    ChannelParameters candidate = best_estimate;
    candidate.loss_limited_bandwidth =
        std::min(bandwidths[i], std::max(best_estimate.loss_limited_bandwidth,
                                         candidate_bandwidth_upper_bound));
    candidate.inherent_loss = GetFeasibleInherentLoss(candidate);
    candidates[i] = candidate;
  }
  return candidates;
}

LossBasedBweV2::Impl::Derivatives LossBasedBweV2::Impl::GetDerivatives(
    const ChannelParameters& channel_parameters) const {
  Derivatives derivatives;

  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double loss_probability = GetLossProbability(
        channel_parameters.inherent_loss,
        channel_parameters.loss_limited_bandwidth, observation.sending_rate);

    double temporal_weight =
        temporal_weights_[(num_observations_ - 1) - observation.id];
    if (config_->use_byte_loss_rate) {
      derivatives.first +=
          temporal_weight *
          ((ToKiloBytes(observation.lost_size) / loss_probability) -
           (ToKiloBytes(observation.size - observation.lost_size) /
            (1.0 - loss_probability)));
      derivatives.second -=
          temporal_weight *
          ((ToKiloBytes(observation.lost_size) /
            std::pow(loss_probability, 2)) +
           (ToKiloBytes(observation.size - observation.lost_size) /
            std::pow(1.0 - loss_probability, 2)));
    } else {
      derivatives.first +=
          temporal_weight *
          ((observation.num_lost_packets / loss_probability) -
           (observation.num_received_packets / (1.0 - loss_probability)));
      derivatives.second -=
          temporal_weight *
          ((observation.num_lost_packets / std::pow(loss_probability, 2)) +
           (observation.num_received_packets /
            std::pow(1.0 - loss_probability, 2)));
    }
  }

  if (derivatives.second >= 0.0) {
    UAV_LOG_ERROR << "[WebRTC][LossBasedBweV2] " << "The second derivative is mathematically guaranteed "
                         "to be negative but is "
                      << derivatives.second << ".";
    derivatives.second = -1.0e-6;
  }

  return derivatives;
}

double LossBasedBweV2::Impl::GetFeasibleInherentLoss(
    const ChannelParameters& channel_parameters) const {
  return std::min(
      std::max(channel_parameters.inherent_loss,
               config_->inherent_loss_lower_bound),
      GetInherentLossUpperBound(channel_parameters.loss_limited_bandwidth));
}

double LossBasedBweV2::Impl::GetInherentLossUpperBound(DataRate bandwidth) const {
  if (bandwidth.IsZero()) {
    return 1.0;
  }

  double inherent_loss_upper_bound =
      config_->inherent_loss_upper_bound_offset +
      DataRate::BitsPerSec(config_->inherent_loss_upper_bound_bandwidth_balance) / bandwidth;
  return std::min(inherent_loss_upper_bound, 1.0);
}

double LossBasedBweV2::Impl::AdjustBiasFactor(double loss_rate,
                                        double bias_factor) const {
  return bias_factor *
         (config_->loss_threshold_of_high_bandwidth_preference - loss_rate) /
         (config_->bandwidth_preference_smoothing_factor +
          std::abs(config_->loss_threshold_of_high_bandwidth_preference -
                   loss_rate));
}

double LossBasedBweV2::Impl::GetHighBandwidthBias(DataRate bandwidth) const {
  if (IsValid(bandwidth)) {
    return AdjustBiasFactor(average_reported_loss_ratio_,
                            config_->higher_bandwidth_bias_factor) *
               bandwidth.kbps() +
           AdjustBiasFactor(average_reported_loss_ratio_,
                            config_->higher_log_bandwidth_bias_factor) *
               std::log(1.0 + bandwidth.kbps());
  }
  return 0.0;
}

double LossBasedBweV2::Impl::GetObjective(
    const ChannelParameters& channel_parameters) const {
  double objective = 0.0;

  const double high_bandwidth_bias =
      GetHighBandwidthBias(channel_parameters.loss_limited_bandwidth);

  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double loss_probability = GetLossProbability(
        channel_parameters.inherent_loss,
        channel_parameters.loss_limited_bandwidth, observation.sending_rate);

    double temporal_weight =
        temporal_weights_[(num_observations_ - 1) - observation.id];
    if (config_->use_byte_loss_rate) {
      objective +=
          temporal_weight *
          ((ToKiloBytes(observation.lost_size) * std::log(loss_probability)) +
           (ToKiloBytes(observation.size - observation.lost_size) *
            std::log(1.0 - loss_probability)));
      objective +=
          temporal_weight * high_bandwidth_bias * ToKiloBytes(observation.size);
    } else {
      objective +=
          temporal_weight *
          ((observation.num_lost_packets * std::log(loss_probability)) +
           (observation.num_received_packets *
            std::log(1.0 - loss_probability)));
      objective +=
          temporal_weight * high_bandwidth_bias * observation.num_packets;
    }
  }

  return objective;
}

DataRate LossBasedBweV2::Impl::GetSendingRate(
    DataRate instantaneous_sending_rate) const {
  if (num_observations_ <= 0) {
    return instantaneous_sending_rate;
  }

  const int most_recent_observation_idx =
      (num_observations_ - 1) % config_->observation_window_size;
  const Observation& most_recent_observation =
      observations_[most_recent_observation_idx];
  DataRate sending_rate_previous_observation =
      most_recent_observation.sending_rate;

  return config_->sending_rate_smoothing_factor *
             sending_rate_previous_observation +
         (1.0 - config_->sending_rate_smoothing_factor) *
             instantaneous_sending_rate;
}

DataRate LossBasedBweV2::Impl::GetInstantUpperBound() const {
  return cached_instant_upper_bound_.value_or(max_bitrate_);
}

void LossBasedBweV2::Impl::CalculateInstantUpperBound() {
  DataRate instant_limit = max_bitrate_;
  if (average_reported_loss_ratio_ > config_->instant_upper_bound_loss_offset) {
    instant_limit = DataRate::BitsPerSec(config_->instant_upper_bound_bandwidth_balance) /
                    (average_reported_loss_ratio_ -
                     config_->instant_upper_bound_loss_offset);
  }

  cached_instant_upper_bound_ = instant_limit;
}

DataRate LossBasedBweV2::Impl::GetInstantLowerBound() const {
  return cached_instant_lower_bound_.value_or(DataRate::Zero());
}

void LossBasedBweV2::Impl::CalculateInstantLowerBound() {
  if (!config_) return;
  DataRate instance_lower_bound = DataRate::Zero();
  if (IsValid(acknowledged_bitrate_) &&
      config_->lower_bound_by_acked_rate_factor > 0.0) {
    instance_lower_bound = config_->lower_bound_by_acked_rate_factor *
                           acknowledged_bitrate_.value();
  }

  if (IsValid(min_bitrate_)) {
    instance_lower_bound = std::max(instance_lower_bound, min_bitrate_);
  }
  cached_instant_lower_bound_ = instance_lower_bound;
}

void LossBasedBweV2::Impl::CalculateTemporalWeights() {
  for (int i = 0; i < config_->observation_window_size; ++i) {
    temporal_weights_[i] = std::pow(config_->temporal_weight_factor, i);
    instant_upper_bound_temporal_weights_[i] =
        std::pow(config_->instant_upper_bound_temporal_weight_factor, i);
  }
}

void LossBasedBweV2::Impl::NewtonsMethodUpdate(
    ChannelParameters& channel_parameters) const {
  if (num_observations_ <= 0) {
    return;
  }

  for (int i = 0; i < config_->newton_iterations; ++i) {
    const Derivatives derivatives = GetDerivatives(channel_parameters);
    channel_parameters.inherent_loss -=
        config_->newton_step_size * derivatives.first / derivatives.second;
    channel_parameters.inherent_loss =
        GetFeasibleInherentLoss(channel_parameters);
  }
}

bool LossBasedBweV2::Impl::PushBackObservation(
    const std::vector<ModelPacket>& packet_results) {
  if (packet_results.empty()) {
    return false;
  }

  partial_observation_.num_packets += packet_results.size();
  Timestamp last_send_time = Timestamp::MinusInfinity();
  Timestamp first_send_time = Timestamp::PlusInfinity();
  for (const ModelPacket& packet : packet_results) {
    if (packet.IsReceived()) {
      partial_observation_.lost_packets.erase(
          packet.sent_packet.sequence_number);
    } else {
      partial_observation_.lost_packets.emplace(
          packet.sent_packet.sequence_number, packet.sent_packet.size);
    }
    partial_observation_.size += packet.sent_packet.size;
    last_send_time = std::max(last_send_time, packet.sent_packet.send_time);
    first_send_time = std::min(first_send_time, packet.sent_packet.send_time);
  }

  // This is the first packet report we have received.
  if (!IsValid(last_send_time_most_recent_observation_)) {
    last_send_time_most_recent_observation_ = first_send_time;
  }

  const TimeDelta observation_duration =
      last_send_time - last_send_time_most_recent_observation_;
  // Too small to be meaningful.
  // To consider: what if it is too long?, i.e. we did not receive any packets
  // for a long time, then all the packets we received are too old.
  if (observation_duration <= TimeDelta::Zero() ||
      observation_duration < TimeDelta::Micros(config_->observation_duration_lower_bound.count())) {
    return false;
  }

  last_send_time_most_recent_observation_ = last_send_time;

  Observation observation;
  observation.num_packets = partial_observation_.num_packets;
  observation.num_lost_packets = partial_observation_.lost_packets.size();
  observation.num_received_packets =
      observation.num_packets - observation.num_lost_packets;
  observation.sending_rate =
      GetSendingRate(partial_observation_.size / observation_duration);
  for (auto const& [key, packet_size] : partial_observation_.lost_packets) {
    observation.lost_size += packet_size;
  }
  observation.size = partial_observation_.size;
  observation.id = num_observations_++;
  observations_[observation.id % config_->observation_window_size] =
      observation;

  partial_observation_ = PartialObservation();
  UpdateAverageReportedLossRatio();
  CalculateInstantUpperBound();
  return true;
}

bool LossBasedBweV2::Impl::IsInLossLimitedState() const {
  return loss_based_result_.state != LossBasedState::kDelayBasedEstimate;
}

bool LossBasedBweV2::Impl::CanKeepIncreasingState(DataRate estimate) const {
  if (TimeDelta::Micros(config_->padding_duration.count()) == TimeDelta::Zero() ||
      loss_based_result_.state != LossBasedState::kIncreaseUsingPadding)
    return true;

  // Keep using the kIncreaseUsingPadding if either the state has been
  // kIncreaseUsingPadding for less than kPaddingDuration or the estimate
  // increases.
  return last_padding_info_.padding_timestamp + TimeDelta::Micros(config_->padding_duration.count()) >=
             last_send_time_most_recent_observation_ ||
         last_padding_info_.padding_rate < estimate;
}

DataRate LossBasedBweV2::Impl::GetMedianSendingRate() const {
  std::vector<DataRate> sending_rates;
  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized() || !IsValid(observation.sending_rate) ||
        observation.sending_rate.IsZero()) {
      continue;
    }
    sending_rates.push_back(observation.sending_rate);
  }
  if (sending_rates.empty()) {
    return DataRate::Zero();
  }
  std::sort(sending_rates.begin(), sending_rates.end());
  if (sending_rates.size() % 2 == 0) {
    return (sending_rates[sending_rates.size() / 2 - 1] +
            sending_rates[sending_rates.size() / 2]) /
           2;
  }
  return sending_rates[sending_rates.size() / 2];
}


LossBasedBweV2::LossBasedBweV2() : LossBasedBweV2(Config{}) {}
LossBasedBweV2::LossBasedBweV2(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
LossBasedBweV2::~LossBasedBweV2() = default;
bool LossBasedBweV2::IsEnabled() const { return impl_->IsEnabled(); }
bool LossBasedBweV2::IsReady() const { return impl_->IsReady(); }
bool LossBasedBweV2::ReadyToUseInStartPhase() const { return impl_->ReadyToUseInStartPhase(); }
bool LossBasedBweV2::UseInStartPhase() const { return impl_->UseInStartPhase(); }
LossBasedBweV2::Result LossBasedBweV2::GetLossBasedResult() const
{
    const auto result = impl_->GetLossBasedResult();
    return {publicRate(result.bandwidth_estimate), result.state};
}
void LossBasedBweV2::SetAcknowledgedBitrate(std::uint64_t bps) { impl_->SetAcknowledgedBitrate(checkedRate(bps)); }
void LossBasedBweV2::SetMinMaxBitrate(std::uint64_t min, std::optional<std::uint64_t> max)
{
    const auto minimum = checkedRate(min);
    const auto maximum = max ? checkedRate(*max) : DataRate::PlusInfinity();
    if (maximum < minimum) throw std::invalid_argument("loss BWE minimum exceeds maximum bitrate");
    impl_->SetMinMaxBitrate(minimum, maximum);
    // An explicit nullopt removes the configured maximum in this C++ API.
}
void LossBasedBweV2::SetBandwidthEstimate(std::uint64_t bps) { impl_->SetBandwidthEstimate(checkedRate(bps)); }
void LossBasedBweV2::UpdateBandwidthEstimate(const std::vector<PacketResult>& packets,
    std::optional<std::uint64_t> delay_bps, bool in_alr)
{
    const auto delay = delay_bps ? checkedRate(*delay_bps) : DataRate::PlusInfinity();
    if (!IsEnabled() || packets.empty())
    {
        impl_->UpdateBandwidthEstimate({}, delay, in_alr);
        return;
    }
    auto remaining_bytes = static_cast<std::uint64_t>(kMaxFinite - impl_->partial_observation_.size.bytes());
    if (packets.size() > static_cast<std::uint64_t>(kMaxFinite - impl_->partial_observation_.num_packets) ||
        impl_->num_observations_ == kMaxFinite)
        throw std::overflow_error("too many loss BWE feedback records");
    std::vector<ModelPacket> converted;
    converted.reserve(packets.size());
    for (const auto& packet : packets)
    {
        if (!packet.sent_packet.send_time || packet.sent_packet.send_time->time_since_epoch() < TransportTime::duration::zero() ||
            (packet.receive_time && packet.receive_time->time_since_epoch() < TransportTime::duration::zero()) ||
            packet.sent_packet.sequence_number < 0)
            throw std::invalid_argument("loss BWE requires valid send history and packet timestamps");
        if (packet.sent_packet.size > remaining_bytes)
            throw std::overflow_error("loss BWE observation byte count exceeds int64 range");
        remaining_bytes -= packet.sent_packet.size;
        const auto send_us = std::chrono::duration_cast<std::chrono::microseconds>(packet.sent_packet.send_time->time_since_epoch()).count();
        converted.push_back({{Timestamp::Micros(send_us), DataSize::Bytes(static_cast<std::int64_t>(packet.sent_packet.size)),
            packet.sent_packet.sequence_number}, packet.IsReceived()});
    }
    const auto previous = impl_->loss_based_result_;
    impl_->UpdateBandwidthEstimate(converted, delay, in_alr);
    const auto& result = impl_->loss_based_result_;
    if (previous.state != result.state || result.bandwidth_estimate < previous.bandwidth_estimate)
        UAV_LOG_DEBUG << "[WebRTC][LossBasedBweV2] state=" << static_cast<int>(result.state)
            << " estimate_bps=" << ToString(result.bandwidth_estimate)
            << " average_loss=" << impl_->average_reported_loss_ratio_
            << " inherent_loss=" << impl_->current_best_estimate_.inherent_loss;
}
LossBasedBweV2::Stats LossBasedBweV2::Snapshot() const
{
    const auto& model = *impl_;
    Stats stats;
    stats.observations = model.num_observations_;
    stats.partial_packets = model.partial_observation_.num_packets;
    stats.pending_lost_packets = model.partial_observation_.lost_packets.size();
    stats.average_loss_ratio = model.average_reported_loss_ratio_;
    stats.inherent_loss = model.current_best_estimate_.inherent_loss;
    stats.model_bandwidth = publicRate(model.current_best_estimate_.loss_limited_bandwidth);
    stats.instant_lower_bound = model.cached_instant_lower_bound_ ? publicRate(*model.cached_instant_lower_bound_) : std::nullopt;
    stats.instant_upper_bound = model.cached_instant_upper_bound_ ? publicRate(*model.cached_instant_upper_bound_) : std::nullopt;
    stats.hold_duration = std::chrono::microseconds(model.last_hold_info_.duration.us());
    if (model.num_observations_ > 0)
        stats.sending_rate = publicRate(model.observations_[(model.num_observations_ - 1) % model.config_->observation_window_size].sending_rate);
    return stats;
}

} // namespace webrtc
