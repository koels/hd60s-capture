/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/shared.h"
#include "../src/shared-internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
static atomic_bool stop=false;
static unsigned frames,blocks,failures,sleep_ms,synthetic;
static uint64_t last_video,last_audio,samples,first_video;
static void halt(int sig) { (void)sig;atomic_store(&stop,true); }
static void video(void *ctx,const uint8_t *p,const hd_mode *m,uint64_t time) {
 (void)ctx;if(last_video && time<=last_video){fprintf(stderr,"Non-monotonic video: %" PRIu64 " after %" PRIu64 "\n",time,last_video);failures++;}if(!first_video)first_video=time;last_video=time;frames++;
 if(synthetic && (m->width!=320 || m->height!=240 || m->rate!=30 || p[0]!=80 || p[1]!=128 || p[m->width*m->height*2-1]!=128)){fprintf(stderr,"Invalid synthetic video: %ux%u@%u first %u,%u last %u\n",m->width,m->height,m->rate,p[0],p[1],p[m->width*m->height*2-1]);failures++;}
 if(sleep_ms)usleep(sleep_ms*1000);
}
static void audio(void *ctx,const uint8_t *p,size_t bytes,unsigned rate,uint64_t time) {
 (void)ctx;(void)p;if(last_audio && time<=last_audio){fprintf(stderr,"Non-monotonic audio: %" PRIu64 " after %" PRIu64 "\n",time,last_audio);failures++;}last_audio=time;if(rate!=48000)failures++;if(bytes%4)failures++;blocks++;samples+=bytes/4;
}
static void message(void *ctx,const char *text) { (void)ctx;fprintf(stderr,"%s\n",text); }
int main(int argc,char **argv) {
 assert(hd_shared_age(100,101)==0); /* Regression: freshly published heartbeats must not expire. */
 assert(hd_shared_age(100,40)==60);
 double seconds=4;
 for(int i=1;i<argc;i++) { if(!strcmp(argv[i],"--synthetic"))synthetic=1;else if(!strcmp(argv[i],"--slow"))sleep_ms=100;else seconds=atof(argv[i]); }
 signal(SIGTERM,halt);signal(SIGINT,halt);hd_callbacks cb={.video=video,.audio=audio,.message=message};int result=hd_shared_capture(&cb,&stop,seconds);
 printf("{\"pid\":%d,\"frames\":%u,\"audio_blocks\":%u,\"samples\":%" PRIu64 ",\"first_video\":%" PRIu64 ",\"last_video\":%" PRIu64 ",\"failures\":%u}\n",getpid(),frames,blocks,samples,first_video,last_video,failures);
 return result||failures||!blocks?1:0;
}
