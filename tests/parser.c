/* SPDX-License-Identifier: GPL-2.0-only */
/* Include the implementation to exercise the private parser without USB I/O. */
#include "../src/capture.c"
#include <assert.h>
static unsigned emitted,audio_calls;
static uint8_t expected[16];
static void got_video(void *ctx,const uint8_t *p,const hd_mode *m,uint64_t ts) {
 (void)ctx; assert(m->width==4 && m->height==2); assert(ts); assert(!memcmp(p,expected,16)); emitted++;
}
static void got_audio(void *ctx,const uint8_t *p,size_t n,unsigned rate,uint64_t ts) {
 (void)ctx; (void)p; assert(n==1920 && rate==48000 && ts); audio_calls++;
}
int main(void) {
 const uint8_t stream[]={
  255,0,0,0xab,0,0,0,0,0,0,0,0,
  255,0,0,0x80,16,128,17,128,18,128,19,128,
  255,0,255,2,1,2,3,4,5,6,7,8,
  255,0,0,0x80,20,128,21,128,22,128,23,128,
  255,0,0,0xab,0,0,0,0,0,0,0,0};
 memcpy(expected,stream+16,8); memcpy(expected+8,stream+40,8);
 for(unsigned chunk=1;chunk<=sizeof stream;chunk++) {
  state s={0}; uint8_t frame[16]={0}; s.frame=frame;
  s.mode=(hd_mode){.width=4,.height=2,.rate=60,.audio_rate=48000}; s.cb.video=got_video;
  unsigned before=emitted;
  for(unsigned off=0;off<sizeof stream;) { unsigned n=sizeof stream-off; if(n>chunk)n=chunk; parse(&s,stream+off,n);off+=n; }
  assert(emitted==before+1 && !s.partial && !s.bad);
 }
 state s={0}; uint8_t frame[16]={0}; s.frame=frame; s.epoch=1;
 s.mode=(hd_mode){.width=4,.height=2,.rate=60,.audio_rate=48000}; s.cb.audio=got_audio;
 uint8_t packet[]={255,0,255,2,1,2,3,4,5,6,7,8};
 for(unsigned i=0;i<240;i++) parse(&s,packet,sizeof packet);
 assert(audio_calls==1 && s.samples==480 && s.pcm_n==0);
 /* An incomplete frame must never be emitted. */
 memset(&s,0,sizeof s); s.frame=frame; s.cb.video=got_video;
 s.mode=(hd_mode){.width=4,.height=3,.rate=60,.audio_rate=48000};
 unsigned before=emitted; parse(&s,stream,sizeof stream);
 assert(emitted==before && s.partial==1);
 /* An interlaced receiver must not expose a capture mode, and progressive
  * timing after it must be accepted without reinitializing the device. */
 uint8_t timing[32]={0};hd_mode decoded={0};
 timing[10]=128;timing[11]=7; /* 1920 */
 timing[8]=56;timing[9]=4; /* 1080 */
 timing[6]=152;timing[7]=8; /* total 2200 */
 timing[4]=101;timing[5]=4; /* total 1125 */
 timing[12]=60;timing[13]=1;timing[14]=48;
 assert(decode_mode(&s,&decoded,timing)==MODE_INTERLACED && !decoded.width);
 timing[13]=2;
 assert(decode_mode(&s,&decoded,timing)==MODE_READY && decoded.width==1920 && decoded.height==1080);
 memset(timing,0,sizeof timing);
 assert(decode_mode(&s,&decoded,timing)==MODE_ABSENT);
 /* A loss of two seconds must not poison all subsequent client timestamps. */
 uint64_t epoch=1000000000ULL;
 assert(paced_time(&epoch,100000000ULL,3100000000ULL)==3070000000ULL);
 assert(paced_time(&epoch,110000000ULL,3110000000ULL)==3080000000ULL);
 assert(paced_time(&epoch,120000000ULL,3090000000ULL)==3090000000ULL);
 puts("Parser passed: every USB chunk size, audio batching, incomplete-frame rejection");
}
