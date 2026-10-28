/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/processing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 uint8_t pcm[]={0,0,0,64,0,128,255,127};float out[4];hd_audio_state s={0};
 hd_audio_process(&s,pcm,out,4,0,0,0,48000);
 assert(out[0]==0 && out[1]==.5f && out[2]==-1 && out[3]>.9999f && s.clipped==2);
 memset(&s,0,sizeof s);hd_audio_process(&s,pcm,out,4,-6,0,0,48000);
 assert(fabs(out[1]-.2505936)<1e-6 && out[2]>-.502f && s.clipped==2);
 memset(&s,0,sizeof s);hd_audio_process(&s,pcm,out,4,6,0,1,48000);
 for(int i=0;i<4;i++) assert(fabs(out[i])<.891252);
 hd_audio_process(&s,pcm,out,4,6,1,1,48000);
 for(int i=0;i<4;i++) assert(out[i]==0);
 /* A minute at 59.94 input, capped to 29.97, must preserve half the frames. */
 uint64_t next=0;unsigned count=0;
 for(unsigned i=0;i<3600;i++) count+=hd_frame_due(1000000000ULL+(uint64_t)i*1001000000000ULL/60000,29970,&next);
 assert(count==1800);
 /* A cap above the input rate never duplicates frames. */
 next=0;count=0;
 for(unsigned i=0;i<100;i++) count+=hd_frame_due(1000000000ULL+i*40000000ULL,60000,&next);
 assert(count==100);
 puts("Processing passed: PCM signedness, -6 dB gain, stereo limiter, mute, fractional FPS cap");
}
