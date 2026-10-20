#!/usr/bin/env python3
"""Reproduce pinned libwebrtc AIMD/DelayBasedBwe golden replay values.

Downloads upstream sources into --directory, builds a standalone reference with
original integer units/algorithm bodies, and prints hashes used by the C++ tests.
Requires network access and a C++17 host compiler; never runs during normal tests.
"""
import argparse
import base64
import concurrent.futures
from pathlib import Path
import subprocess
import tempfile
import urllib.request

REVISION = "602d5fc23caaca7728ad5f3f0e68b92274d1a38a"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--directory", type=Path, default=Path(tempfile.gettempdir()) / ("uav-bwe-reference-" + REVISION))
parser.add_argument("--cxx", default="c++")
args = parser.parse_args()
root = args.directory.resolve()
root.mkdir(parents=True, exist_ok=True)
link = trend = root
files = [
    "rtc_base/units/unit_base.h",
    *["api/units/" + name + ".h" for name in ("data_rate", "data_size", "frequency", "time_delta", "timestamp")],
    "api/transport/bandwidth_usage.h",
    "api/network_state_predictor.h",
    "modules/remote_bitrate_estimator/include/bwe_defines.h",
    *["modules/remote_bitrate_estimator/aimd_rate_control." + ext for ext in ("h", "cc")],
    *["modules/congestion_controller/goog_cc/" + name + "." + ext
      for name in ("link_capacity_estimator", "inter_arrival_delta", "trendline_estimator", "delay_based_bwe")
      for ext in ("h", "cc")],
    "modules/congestion_controller/goog_cc/delay_increase_detector_interface.h",
]
def download(path):
    # Always download from this revision; do not trust an unrelated cached file.
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

# Keep original algorithm and units. Replace platform headers, logging,
# field-trial parsers, and transport data containers; expose private fields only
# to configure the separate-audio replay without the FieldTrials parser.
def source(name,folder=root):
 return '\n'.join(line for line in (folder/name).read_text().splitlines() if not line.lstrip().startswith('#')).replace(' private:', ' public:')+'\n'
prefix=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#define RTC_CHECK(x) ((void)0)
#define RTC_EXPORT
#define RTC_DCHECK(x) ((void)0)
#define RTC_DCHECK_GE(a,b) ((void)0)
#define RTC_DCHECK_GT(a,b) ((void)0)
#define RTC_DCHECK_LE(a,b) ::webrtc::Ignore{}
#define RTC_DCHECK_LT(a,b) ((void)0)
#define RTC_DCHECK_NOTREACHED() ((void)0)
#define RTC_DCHECK_RUNS_SERIALIZED(x) ((void)0)
#define RTC_HISTOGRAM_ENUMERATION(a,b,c) ((void)0)
#define RTC_LOG(x) Ignore{}
namespace webrtc {
struct Ignore { template<class T> constexpr Ignore& operator<<(const T&) { return *this; } };
template<class T,class U> constexpr T dchecked_cast(U value) { return static_cast<T>(value); }
template<class T,class U> constexpr T DivideRoundToNearest(T value,U denominator) {return value/denominator+(value%denominator>=denominator/2)-(value%denominator<=-denominator/2);}
template<class T,class L,class H> T SafeClamp(T value,L low,H high) {return std::clamp(value,T(low),T(high));}
struct FieldTrialsView {
 bool hold_alr=false,disable_upper=false,c_upper=true; double beta=.85;
 std::string Lookup(const char* key) const {return std::string(key)=="WebRTC-BweBackOffFactor"?"Enabled-"+std::to_string(beta):"";}
 bool IsEnabled(const char* key) const {return std::string(key)=="WebRTC-BweBackOffFactor" || (hold_alr&&std::string(key)=="WebRTC-DontIncreaseDelayBasedBweInAlr");}
};
bool ref_disable_upper=false,ref_c_upper=true;
struct FieldTrialFlag { bool value; FieldTrialFlag(const char*):value(ref_disable_upper){} operator bool()const{return value;} };
template<class T> struct FieldTrialParameter { T value;FieldTrialParameter(const char*,T):value(ref_c_upper){} operator T()const{return value;} };
inline void ParseFieldTrial(std::initializer_list<void*>,const std::string&){}
struct StructParametersParser {template<class... T> static std::unique_ptr<StructParametersParser> Create(T&&...){return std::make_unique<StructParametersParser>();}void Parse(const std::string&){}std::string Encode(){return {};}};
struct RaceChecker {};
struct InterArrival {};
struct RtcEventLog {template<class T>void Log(T&&){}};
struct RtcEventBweUpdateDelayBased {template<class... T> explicit RtcEventBweUpdateDelayBased(T&&...){}};
}
'''
units=source('unit_base.h',link)+source('time_delta.h')+source('timestamp.h')+source('data_size.h')+source('frequency.h')+source('data_rate.h',link)
types=r'''
namespace webrtc {
inline std::string ToString(DataRate r){return std::to_string(r.bps());}
struct NetworkStateEstimate {DataRate link_capacity_lower=DataRate::MinusInfinity(),link_capacity_upper=DataRate::MinusInfinity();};
struct SentPacket {Timestamp send_time=Timestamp::MinusInfinity();DataSize size=DataSize::Zero();bool audio=false;int64_t sequence_number=0;};
struct PacketResult {SentPacket sent_packet;Timestamp receive_time=Timestamp::PlusInfinity();};
struct TransportPacketsFeedback {Timestamp feedback_time=Timestamp::MinusInfinity();std::vector<PacketResult> packet_feedbacks;
 std::vector<PacketResult> SortedByReceiveTime() const {
  std::vector<PacketResult> result;for(const auto& p:packet_feedbacks)if(p.sent_packet.send_time.IsFinite()&&p.receive_time.IsFinite())result.push_back(p);
  std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){if(a.receive_time!=b.receive_time)return a.receive_time<b.receive_time;if(a.sent_packet.send_time!=b.sent_packet.send_time)return a.sent_packet.send_time<b.sent_packet.send_time;return a.sent_packet.sequence_number<b.sent_packet.sequence_number;});return result;
 }
};
}
'''
body=source('bandwidth_usage.h',trend)+source('network_state_predictor.h',trend)+source('delay_increase_detector_interface.h',trend)+source('bwe_defines.h')
body+=r'''namespace webrtc {RateControlInput::RateControlInput(BandwidthUsage state,const std::optional<DataRate>& rate):bw_state(state),estimated_throughput(rate){} RateControlInput::~RateControlInput()=default;}'''
body+=source('link_capacity_estimator.h',link)+source('link_capacity_estimator.cc',link)
body+=source('aimd_rate_control.h')+source('aimd_rate_control.cc')
body+=source('inter_arrival_delta.h')+source('inter_arrival_delta.cc')
body+=source('trendline_estimator.h',trend)+source('trendline_estimator.cc',trend)
body+=source('delay_based_bwe.h')+source('delay_based_bwe.cc')
main=r'''
using namespace webrtc;
struct Predictor:NetworkStatePredictor {BandwidthUsage state=BandwidthUsage::kBwNormal;BandwidthUsage Update(int64_t,int64_t,BandwidthUsage)override{return state;}};
int main(){
 for(int s=0;s<6;++s){
  FieldTrialsView trials;trials.hold_alr=s==2;ref_disable_upper=s==4;ref_c_upper=s!=3;trials.beta=s==5?.7:.85;
  AimdRateControl e(trials,true);if(s!=0)e.SetStartBitrate(DataRate::BitsPerSec(600000));
  uint64_t hash=14695981039346656037ULL;auto mix=[&](uint64_t v){hash=(hash^v)*1099511628211ULL;};uint32_t random=0x61696d64U+s;
  for(int i=0;i<1000;++i){
   random=random*1664525U+1013904223U;uint64_t ack=10000+(random>>8)%1000000;
   const auto now=Timestamp::Micros(1000000+i*100000LL+(i%3)*250);
   std::optional<NetworkStateEstimate> network;if(i%13!=0){network.emplace();network->link_capacity_lower=DataRate::BitsPerSec(i%7?100000:700000);network->link_capacity_upper=DataRate::BitsPerSec(i%11?750000:200000);}
   e.SetNetworkStateEstimate(network);e.SetInApplicationLimitedRegion((i/100)%2);
   if(i%97==0)e.SetRtt(TimeDelta::Millis((i%5)*75));if(i%61==0)e.SetMinBitrate(DataRate::BitsPerSec(i%2?10000:5000));
   if(i%47==46)e.SetEstimate(DataRate::BitsPerSec(20000+ack),now);
   const auto state=i%200<80?BandwidthUsage::kBwNormal:(i%200<110?BandwidthUsage::kBwOverusing:(i%200<150?BandwidthUsage::kBwUnderusing:BandwidthUsage::kBwNormal));
   std::optional<DataRate> rate;if(i%9!=0)rate=DataRate::BitsPerSec(ack);
   mix(e.Update({state,rate},now).bps());mix(e.ValidEstimate());mix(uint64_t(e.GetNearMaxIncreaseRateBpsPerSecond()));mix(e.GetFeedbackInterval().us());mix(e.TimeToReduceFurther(now,DataRate::BitsPerSec(ack)));mix(e.InitialTimeToReduceFurther(now));
  }
  std::cout<<"aimd "<<s<<' '<<hash<<' '<<e.LatestEstimate().bps()<<'\n';
 }
 for(int s=0;s<4;++s){
  ref_disable_upper=false;ref_c_upper=true;FieldTrialsView trials;trials.hold_alr=s==3;Predictor predictor;
  DelayBasedBwe e(&trials,nullptr,s==3?&predictor:nullptr);if(s!=0)e.SetStartBitrate(DataRate::BitsPerSec(600000));
  if(s==2){e.separate_audio_.enabled=true;e.separate_audio_.packet_threshold=3;e.separate_audio_.time_threshold=TimeDelta::Millis(100);}
  uint64_t hash=14695981039346656037ULL;auto mix=[&](uint64_t v){hash=(hash^v)*1099511628211ULL;};int64_t recv=2000000,offset=0;int counts[3]={};
  for(int i=0;i<600;++i){
   if(i==400)offset+=2000001;
   int64_t recv_delta=i<80?20000:(i<180?23000:(i<260?17000:20000));if(i>=450)recv_delta+=(i%3-1)*250;
   recv+=recv_delta;const auto now=Timestamp::Micros(1000000+i*30000LL+offset);
   TransportPacketsFeedback feedback;feedback.feedback_time=now;
   for(int j=0;j<(s==1?4:1);++j){PacketResult p;p.sent_packet.send_time=Timestamp::Micros(1000000+i*20000LL+j*2000+offset+(i%2?275:0));p.receive_time=Timestamp::Micros(recv+j*1800+offset);p.sent_packet.size=DataSize::Bytes(j%2?100:1200);p.sent_packet.sequence_number=i*4+j;p.sent_packet.audio=(i>=100&&i<180)||i%7==0;feedback.packet_feedbacks.push_back(p);}
   if(s==1)std::reverse(feedback.packet_feedbacks.begin(),feedback.packet_feedbacks.end());
   std::optional<DataRate> ack,probe;if(i>=45&&i%13!=0)ack=DataRate::BitsPerSec(i<180?450000:650000);
   if(i==300||i==420)probe=DataRate::BitsPerSec(900000);
   predictor.state=i<80?BandwidthUsage::kBwNormal:(i<120?BandwidthUsage::kBwOverusing:(i<170?BandwidthUsage::kBwUnderusing:BandwidthUsage::kBwNormal));
   std::optional<NetworkStateEstimate> network;if(i>=320&&i<360){network.emplace();network->link_capacity_upper=DataRate::BitsPerSec(800000);network->link_capacity_lower=DataRate::BitsPerSec(200000);}
   auto r=e.IncomingPacketFeedbackVector(feedback,ack,probe,network,i>=280&&i<380);++counts[int(r.delay_detector_state)];
   mix(r.updated);mix(r.probe);mix(r.target_bitrate.bps());mix(r.recovered_from_overuse);mix(int(r.delay_detector_state));mix(e.last_estimate().bps());mix(int(e.last_state()));
   std::vector<uint32_t> ssrcs;DataRate bitrate=DataRate::Zero();bool valid=e.LatestEstimate(&ssrcs,&bitrate);mix(valid);if(valid)mix(bitrate.bps());
  }
  std::cout<<"delay "<<s<<' '<<hash<<' '<<counts[0]<<' '<<counts[1]<<' '<<counts[2]<<' '<<e.last_estimate().bps()<<'\n';
 }
}
'''
(root/'reference.cpp').write_text(prefix+units+types+body+main)

subprocess.run([args.cxx, "-std=c++17", "-O0", str(root / "reference.cpp"), "-o", str(root / "reference")], check=True)
subprocess.run([str(root / "reference")], check=True)
