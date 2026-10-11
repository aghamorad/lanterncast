#include "bplist.h"
#include <cstdio>
int main(){BplistBuilder b;std::vector<int> v;uint64_t n[]={0,255,256,65535,65536,0x3fffffc,0x1E527FFFF7ULL,0xffffffffffffffffULL};for(int i=0;i<8;++i)v.push_back(b.Uint(n[i]));std::string s=b.Encode(b.Array(v));fwrite(s.data(),1,s.size(),stdout);}
