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
static unsigned fields;
static void got_field(void *ctx,const uint8_t *p,const hd_mode *m,uint64_t ts) {
 (void)ctx;assert(ts && m->width==4 && m->height==4 && m->rate==60);
 const uint8_t *source=expected;
 assert(!memcmp(p,source,8) && !memcmp(p+8,source,8));
 assert(!memcmp(p+16,source+8,8) && !memcmp(p+24,source+8,8));
 fields++;
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
 /* Both field markers must work across every USB fragmentation boundary. */
 for(unsigned field=0;field<2;field++) {
  uint8_t interlace[sizeof stream];memcpy(interlace,stream,sizeof stream);
  interlace[3]=interlace[51]=field?0xec:0xab;
  interlace[15]=interlace[39]=field?0xc7:0x80;
  for(unsigned chunk=1;chunk<=sizeof stream;chunk++) {
   state t={0};uint8_t pixels[32]={0};t.frame=pixels;t.interlaced=true;
   t.mode=(hd_mode){.width=4,.height=4,.rate=60};t.cb.video=got_field;
   unsigned before=fields;
   for(unsigned off=0;off<sizeof interlace;) {
    unsigned n=sizeof interlace-off;if(n>chunk)n=chunk;parse(&t,interlace+off,n);off+=n;
   }
   assert(fields==before+1 && !t.bad && !t.partial);
   /* An incomplete field must not leak old lines into output. */
   t.mode.height=6;parse(&t,interlace,sizeof interlace);
   assert(fields==before+1 && t.partial==1);
   t.mode.height=4;parse(&t,interlace,sizeof interlace);
   assert(fields==before+2);
  }
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
 /* Interlaced timing reports field height. Normalize to progressive output
  * and accept a subsequent progressive source at the same output size. */
 uint8_t timing[32]={0};hd_mode decoded={0};
 timing[10]=128;timing[11]=7; /* 1920 */
 timing[8]=28;timing[9]=2; /* 540 lines per field */
 timing[6]=152;timing[7]=8; /* total 2200 */
 timing[4]=101;timing[5]=4; /* total 1125 */
 timing[12]=60;timing[13]=1;timing[14]=48;
 assert(decode_mode(&s,&decoded,timing)==MODE_READY && s.detected_interlaced && decoded.height==1080 && decoded.rate==60);
 timing[13]=2;timing[8]=56;timing[9]=4;
 assert(decode_mode(&s,&decoded,timing)==MODE_READY && decoded.width==1920 && decoded.height==1080 && !s.detected_interlaced);
 /* SD interlaced modes retain field cadence and double active height. */
 timing[10]=208;timing[11]=2; /* 720 */
 timing[8]=240;timing[9]=0;timing[13]=3;
 assert(decode_mode(&s,&decoded,timing)==MODE_READY && decoded.height==480 && decoded.fractional);
 timing[8]=32;timing[9]=1;timing[12]=50;timing[13]=1;
 assert(decode_mode(&s,&decoded,timing)==MODE_READY && decoded.height==576 && decoded.rate==50);
 /* Power-on hardware trace: partially updated 720x146 @95 timing must
  * keep waiting on the initialized card, rather than fail/reinitialize USB. */
 timing[8]=146;timing[9]=0;timing[12]=95;
 assert(decode_mode(&s,&decoded,timing)==MODE_UNSTABLE);
 timing[8]=240;timing[12]=60;timing[13]=1;
 assert(decode_mode(&s,&decoded,timing)==MODE_READY && decoded.height==480);
 timing[13]=9;
 assert(decode_mode(&s,&decoded,timing)<0); /* HDCP is still rejected. */
 memset(timing,0,sizeof timing);
 assert(decode_mode(&s,&decoded,timing)==MODE_ABSENT);
 /* A loss of two seconds must not poison all subsequent client timestamps. */
 uint64_t epoch=1000000000ULL;
 assert(paced_time(&epoch,100000000ULL,3100000000ULL)==3070000000ULL);
 assert(paced_time(&epoch,110000000ULL,3110000000ULL)==3080000000ULL);
 assert(paced_time(&epoch,120000000ULL,3090000000ULL)==3090000000ULL);
 /* A warm restart must discard partial records and old frame pacing while
  * preserving the initialized USB session and callbacks. ASan checks ownership. */
 state warm={.opened=true,.product=0x0076,.cb={.video=got_video},
  .remaining=128,.header_n=3,.pcm_n=100,.frames=60,.delivered=59,
  .epoch=123,.audio_epoch=456,.failed=true,.interlaced=true,.suspect=2};
 warm.frame=malloc(16);
 for(unsigned i=0;i<RING;i++) warm.ring[i].data=malloc(8);
 reset_stream(&warm);
 assert(warm.opened && warm.product==0x0076 && warm.cb.video==got_video);
 assert(!warm.frame && !warm.remaining && !warm.header_n && !warm.pcm_n);
 assert(!warm.frames && !warm.delivered && !warm.epoch && !warm.audio_epoch);
 assert(!warm.failed && !warm.interlaced && !warm.suspect);
 for(unsigned i=0;i<RING;i++) assert(!warm.ring[i].data && !warm.ring[i].ready);
 puts("Parser passed: progressive and interlaced fields at every USB chunk size, bob output, mode decoding, audio batching, incomplete-frame rejection");
}
