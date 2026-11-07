// Host tool for the shared NIB codec (sim/a2_nib_codec.*):
//   nibtool roundtrip in.dsk              self-test encode/decode (both orders, rotated tracks)
//   nibtool dsk2nib in.dsk <order> out.nib   order 0=DOS(.dsk/.do) 1=ProDOS(.po); same bytes the sim/Main feed the core
//   nibtool nib2dsk in.nib <order> out.dsk
//   nibtool cmp in.nib in.dsk <order>      decode a .nib and compare with a .dsk
// Build: make nibtool

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "a2_nib_codec.h"
static std::vector<uint8_t> rd(const char*f){FILE*h=fopen(f,"rb");std::vector<uint8_t>v;if(!h)return v;fseek(h,0,2);v.resize(ftell(h));fseek(h,0,0);fread(v.data(),1,v.size(),h);fclose(h);return v;}
int main(int c,char**a){
 // nibtool nib2dsk in.nib order out.dsk  | dsk2nib in.dsk order out.nib | cmp in.nib in.dsk order
 std::string m=a[1];
 if(m=="nib2dsk"||m=="cmp"){
  auto nib=rd(a[2]); int order=atoi(a[m=="cmp"?4:3]);
  std::vector<uint8_t> dsk(A2NIB_DSK_IMAGE_SIZE,0);
  for(int t=0;t<35;t++){uint32_t g=a2_denibblize_track(&nib[t*6656],t,order,&dsk[t*4096]); if(g!=0xffff) printf("track %d mask %04x\n",t,g);}
  if(m=="nib2dsk"){FILE*o=fopen(a[4],"wb");fwrite(dsk.data(),1,dsk.size(),o);fclose(o);}
  else {auto ref=rd(a[3]); int diff=0; for(int t=0;t<35;t++)for(int s=0;s<16;s++) if(memcmp(&dsk[t*4096+s*256],&ref[t*4096+s*256],256)){ if(diff<20)printf("diff t%d s%d\n",t,s); diff++;} printf("%d sectors differ\n",diff);}
 } else if(m=="dsk2nib"){
  auto dsk=rd(a[2]); int order=atoi(a[3]); dsk.resize(A2NIB_DSK_IMAGE_SIZE);
  std::vector<uint8_t> nib(A2NIB_IMAGE_SIZE);
  for(int t=0;t<35;t++) a2_nibblize_track(&dsk[t*4096],t,254,order,&nib[t*6656]);
  FILE*o=fopen(a[4],"wb");fwrite(nib.data(),1,nib.size(),o);fclose(o);
 } else if(m=="roundtrip"){
  auto dsk=rd(a[2]); dsk.resize(A2NIB_DSK_IMAGE_SIZE); int bad=0;
  for(int order=0;order<2;order++)for(int t=0;t<35;t++){uint8_t n[6656],o2[4096];memset(o2,0,4096);a2_nibblize_track(&dsk[t*4096],t,254,order,n);
   // rotate to test circular wrap
   uint8_t r[6656]; for(int k=0;k<6656;k++) r[k]=n[(k+1000*t)%6656];
   if(a2_denibblize_track(r,t,order,o2)!=0xffff||memcmp(o2,&dsk[t*4096],4096)) bad++;}
  printf("roundtrip bad tracks: %d\n",bad);
 }
}
