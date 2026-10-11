// Parameter sets generated independently by FFmpeg/libx264.
#include "h264_size.h"
#include <iostream>
#include <cassert>
static std::string Unhex(const char* h) {
 std::string s;while(*h) {unsigned v=0;for(int i=0;i<2;++i) {char c=*h++;v=v*16+(c<='9'?c-'0':c-'a'+10);}s.push_back(v);}return s;
}
int main() {
 int w=0,h=0;
 assert(h264_size::Parse(Unhex("000000016764001facd940b40a1b011000000300100000030020f18319600000000168ebe3cb22c0"),&w,&h));assert(w==720 && h==1280);
 for(size_t n=0;n<8;++n) assert(!h264_size::Parse(Unhex("000000016764001facd940b40a1b011000000300100000030020f18319600000000168ebe3cb22c0").substr(0,n),&w,&h));
 assert(h264_size::Parse(Unhex("000000016764001facd9405005bb011000000300100000030020f18319600000000168ebe3cb22c0"),&w,&h));assert(w==1280 && h==720);
 for(size_t n=0;n<8;++n) assert(!h264_size::Parse(Unhex("000000016764001facd9405005bb011000000300100000030020f18319600000000168ebe3cb22c0").substr(0,n),&w,&h));
 assert(h264_size::Parse(Unhex("0000000167640032acd9404a013fe227c044000003000400000300083c60c6580000000168ebe3cb22c0"),&w,&h));assert(w==1170 && h==2532);
 for(size_t n=0;n<8;++n) assert(!h264_size::Parse(Unhex("0000000167640032acd9404a013fe227c044000003000400000300083c60c6580000000168ebe3cb22c0").substr(0,n),&w,&h));
 assert(h264_size::Parse(Unhex("000000016742c028d900780227e5c044000003000400000300083c60c9200000000168cb83cb20"),&w,&h));assert(w==1920 && h==1080);
 for(size_t n=0;n<8;++n) assert(!h264_size::Parse(Unhex("000000016742c028d900780227e5c044000003000400000300083c60c9200000000168cb83cb20").substr(0,n),&w,&h));
 assert(!h264_size::Parse(std::string(200,0),&w,&h));
 std::cout<<"PASS SPS portrait, landscape, cropping and truncated inputs\n";
}
