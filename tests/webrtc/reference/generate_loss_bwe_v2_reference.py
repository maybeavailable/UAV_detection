#!/usr/bin/env python3
"""Reproduce LossBasedBweV2 golden values from pinned, original libwebrtc.

Requires network access and a C++20 host compiler for upstream std::span.
The production implementation remains C++17. Not run by normal CMake/CTest.
"""
import argparse
import base64
import concurrent.futures
import json
from pathlib import Path
import subprocess
import tempfile
import urllib.request

REVISION = "602d5fc23caaca7728ad5f3f0e68b92274d1a38a"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--directory", type=Path, default=Path(tempfile.gettempdir()) / ("uav-loss-reference-" + REVISION))
parser.add_argument("--cxx", default="c++")
args = parser.parse_args()
root = args.directory.resolve()
root.mkdir(parents=True, exist_ok=True)
files = [
    "rtc_base/units/unit_base.h",
    *["api/units/" + name + ".h" for name in ("time_delta", "timestamp", "data_size", "frequency", "data_rate")],
    *["modules/congestion_controller/goog_cc/loss_based_bwe_v2." + ext for ext in ("h", "cc")],
]


def download(path):
    for attempt in range(3):
        try:
            url = "https://webrtc.googlesource.com/src/+/" + REVISION + "/" + path + "?format=TEXT"
            with urllib.request.urlopen(url, timeout=30) as response:
                contents = base64.b64decode(response.read(), validate=True)
            (root / path.rsplit("/", 1)[-1]).write_bytes(contents)
            return
        except Exception:
            if attempt == 2:
                raise


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(download, files))


def source(name):
    # Only remove platform includes/guards; keep original units and algorithms.
    return "\n".join(line for line in (root / name).read_text().splitlines()
                     if not line.lstrip().startswith("#")).replace(" private:", " public:") + "\n"


prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#define RTC_EXPORT
#define RTC_CHECK(x) ((void)0)
#define RTC_DCHECK(x) ((void)0)
#define RTC_DCHECK_GE(a,b) ((void)0)
#define RTC_DCHECK_GT(a,b) ((void)0)
#define RTC_DCHECK_LE(a,b) ::webrtc::Ignore{}
#define RTC_DCHECK_LT(a,b) ((void)0)
#define RTC_LOG(x) Ignore{}
namespace webrtc {
struct Ignore {template<class T> constexpr Ignore& operator<<(const T&) {return *this;}};
template<class T,class U> constexpr T dchecked_cast(U value) {return static_cast<T>(value);}
template<class T,class U> constexpr T DivideRoundToNearest(T v,U d) {return v/d+(v%d>=d/2)-(v%d<=-d/2);}
struct FieldTrialsView {std::string Lookup(const char*)const{return {};}};
template<class T> struct FieldTrialParameter {
 T value; FieldTrialParameter(const char*,T v):value(v){} T Get()const{return value;}
};
template<class T> struct FieldTrialList {
 std::vector<T> value;FieldTrialList(const char*,std::initializer_list<T> v):value(v){}std::vector<T> Get()const{return value;}
};
inline void ParseFieldTrial(std::initializer_list<void*>,const std::string&){}
}
namespace absl {
template<class C,class F> bool c_any_of(const C& c,F f){return std::any_of(c.begin(),c.end(),f);}
template<class C> void c_sort(C& c){std::sort(c.begin(),c.end());}
}
'''
units = "".join(source(name + ".h") for name in ("unit_base", "time_delta", "timestamp", "data_size", "frequency", "data_rate"))
containers = r'''
namespace webrtc {
inline std::string ToString(DataRate r){return r.IsFinite()?std::to_string(r.bps()):"inf";}
inline std::string ToString(TimeDelta t){return t.IsFinite()?std::to_string(t.us()):"inf";}
struct PacketResult {
 struct Sent {Timestamp send_time=Timestamp::MinusInfinity();DataSize size=DataSize::Zero();int64_t sequence_number=-1;} sent_packet;
 Timestamp receive_time=Timestamp::PlusInfinity();bool IsReceived()const{return receive_time.IsFinite();}
};
constexpr DataRate kCongestionControllerMinBitrate=DataRate::BitsPerSec(5000);
}
'''
fixture = Path(__file__).with_name("loss_bwe_v2_replay.h").resolve()
main = r'''
using namespace webrtc;
std::optional<uint64_t> rate(DataRate r){return r.IsFinite()?std::optional<uint64_t>(r.bps()):std::nullopt;}
int main(){
 std::cout<<std::setprecision(17);
 for(int s=0;s<8;++s){
  LossBasedBweV2 e(nullptr);
  loss_bwe_replay::configure(s,*e.config_,[](uint64_t v){return DataRate::BitsPerSec(v);},[](int64_t v){return TimeDelta::Micros(v);});
  e.observations_.resize(e.config_->observation_window_size);
  e.temporal_weights_.resize(e.config_->observation_window_size);
  e.instant_upper_bound_temporal_weights_.resize(e.config_->observation_window_size);
  e.CalculateTemporalWeights();e.current_best_estimate_.inherent_loss=e.config_->initial_inherent_loss_estimate;
  if(s!=0&&s!=7)e.SetBandwidthEstimate(DataRate::BitsPerSec(600000));
  loss_bwe_replay::Input input(s);loss_bwe_replay::Hash hash;int counts[4]={};
  for(int i=0;i<600;++i){
   std::vector<PacketResult> packets;
   for(const auto& sample:input.packets(i)){
    PacketResult p;p.sent_packet.send_time=Timestamp::Micros(sample.send_us);p.sent_packet.size=DataSize::Bytes(sample.size);p.sent_packet.sequence_number=sample.sequence;
    if(sample.received)p.receive_time=p.sent_packet.send_time+TimeDelta::Millis(100);packets.push_back(p);
   }
   const auto ack=input.ack(i);if(i%7!=0)e.SetAcknowledgedBitrate(DataRate::BitsPerSec(ack));
   if(i%79==0)e.SetMinMaxBitrate(DataRate::BitsPerSec(i%2?20000:5000),DataRate::BitsPerSec(i%3?1600000:900000));
   if(i%137==136)e.SetBandwidthEstimate(DataRate::BitsPerSec(400000+ack%70000));
   const auto delay=input.delay(i,s);
   e.UpdateBandwidthEstimate(packets,delay?DataRate::BitsPerSec(*delay):DataRate::PlusInfinity(),(i/40)%2);
   const auto result=e.GetLossBasedResult();++counts[int(result.state)];
   hash.rate(rate(result.bandwidth_estimate));hash.mix(int(result.state));hash.mix(e.IsReady());hash.mix(e.UseInStartPhase());hash.mix(e.ReadyToUseInStartPhase());
   hash.mix(e.num_observations_);hash.mix(e.partial_observation_.num_packets);hash.mix(e.partial_observation_.lost_packets.size());
   hash.rate(rate(e.current_best_estimate_.loss_limited_bandwidth));
   std::optional<uint64_t> sending;if(e.num_observations_>0)sending=rate(e.observations_[(e.num_observations_-1)%e.config_->observation_window_size].sending_rate);
   hash.rate(sending);hash.rate(e.cached_instant_lower_bound_?rate(*e.cached_instant_lower_bound_):std::nullopt);hash.rate(e.cached_instant_upper_bound_?rate(*e.cached_instant_upper_bound_):std::nullopt);
   hash.mix(e.last_hold_info_.duration.us());
   hash.mix(std::llround(e.average_reported_loss_ratio_*1e9));hash.mix(std::llround(e.current_best_estimate_.inherent_loss*1e9));
   if(i==100||i==250||i==599)std::cout<<"point "<<s<<' '<<i<<' '<<e.average_reported_loss_ratio_<<' '<<e.current_best_estimate_.inherent_loss<<'\n';
  }
  std::cout<<"golden "<<s<<' '<<hash.value<<' '<<counts[0]<<' '<<counts[1]<<' '<<counts[2]<<' '<<counts[3]<<'\n';
 }
}
'''
(root / "reference.cpp").write_text(prefix + units + containers + source("loss_based_bwe_v2.h") +
                                    source("loss_based_bwe_v2.cc") + "\n#include " + json.dumps(str(fixture)) + "\n" + main)
subprocess.run([args.cxx, "-std=c++20", "-O0", str(root / "reference.cpp"), "-o", str(root / "reference")], check=True)
subprocess.run([str(root / "reference")], check=True)
