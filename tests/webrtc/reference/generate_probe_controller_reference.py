#!/usr/bin/env python3
"""Replay pinned original libwebrtc ProbeController using original integer units.

Requires network access and a C++20 compiler for upstream std::span.
The product and normal CMake/CTest remain C++17 and do not use this script.
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
parser.add_argument("--directory", type=Path, default=Path(tempfile.gettempdir()) / ("uav-probe-reference-" + REVISION))
parser.add_argument("--cxx", default="c++")
args = parser.parse_args()
root = args.directory.resolve()
root.mkdir(parents=True, exist_ok=True)
files = ["rtc_base/units/unit_base.h",
         *["api/units/" + name + ".h" for name in ("time_delta", "timestamp", "data_size", "frequency", "data_rate")],
         *["modules/congestion_controller/goog_cc/probe_controller." + ext for ext in ("h", "cc")]]


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
    return "\n".join(line for line in (root / name).read_text().splitlines()
                     if not line.lstrip().startswith("#")).replace(" private:", " public:") + "\n"


prefix = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
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
#define RTC_HISTOGRAM_COUNTS_10000(a,b) ((void)0)
namespace webrtc {
struct Ignore {template<class T> constexpr Ignore& operator<<(const T&){return *this;}};
template<class T,class U> constexpr T dchecked_cast(U v){return static_cast<T>(v);}
template<class T,class U> constexpr T DivideRoundToNearest(T v,U d){return v/d+(v%d>=d/2)-(v%d<=-d/2);}
struct FieldTrialsView {bool rapid=false;std::string Lookup(const char*)const{return {};}bool IsEnabled(const char*)const{return rapid;}};
template<class T> struct FieldTrialParameter {
 T value;FieldTrialParameter(const char*,T v):value(v){}T Get()const{return value;}operator T()const{return value;}
 const T* operator->()const{return &value;}FieldTrialParameter& operator=(T v){value=v;return *this;}
};
template<class T> struct FieldTrialOptional {
 std::optional<T> value;FieldTrialOptional(const char*,T v):value(v){}explicit operator bool()const{return value.has_value();}
 T Value()const{return *value;}const std::optional<T>& GetOptional()const{return value;}
 FieldTrialOptional& operator=(std::optional<T> v){value=v;return *this;}
};
inline void ParseFieldTrial(std::initializer_list<void*>,const std::string&){}
struct RtcEventLog {template<class T>void Log(T&&){}};
struct RtcEventProbeClusterCreated {template<class... T>explicit RtcEventProbeClusterCreated(T&&...){}};
}
'''
units = "".join(source(n + ".h") for n in ("unit_base", "time_delta", "timestamp", "data_size", "frequency", "data_rate"))
containers = r'''
namespace webrtc {
struct ProbeClusterConfig {
 Timestamp at_time=Timestamp::PlusInfinity();DataRate target_data_rate=DataRate::Zero();
 TimeDelta target_duration=TimeDelta::Zero(),min_probe_delta=TimeDelta::Millis(2);int32_t target_probe_count=0,id=0;
};
struct NetworkAvailability {Timestamp at_time=Timestamp::PlusInfinity();bool network_available=false;};
struct NetworkStateEstimate {DataRate link_capacity_lower=DataRate::MinusInfinity(),link_capacity_upper=DataRate::MinusInfinity();};
}
'''
main = r'''
using namespace webrtc;
std::optional<uint64_t> finiteRate(DataRate r){return r.IsFinite()?std::optional<uint64_t>(r.bps()):std::nullopt;}
int main(){
 for(int s=0;s<8;++s){
  FieldTrialsView trials;trials.rapid=probe_replay::rapidRecovery(s);ProbeController e(&trials,nullptr);
  probe_replay::configure(s,e.config_,[](int64_t us){return TimeDelta::Micros(us);},[]{return TimeDelta::PlusInfinity();});
  probe_replay::Input input(s);probe_replay::Hash hash;int states[3]={};int clusters=0;
  for(int i=0;i<1500;++i){
   const auto ev=input.event(i,s);const auto now=Timestamp::Micros(ev.at_us);std::vector<ProbeClusterConfig> probes;
   using K=probe_replay::Kind;
   switch(ev.kind){
    case K::Reset:e.Reset(now);break;
    case K::Availability:probes=e.OnNetworkAvailability({now,ev.flag});break;
    case K::Bitrates:probes=e.SetBitrates(DataRate::BitsPerSec(10000),DataRate::BitsPerSec(ev.rate),ev.maximum?DataRate::BitsPerSec(ev.maximum):DataRate::PlusInfinity(),now);break;
    case K::Allocation:probes=e.OnMaxTotalAllocatedBitrate(DataRate::BitsPerSec(ev.rate),now);break;
    case K::Estimate:probes=e.SetEstimatedBitrate(DataRate::BitsPerSec(ev.rate),static_cast<BandwidthLimitedCause>(ev.cause),now);break;
    case K::Periodic:e.EnablePeriodicAlrProbing(ev.flag);break;
    case K::Repeated:e.EnableRepeatedInitialProbing(ev.flag);break;
    case K::AlrStart:e.SetAlrStartTime(ev.flag?std::optional<Timestamp>(now-TimeDelta::Millis(500)):std::nullopt);break;
    case K::AlrEnd:e.SetAlrEndedTime(now);break;
    case K::Request:probes=e.RequestProbe(now);break;
    case K::NetworkEstimate:{NetworkStateEstimate n;if(ev.upper)n.link_capacity_upper=DataRate::BitsPerSec(*ev.upper);e.SetNetworkStateEstimate(n);break;}
    case K::Process:probes=e.Process(now);break;
   }
   hash.mix(probes.size());clusters+=probes.size();
   for(const auto&p:probes){hash.mix(p.at_time.us());hash.mix(p.target_data_rate.bps());hash.mix(p.target_duration.us());hash.mix(p.min_probe_delta.us());hash.mix(p.target_probe_count);hash.mix(p.id);}
   ++states[int(e.state_)];hash.mix(int(e.state_));hash.mix(e.network_available_);hash.mix(e.enable_periodic_alr_probing_);hash.mix(e.repeated_initial_probing_enabled_);
   hash.mix(e.estimated_bitrate_.bps());hash.mix(e.start_bitrate_.bps());hash.mix(e.max_bitrate_.bps());hash.mix(e.max_total_allocated_bitrate_.bps());
   hash.optional(finiteRate(e.min_bitrate_to_probe_further_));hash.mix(e.time_last_probing_initiated_.us());
   hash.optional(e.last_allowed_repeated_initial_probe_.IsFinite()?std::optional<uint64_t>(e.last_allowed_repeated_initial_probe_.us()):std::nullopt);
   hash.mix(e.next_probe_cluster_id_);hash.mix(int(e.bandwidth_limited_cause_));
  }
  std::cout<<"golden "<<s<<' '<<hash.value<<' '<<clusters<<' '<<states[0]<<' '<<states[1]<<' '<<states[2]<<'\n';
 }
}
'''
fixture = Path(__file__).with_name("probe_controller_replay.h").resolve()
(root / "reference.cpp").write_text(prefix + units + containers + source("probe_controller.h") + source("probe_controller.cc") +
                                    "\n#include " + json.dumps(str(fixture)) + "\n" + main)
subprocess.run([args.cxx, "-std=c++20", "-O0", str(root / "reference.cpp"), "-o", str(root / "reference")], check=True)
subprocess.run([str(root / "reference")], check=True)
