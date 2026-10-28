/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stddef.h>
typedef struct { double gain; float peak; uint64_t clipped,samples; } hd_audio_state;
/* Convert signed little-endian PCM to float; measure BEFORE gain/limiting. */
static inline void hd_audio_process(hd_audio_state *s,const uint8_t *input,float *output,size_t n,double db,int mute,int limiter,unsigned rate) {
 float peak=0;
 for(size_t i=0;i<n;i++) {
  unsigned raw=input[2*i]|((unsigned)input[2*i+1]<<8);
  int value=raw>=32768?(int)raw-65536:(int)raw;
  float x=value/32768.0f;
  if(fabsf(x)>peak) peak=fabsf(x);
  if(value>=32760 || value<=-32760) s->clipped++;
  output[i]=x;
 }
 s->peak=peak; s->samples+=n;
 double amp=mute?0:pow(10,db/20);
 double desired=(limiter && peak*amp>0.891250938)?0.891250938/(peak*amp):1;
 /* Immediate attenuation per block; 100 ms release, shared stereo gain. */
 if(!s->gain) s->gain=1;
 if(desired<s->gain) s->gain=desired;
 else s->gain=desired+(s->gain-desired)*exp(-(double)n/(2*rate*.1));
 if(!limiter) s->gain=1;
 for(size_t i=0;i<n;i++) output[i]=(float)(output[i]*amp*s->gain);
}
static inline int hd_frame_due(uint64_t ts,unsigned cap,uint64_t *next) {
 if(!cap) { *next=0; return 1; }
 uint64_t period=1000000000000ULL/cap; /* cap is milli-fps */
 if(!*next || ts+period<*next) *next=ts;
 if(ts+100000<*next) return 0;
 do { *next+=period; } while(*next<=ts);
 return 1;
}
