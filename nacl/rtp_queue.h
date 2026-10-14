#ifndef LANTERNCAST_RTP_QUEUE_H
#define LANTERNCAST_RTP_QUEUE_H
#include <map>
#include <string>
#include <stdint.h>
struct AudioPacket {uint16_t sequence;uint32_t timestamp;std::string bytes;};
class RtpQueue {
 public:
  void Reset(){packets_.clear();started_=false;waiting_=0;duplicates_=lost_=0;}
  bool Push(const AudioPacket& p) {
    if(!started_){next_=p.sequence;started_=true;}
    int delta=static_cast<int16_t>(p.sequence-next_);
    if(delta<0 || packets_.count(p.sequence)){++duplicates_;return false;}
    if(delta>128){packets_.clear();next_=p.sequence;waiting_=0;}
    packets_[p.sequence]=p;return true;
  }
  bool Pop(double now,AudioPacket* p) {
    if(packets_.empty()){waiting_=0;return false;}
    auto it=packets_.find(next_);
    if(it==packets_.end()){
      if(waiting_==0)waiting_=now;
      if(packets_.size()<6 && now-waiting_<0.04)return false;
      unsigned distance=65535;auto first=packets_.begin();
      for(auto q=packets_.begin();q!=packets_.end();++q){unsigned d=static_cast<uint16_t>(q->first-next_);if(d<distance){distance=d;first=q;}}
      lost_+=distance;next_=first->first;it=first;
    }
    *p=it->second;packets_.erase(it);++next_;waiting_=0;return true;
  }
  unsigned duplicates()const{return duplicates_;}
  unsigned lost()const{return lost_;}
 private:
  std::map<uint16_t,AudioPacket> packets_;uint16_t next_=0;bool started_=false;
  double waiting_=0;unsigned duplicates_=0,lost_=0;
};
#endif
