#ifndef LANTERNCAST_PCM_QUEUE_H
#define LANTERNCAST_PCM_QUEUE_H
#include <atomic>
#include <stdint.h>
#include <algorithm>
// One producer and one consumer. Only the audio callback advances read_.
class PcmQueue {
 public:
  static const uint32_t kFrames=32768;
  void Reset(){read_=0;write_=0;trim_=0;dropped_=0;empty_=0;held_=0;gain_=0;left_=right_=0;primed_=false;}
  unsigned queued()const{return write_.load(std::memory_order_acquire)-read_.load(std::memory_order_acquire);}
  unsigned dropped()const{return dropped_.load();}
  unsigned empty()const{return empty_.load();}
  unsigned held()const{return held_.load();}
  void Clear(){trim_.store(write_.load(std::memory_order_acquire),std::memory_order_release);}
  bool Push(const int16_t* samples,unsigned frames,double pts,int rate,bool synchronized=false) {
    uint32_t w=write_.load(std::memory_order_relaxed),r=read_.load(std::memory_order_acquire);
    if(frames>=kFrames || w-r+frames>=kFrames){Clear();return false;}
    for(unsigned i=0;i<frames;++i){unsigned slot=(w+i)%kFrames;data_[slot*2]=samples[i*2];data_[slot*2+1]=samples[i*2+1];time_[slot]=static_cast<float>(pts+double(i)/rate);}
    w+=frames;write_.store(w,std::memory_order_release);
    // Once a stall has created >120ms of audio, keep the newest 60ms.
    if(!synchronized && w-r>unsigned(rate*0.12))trim_.store(w-unsigned(rate*0.06),std::memory_order_release);
    return true;
  }
  void Render(int16_t* out,unsigned frames,int rate,double clock,bool synchronized) {
    uint32_t r=read_.load(std::memory_order_relaxed),w=write_.load(std::memory_order_acquire),cut=trim_.load(std::memory_order_acquire);
    if(static_cast<int32_t>(cut-r)>0 && static_cast<int32_t>(w-cut)>=0){dropped_.fetch_add(cut-r);r=cut;}
    if(!primed_ && w-r>=unsigned(rate*0.025))primed_=true;
    for(unsigned i=0;i<frames;++i){
      double target=clock+double(i)/rate;
      if(synchronized)while(r!=w && time_[r%kFrames]<target-0.045){++r;dropped_.fetch_add(1);}
      bool available=primed_ && r!=w && (!synchronized || time_[r%kFrames]<=target+0.015);
      if(available){gain_=std::min(1.0f,gain_+1.0f/64);left_=data_[2*(r%kFrames)];right_=data_[2*(r%kFrames)+1];++r;}
      else {gain_=std::max(0.0f,gain_-1.0f/64);if(r==w)empty_.fetch_add(1);else held_.fetch_add(1);}
      out[2*i]=static_cast<int16_t>(left_*gain_);out[2*i+1]=static_cast<int16_t>(right_*gain_);
    }
    if(r==w)primed_=false;
    read_.store(r,std::memory_order_release);
  }
 private:
  bool primed_=false;float gain_=0;int16_t left_=0,right_=0;
  int16_t data_[kFrames*2];float time_[kFrames];
  std::atomic<uint32_t> read_{0},write_{0},trim_{0},dropped_{0},empty_{0},held_{0};
};
#endif
