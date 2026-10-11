#include <stdio.h>
int crypto_hash(unsigned char*,const unsigned char*,unsigned long long);
int main(){unsigned char out[64];crypto_hash(out,(const unsigned char*)"abc",3);for(int i=0;i<64;i++)printf("%02x",out[i]);}
