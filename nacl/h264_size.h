#ifndef LANTERNCAST_H264_SIZE_H
#define LANTERNCAST_H264_SIZE_H
#include <stdint.h>
#include <string>

// Read the visible dimensions from an Annex B SPS, including chroma crop units.
// AirPlay senders can leave the mirror header's width and height at zero.
namespace h264_size {
class Bits {
 public:
  explicit Bits(const std::string& data) : data_(data), pos_(0), ok_(true) {}
  uint32_t Read(unsigned n) {
    if(n>32 || pos_+n>data_.size()*8) {ok_=false;return 0;}
    uint32_t v=0;
    while(n--) {v=(v<<1)|((static_cast<unsigned char>(data_[pos_/8])>>(7-pos_%8))&1);++pos_;}
    return v;
  }
  uint32_t UE() {
    unsigned n=0;
    while(ok_ && Read(1)==0) {if(++n>30) {ok_=false;return 0;}}
    return ok_ ? ((1u<<n)-1)+Read(n) : 0;
  }
  int SE() {uint32_t v=UE();return (v&1)?static_cast<int>((v+1)/2):-static_cast<int>(v/2);}
  bool ok() const {return ok_;}
 private:
  std::string data_;size_t pos_;bool ok_;
};
inline bool Parse(const std::string& annexb, int* width, int* height) {
  size_t start=std::string::npos,end=annexb.size();
  for(size_t i=0;i+3<annexb.size();++i) {
    size_t prefix=0;
    if(annexb[i]==0 && annexb[i+1]==0) {
      if(annexb[i+2]==1) prefix=3;
      else if(i+4<annexb.size() && annexb[i+2]==0 && annexb[i+3]==1) prefix=4;
    }
    if(!prefix) continue;
    if(start!=std::string::npos) {end=i;break;}
    if((annexb[i+prefix]&31)==7) start=i+prefix+1;
    i+=prefix-1;
  }
  if(start==std::string::npos || end<=start) return false;
  std::string rbsp;
  unsigned zeros=0;
  for(size_t i=start;i<end;++i) {
    unsigned char c=annexb[i];
    if(zeros>=2 && c==3) {zeros=0;continue;}
    rbsp.push_back(c);zeros=c==0?zeros+1:0;
  }
  Bits b(rbsp);
  unsigned profile=b.Read(8);b.Read(8);b.Read(8);b.UE();
  unsigned chroma=1,separate=0;
  if(profile==100 || profile==110 || profile==122 || profile==244 || profile==44 ||
     profile==83 || profile==86 || profile==118 || profile==128 || profile==138 ||
     profile==139 || profile==134 || profile==135) {
    chroma=b.UE();if(chroma>3) return false;
    if(chroma==3) separate=b.Read(1);
    b.UE();b.UE();b.Read(1);
    if(b.Read(1)) {
      for(unsigned i=0;i<(chroma==3?12u:8u);++i) if(b.Read(1)) {
        int last=8,next=8;
        for(unsigned j=0;j<(i<6?16u:64u);++j) {
          if(next) {int delta=b.SE();next=static_cast<int>((static_cast<int64_t>(last)+delta+256)%256);if(next<0) next+=256;}
          last=next?next:last;
        }
      }
    }
  }
  b.UE();unsigned poc=b.UE();
  if(poc==0) b.UE();
  else if(poc==1) {
    b.Read(1);b.SE();b.SE();unsigned cycle=b.UE();if(cycle>255) return false;
    for(unsigned i=0;i<cycle;++i) b.SE();
  } else if(poc>2) return false;
  b.UE();b.Read(1);
  uint32_t mw=b.UE(),mh=b.UE();unsigned frame=b.Read(1);
  if(!frame) b.Read(1);
  b.Read(1);
  uint32_t left=0,right=0,top=0,bottom=0;
  if(b.Read(1)) {left=b.UE();right=b.UE();top=b.UE();bottom=b.UE();}
  if(!b.ok() || mw>1023 || mh>1023) return false;
  unsigned array=separate?0:chroma;
  unsigned cx=(array==1 || array==2)?2:1;
  unsigned cy=(array==1?2:1)*(2-frame);
  int64_t w=(mw+1)*16-static_cast<int64_t>(cx)*(static_cast<int64_t>(left)+right);
  int64_t h=(mh+1)*16*(2-frame)-static_cast<int64_t>(cy)*(static_cast<int64_t>(top)+bottom);
  if(w<=0 || h<=0 || w>8192 || h>8192) return false;
  *width=static_cast<int>(w);*height=static_cast<int>(h);return true;
}
}
#endif
