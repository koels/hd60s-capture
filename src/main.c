/* SPDX-License-Identifier: GPL-2.0-only */
#include "capture.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static atomic_bool stop=false;
static const char *frame_path;
static unsigned frames;
static void interrupt(int signum) { (void)signum; atomic_store(&stop,true); }
static void video(void *ctx,const uint8_t *data,const hd_mode *mode,uint64_t ts) {
 (void)ctx; (void)ts;
 if(!frames++ && frame_path) {
  FILE *f=fopen(frame_path,"wb");
  if(f) { size_t n=(size_t)mode->width*mode->height*2; if(fwrite(data,1,n,f)!=n) perror("frame write"); fclose(f); }
  else perror(frame_path);
 }
}
int main(int argc,char **argv) {
 double seconds=10;
 for(int i=1;i<argc;i++) {
  if(!strcmp(argv[i],"--seconds") && i+1<argc) seconds=atof(argv[++i]);
  else if(!strcmp(argv[i],"--frame") && i+1<argc) frame_path=argv[++i];
  else { fprintf(stderr,"Usage: %s [--seconds N] [--frame output.yuy2]\n",argv[0]); return 2; }
 }
 signal(SIGINT,interrupt); signal(SIGTERM,interrupt);
 hd_callbacks cb={.video=video}; return hd_capture(&cb,&stop,seconds)?1:0;
}
