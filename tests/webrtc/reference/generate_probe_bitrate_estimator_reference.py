#!/usr/bin/env python3
"""Replay pinned original libwebrtc ProbeBitrateEstimator and integer units.

Requires explicit network access and a C++17 host compiler. Not used by CTest.
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
parser.add_argument("--directory", type=Path, default=Path(tempfile.gettempdir()) / ("uav-probe-rate-reference-" + REVISION))
parser.add_argument("--cxx", default="c++")
args = parser.parse_args()
root = args.directory.resolve()
root.mkdir(parents=True, exist_ok=True)
files = ["rtc_base/units/unit_base.h",
         *["api/units/" + n + ".h" for n in ("time_delta", "timestamp", "data_size", "frequency", "data_rate")],
         *["modules/congestion_controller/goog_cc/probe_bitrate_estimator." + ext for ext in ("h", "cc")]]


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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#define RTC_EXPORT
#define RTC_CHECK(x) ((void)0)
#define RTC_DCHECK(x) ((void)0)
#define RTC_DCHECK_GE(a,b) ((void)0)
#define RTC_DCHECK_GT(a,b) ((void)0)
#define RTC_DCHECK_NE(a,b) ((void)0)
#define RTC_DCHECK_LE(a,b) ::webrtc::Ignore{}
#define RTC_DCHECK_LT(a,b) ((void)0)
#define RTC_LOG(x) Ignore{}
namespace webrtc {
struct Ignore {template<class T> constexpr Ignore& operator<<(const T&){return *this;}};
template<class T,class U> constexpr T dchecked_cast(U v){return static_cast<T>(v);}
template<class T,class U> constexpr T DivideRoundToNearest(T v,U d){return v/d+(v%d>=d/2)-(v%d<=-d/2);}
struct RtcEventLog {template<class T>void Log(T&&){}};
enum class ProbeFailureReason {kInvalidSendReceiveInterval,kInvalidSendReceiveRatio};
struct RtcEventProbeResultSuccess {template<class... T>explicit RtcEventProbeResultSuccess(T&&...){}};
struct RtcEventProbeResultFailure {template<class... T>explicit RtcEventProbeResultFailure(T&&...){}};
}
'''
units = "".join(source(n + ".h") for n in ("unit_base", "time_delta", "timestamp", "data_size", "frequency", "data_rate"))
containers = r'''
namespace webrtc {
inline std::string ToString(DataRate r){return r.IsFinite()?std::to_string(r.bps()):"inf";}
inline std::string ToString(DataSize s){return s.IsFinite()?std::to_string(s.bytes()):"inf";}
inline std::string ToString(TimeDelta t){return t.IsFinite()?std::to_string(t.us()):"inf";}
struct PacedPacketInfo {static constexpr int kNotAProbe=-1;int probe_cluster_id=-1,probe_cluster_min_probes=-1,probe_cluster_min_bytes=-1;};
struct PacketResult {
 struct Sent {Timestamp send_time=Timestamp::MinusInfinity();DataSize size=DataSize::Zero();PacedPacketInfo pacing_info;} sent_packet;
 Timestamp receive_time=Timestamp::PlusInfinity();
};
}
'''
main = r'''
using namespace webrtc;
std::optional<uint64_t> rate(std::optional<DataRate> r){return r?std::optional<uint64_t>(r->bps()):std::nullopt;}
int main(){
 for(int s=0;s<6;++s){
  ProbeBitrateEstimator e(nullptr);probe_bitrate_replay::Input input(s);probe_bitrate_replay::Hash hash;int success=0,fetched=0;
  for(int i=0;i<4000;++i){
   const auto sample=input.sample(i,s);PacketResult p;
   p.sent_packet.send_time=Timestamp::Micros(sample.send_us);p.receive_time=Timestamp::Micros(sample.receive_us);
   p.sent_packet.size=DataSize::Bytes(sample.size);p.sent_packet.pacing_info={sample.cluster_id,sample.min_probes,sample.min_bytes};
   const auto result=e.HandleProbeAndEstimateBitrate(p);hash.rate(rate(result));success+=result.has_value();
   if(sample.fetch){auto last=e.FetchAndResetLastEstimatedBitrate();hash.rate(rate(last));fetched+=last.has_value();hash.rate(rate(e.FetchAndResetLastEstimatedBitrate()));}
   hash.mix(e.clusters_.size());hash.rate(rate(e.estimated_data_rate_));
  }
  std::cout<<"golden "<<s<<' '<<hash.value<<' '<<success<<' '<<fetched<<'\n';
 }
}
'''
fixture = Path(__file__).with_name("probe_bitrate_estimator_replay.h").resolve()
(root / "reference.cpp").write_text(prefix + units + containers + source("probe_bitrate_estimator.h") +
                                    source("probe_bitrate_estimator.cc") + "\n#include " + json.dumps(str(fixture)) + "\n" + main)
subprocess.run([args.cxx, "-std=c++17", "-O0", str(root / "reference.cpp"), "-o", str(root / "reference")], check=True)
subprocess.run([str(root / "reference")], check=True)
