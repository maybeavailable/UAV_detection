#!/usr/bin/env python3
"""Replay the pinned original GoogCC and all its congestion-control algorithms.

Requires network access and C++20 (upstream std::span); product remains C++17.
Platform logging/experiments/environment are shimmed. Algorithms and integer
units are original source. The ACK factory explicitly selects classic ACK BWE,
matching this project's AcknowledgedBitrateEstimator (not robust throughput).
"""
import argparse
import base64
import concurrent.futures
from pathlib import Path
import subprocess
import tempfile
import urllib.request

REVISION = '602d5fc23caaca7728ad5f3f0e68b92274d1a38a'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--directory', type=Path, default=Path(tempfile.gettempdir()) / ('uav-goog-cc-' + REVISION))
p.add_argument('--cxx', default='c++')
p.add_argument('--offline', action='store_true', help='Use files already downloaded from this revision in --directory')
args = p.parse_args()
root = args.directory.resolve()
root.mkdir(parents=True, exist_ok=True)
algorithms = ['alr_detector', 'acknowledged_bitrate_estimator', 'bitrate_estimator', 'probe_controller',
              'probe_bitrate_estimator', 'delay_based_bwe', 'loss_based_bwe_v2', 'link_capacity_estimator',
              'inter_arrival_delta', 'trendline_estimator', 'send_side_bandwidth_estimation',
              'congestion_window_pushback_controller', 'goog_cc_network_control']
files = ['modules/congestion_controller/goog_cc/' + name + '.' + ext for name in algorithms for ext in ['h', 'cc']]
files += ['api/units/' + n + '.h' for n in ['time_delta', 'timestamp', 'data_size', 'frequency', 'data_rate']]
files += ['rtc_base/units/unit_base.h', 'api/network_state_predictor.h', 'api/transport/bandwidth_usage.h',
          'api/transport/network_control.h', 'api/transport/network_types.h', 'api/transport/network_types.cc',
          'modules/remote_bitrate_estimator/include/bwe_defines.h',
          'modules/remote_bitrate_estimator/aimd_rate_control.h', 'modules/remote_bitrate_estimator/aimd_rate_control.cc',
          'modules/congestion_controller/goog_cc/delay_increase_detector_interface.h',
          'modules/congestion_controller/goog_cc/acknowledged_bitrate_estimator_interface.h',
          'modules/pacing/interval_budget.h', 'modules/pacing/interval_budget.cc']


def download(path):
    if args.offline:
        if not (root / path.rsplit('/', 1)[-1]).is_file():
            raise FileNotFoundError(path)
        return
    for attempt in range(3):
        try:
            with urllib.request.urlopen('https://webrtc.googlesource.com/src/+/' + REVISION + '/' + path + '?format=TEXT', timeout=30) as response:
                data = base64.b64decode(response.read(), validate=True)
            (root / path.rsplit('/', 1)[-1]).write_bytes(data)
            return
        except Exception:
            if attempt == 2:
                raise


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(download, files))


def source(name):
    result = '\n'.join(line for line in (root / name).read_text().splitlines() if not line.lstrip().startswith('#')) + '\n'
    result = result.replace(' private:', ' public:')
    if name == 'goog_cc_network_control.h':
        result = result.replace('const bool ', 'bool ')  # expose experiment configuration only
    return result


prefix = r'''
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#define RTC_EXPORT
#define RTC_CHECK(...) ::webrtc::Ignore{}
#define RTC_CHECK_GT(...) ::webrtc::Ignore{}
#define RTC_CHECK_GE(...) ::webrtc::Ignore{}
#define RTC_CHECK_LE(...) ::webrtc::Ignore{}
#define RTC_CHECK_LT(...) ::webrtc::Ignore{}
#define RTC_DCHECK(...) ::webrtc::Ignore{}
#define RTC_DCHECK_GT(...) ::webrtc::Ignore{}
#define RTC_DCHECK_GE(...) ::webrtc::Ignore{}
#define RTC_DCHECK_LE(...) ::webrtc::Ignore{}
#define RTC_DCHECK_LT(...) ::webrtc::Ignore{}
#define RTC_DCHECK_EQ(...) ::webrtc::Ignore{}
#define RTC_DCHECK_NE(...) ::webrtc::Ignore{}
#define RTC_DCHECK_NOTREACHED(...) ::webrtc::Ignore{}
#define RTC_DCHECK_RUNS_SERIALIZED(...) ::webrtc::Ignore{}
#define RTC_LOG(...) ::webrtc::Ignore{}
#define RTC_HISTOGRAM_ENUMERATION(...) ((void)0)
#define RTC_HISTOGRAM_COUNTS(...) ((void)0)
#define RTC_HISTOGRAMS_COUNTS_100000(...) ((void)0)
#define RTC_HISTOGRAM_COUNTS_10000(...) ((void)0)
namespace webrtc {
inline int reference_scenario=0;
inline int64_t reference_now_us=1000000000;
struct Ignore {template<class T> constexpr Ignore& operator<<(const T&){return *this;}};
template<class T,class U> constexpr T dchecked_cast(U v){return static_cast<T>(v);}
template<class T,class U> constexpr T saturated_cast(U v){return static_cast<long double>(v)>=static_cast<long double>(std::numeric_limits<T>::max())?std::numeric_limits<T>::max():(v<0?T(0):static_cast<T>(v));}
template<class T,class U> constexpr T DivideRoundToNearest(T v,U d){return v/d+(v%d>=d/2)-(v%d<=-d/2);}
template<class T,class L,class H> T SafeClamp(T v,L l,H h){return std::clamp(v,T(l),T(h));}
struct FieldTrialsView {
 std::string Lookup(std::string_view)const{return {};}
 bool IsEnabled(std::string_view key)const{return reference_scenario==3&&key=="WebRTC-AddPacingToCongestionWindowPushback";}
 bool IsDisabled(std::string_view)const{return false;}
};
struct FieldTrialFlag {bool value;explicit FieldTrialFlag(const char*):value(false){}FieldTrialFlag(const char*,bool v):value(v){}bool Get()const{return value;}operator bool()const{return value;}};
template<class T> struct FieldTrialParameter {
 T value;FieldTrialParameter(const char*,T v):value(v){}T Get()const{return value;}operator T()const{return value;}
 const T* operator->()const{return &value;}FieldTrialParameter& operator=(T v){value=v;return *this;}
};
template<class T> struct FieldTrialConstrained:FieldTrialParameter<T>{FieldTrialConstrained(const char* k,T v,T,T):FieldTrialParameter<T>(k,v){}};
template<class T> struct FieldTrialOptional {
 std::optional<T> value;FieldTrialOptional(const char*,T v):value(v){}explicit operator bool()const{return value.has_value();}
 T Value()const{return *value;}const std::optional<T>& GetOptional()const{return value;}
 FieldTrialOptional& operator=(std::optional<T> v){value=v;return *this;}
};
template<class T> struct FieldTrialList {std::vector<T> value;FieldTrialList(const char*,std::initializer_list<T> v):value(v){}std::vector<T> Get()const{return value;}};
inline void ParseFieldTrial(std::initializer_list<void*>,const std::string&){}
struct StructParametersParser {template<class... T>static std::unique_ptr<StructParametersParser>Create(T&&...){return std::make_unique<StructParametersParser>();}void Parse(const std::string&){}std::string Encode(){return {};}};
struct RaceChecker{};
struct InterArrival{};
struct RtcEventLog {template<class T>void Log(T&&){}};
#define REF_EVENT(N) struct N {template<class... T>explicit N(T&&...){}};
REF_EVENT(RtcEventBweUpdateDelayBased) REF_EVENT(RtcEventBweUpdateLossBased) REF_EVENT(RtcEventAlrState)
REF_EVENT(RtcEventProbeClusterCreated) REF_EVENT(RtcEventProbeResultFailure) REF_EVENT(RtcEventProbeResultSuccess) REF_EVENT(RtcEventRemoteEstimate)
enum class ProbeFailureReason {kInvalidSendReceiveInterval,kInvalidSendReceiveRatio};
}
namespace absl {
template<class C,class F>bool c_any_of(const C& c,F f){return std::any_of(c.begin(),c.end(),f);}
template<class C>void c_sort(C& c){std::sort(c.begin(),c.end());}
}
'''
units = ''.join(source(n+'.h') for n in ['unit_base', 'time_delta', 'timestamp', 'data_size', 'frequency', 'data_rate'])
environment = r'''
namespace webrtc {
template<class T> std::string ToString(const T&){return {};}
inline std::string ToString(DataRate){return {};}
inline std::string ToString(DataSize){return {};}
inline std::string ToString(TimeDelta){return {};}
inline std::string ToString(Timestamp){return {};}
enum class EcnMarking {kNotEct,kEct0,kEct1,kCe};
struct Clock {Timestamp CurrentTime()const{return Timestamp::Micros(reference_now_us);}};
struct Environment {
 const FieldTrialsView& field_trials()const{static FieldTrialsView f;return f;}
 RtcEventLog& event_log()const{static RtcEventLog e;return e;}
 const Clock& clock()const{static Clock c;return c;}
};
struct AlrExperimentSettings {
 int alr_bandwidth_usage_percent=80,alr_start_budget_level_percent=40,alr_stop_budget_level_percent=-60;
 static constexpr const char* kScreenshareProbingBweExperimentName="screenshare";
 static constexpr const char* kStrictPacingAndProbingExperimentName="strict";
 static bool MaxOneFieldTrialEnabled(const FieldTrialsView&){return true;}
 static std::optional<AlrExperimentSettings>CreateFromFieldTrial(const FieldTrialsView&,const char*){return AlrExperimentSettings{};}
};
struct RateControlSettings {
 explicit RateControlSettings(const FieldTrialsView&){}
 bool UseCongestionWindow()const{return reference_scenario!=1;}
 bool UseCongestionWindowPushback()const{return reference_scenario!=1;}
 bool UseCongestionWindowDropFrameOnly()const{return reference_scenario!=2;}
 int64_t GetCongestionWindowAdditionalTimeMs()const{return 350;}
 uint32_t CongestionWindowMinPushbackTargetBitrateBps()const{return 30000;}
 std::optional<DataSize> CongestionWindowInitialDataWindow()const{return {};}
};
}
'''
headers = ['bandwidth_usage', 'network_types', 'network_control', 'network_state_predictor',
           'bwe_defines', 'interval_budget', 'link_capacity_estimator', 'aimd_rate_control',
           'inter_arrival_delta', 'delay_increase_detector_interface', 'trendline_estimator',
           'delay_based_bwe', 'bitrate_estimator', 'acknowledged_bitrate_estimator_interface',
           'acknowledged_bitrate_estimator', 'alr_detector', 'probe_controller', 'probe_bitrate_estimator',
           'loss_based_bwe_v2', 'send_side_bandwidth_estimation', 'congestion_window_pushback_controller', 'goog_cc_network_control']
(root/'upstream_all.h').write_text(prefix + units + environment + ''.join(source(n+'.h') for n in headers))
implementations = algorithms + ['interval_budget', 'aimd_rate_control', 'network_types']
objects = []
for name in implementations:
    target = root/(name+'_reference.cpp')
    target.write_text('#include "upstream_all.h"\n'+source(name+'.cc'))
    objects.append(target)
fixture = Path(__file__).with_name('goog_cc_network_control_replay.h').resolve()
main = r'''
#include "upstream_all.h"
#define UAV_CC_UPSTREAM_REFERENCE
#include "FIXTURE"
namespace webrtc {
const char kBweTypeHistogram[]="";
RateControlInput::RateControlInput(BandwidthUsage s,const std::optional<DataRate>& r):bw_state(s),estimated_throughput(r){}
RateControlInput::~RateControlInput()=default;
AcknowledgedBitrateEstimatorInterface::~AcknowledgedBitrateEstimatorInterface()=default;
std::unique_ptr<AcknowledgedBitrateEstimatorInterface>AcknowledgedBitrateEstimatorInterface::Create(const FieldTrialsView* f){return std::make_unique<AcknowledgedBitrateEstimator>(f);}
}
int main(){
 using namespace webrtc;
 for(int s=0;s<8;++s){
  reference_scenario=s;reference_now_us=1000000000;
  NetworkControllerConfig config(Environment{});config.constraints.at_time=Timestamp::Micros(reference_now_us);
  config.constraints.starting_rate=DataRate::BitsPerSec(300000);config.constraints.max_data_rate=DataRate::BitsPerSec(5000000);
  if(s==4)config.stream_based_config.requests_alr_probing=true;
  if(s==7){config.default_pacing_time_window=TimeDelta::Zero();config.stream_based_config.pacing_factor=4.0;
   config.stream_based_config.min_total_allocated_bitrate=DataRate::BitsPerSec(50000);config.stream_based_config.max_padding_rate=DataRate::BitsPerSec(100000);}
  GoogCcNetworkController c(config,{});
  if(s==4){c.safe_reset_on_route_change_.value=true;c.safe_reset_acknowledged_rate_.value=true;}
  if(s==5){c.use_min_allocatable_as_lower_bound_=false;c.limit_pacingfactor_by_upper_link_capacity_estimate_=true;}
  if(s==6)c.bandwidth_estimation_.loss_based_bandwidth_estimator_v2_->config_.reset();
  auto result=goog_cc_replay::run(s,c);
  std::cout<<"googcc "<<s<<' '<<result.hash<<' '<<result.updates<<' '<<result.targets<<' '<<result.probes<<'\n';
 }
}
'''.replace('FIXTURE',str(fixture))
(root/'main.cpp').write_text(main)
subprocess.run([args.cxx, '-std=c++20', '-O0', *map(str, objects), str(root/'main.cpp'), '-o', str(root/'reference')], check=True)
subprocess.run([str(root/'reference')], check=True)
