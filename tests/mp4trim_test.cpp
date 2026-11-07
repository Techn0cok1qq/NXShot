#include "../sysmodule/source/mp4trim.h"
#include <cstdlib>
// 使い方: g++ -std=c++17 -O2 -o t mp4trim_test.cpp && ./t 入力.mp4 出力.mp4 上限バイト
int main(int argc,char**argv){
  if(argc<4){printf("usage: %s in.mp4 out.mp4 maxBytes\n",argv[0]);return 2;}
  mp4trim::Info i;
  int64_t lim=atoll(argv[3]);
  int r=mp4trim::trim(argv[1],argv[2],lim,&i);
  printf("result=%d orig=%lld new=%lld cut=%.3f newlen=%.3f\n",r,(long long)i.origSize,(long long)i.newSize,i.cutSeconds,i.newSeconds);
  return r<0;
}
