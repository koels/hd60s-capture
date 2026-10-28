/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "capture.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HD_SHARED_MAGIC 0x4844363053484152ULL
#define HD_SHARED_VERSION 1
/* Which build of the service code this is, as one number: 0.4.6 is 406. The
 * build passes it in from the VERSION file, which also fills in the bundles'
 * CFBundleVersion, so the number a client compares and the version a human
 * reads cannot drift apart. The map's version covers its layout; this covers
 * the code, so a client can tell an older service from a current one and take
 * the card off it instead of inheriting its bugs. */
#ifndef HD_SHARED_BUILD
#error "HD_SHARED_BUILD must come from the build; see VERSION and the Makefile"
#endif
#define HD_SHARED_CLIENTS 32
#define HD_SHARED_FRAME (1920u*1080u*2u)
#define HD_SHARED_AUDIO 128
#define HD_SHARED_PCM 4096
#define HD_NO_FRAME 99
#include <mach/mach_time.h>
static inline uint64_t hd_shared_now(void) { mach_timebase_info_data_t t;mach_timebase_info(&t);return (uint64_t)((long double)mach_absolute_time()*t.numer/t.denom); }
/* Other processes can publish a heartbeat after the caller sampled now. */
static inline uint64_t hd_shared_age(uint64_t now,uint64_t then) { return now>=then?now-then:0; }
typedef struct {
 _Atomic int pid;
 _Atomic unsigned reading;
 _Atomic uint64_t heartbeat;
} hd_lease;
typedef struct { hd_mode mode;uint64_t timestamp;uint32_t bytes;uint8_t data[HD_SHARED_FRAME];uint32_t source_scan; } hd_shared_frame;
/* source_scan consumes the v1 frame's four trailing padding bytes. Existing
 * clients retain identical offsets/size and ignore the additional metadata. */
_Static_assert(sizeof(hd_shared_frame)==HD_SHARED_FRAME+40,"shared frame ABI changed");
typedef struct { _Atomic uint64_t serial;uint64_t timestamp;uint32_t bytes,rate;uint8_t data[HD_SHARED_PCM]; } hd_shared_pcm;
typedef struct {
 _Atomic uint64_t magic;
 uint32_t version,size;
 _Atomic int service_pid;
 _Atomic uint64_t heartbeat;
 _Atomic unsigned status;
 _Atomic uint64_t video_token,audio_serial;
 _Atomic unsigned active_clients;
 _Atomic uint64_t dropped_video;
 hd_lease clients[HD_SHARED_CLIENTS];
 hd_shared_frame frames[3];
 hd_shared_pcm audio[HD_SHARED_AUDIO];
} hd_shared_map;
static inline int hd_shared_paths(char *root,size_t capacity) {
 const char *test=getenv("HD60S_TEST_NAMESPACE");
 if(test && *test) {
  if(strlen(test)>32)return -1;
  for(const char *p=test;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'))return -1;
 }
 snprintf(root,capacity,"/tmp/hd60s-%u%s%s",getuid(),test&&*test?"-":"",test&&*test?test:"");
 if(mkdir(root,0700) && access(root,F_OK))return -1;
 struct stat st;if(lstat(root,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077))return -1;
 return 0;
}
/* The running service's build and pid, in a file whose format never changes,
 * so a future client can always read what an older one wrote. Written before
 * the map is published, so a visible service_pid implies this file is there. */
static inline int hd_service_identity_write(const char *root,unsigned build,int pid) {
 char path[220];snprintf(path,sizeof path,"%s/service.id",root);
 char text[64];int length=snprintf(text,sizeof text,"%u %d\n",build,pid);
 int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW,0600);
 if(fd<0)return -1;
 int written=(int)write(fd,text,(size_t)length);close(fd);
 return written==length?0:-1;
}
static inline int hd_service_identity_read(const char *root,unsigned *build,int *pid) {
 char path[220];snprintf(path,sizeof path,"%s/service.id",root);
 int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return -1;
 char text[64];ssize_t got=read(fd,text,sizeof text-1);close(fd);
 if(got<=0)return -1;
 text[got]=0;
 unsigned parsed_build=0;int parsed_pid=0;
 if(sscanf(text,"%u %d",&parsed_build,&parsed_pid)!=2 || parsed_pid<=0)return -1;
 *build=parsed_build;*pid=parsed_pid;return 0;
}
/* The oldest service build still allowed to own the card. A client raises it to
 * its own build, and a service that reads a higher number stands down instead of
 * taking the card, so an old client restarting its old service cannot win the
 * race against an updated one. */
static inline unsigned hd_min_build_read(const char *root) {
 char path[220];snprintf(path,sizeof path,"%s/min-build",root);
 int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return 0;
 char text[32];ssize_t got=read(fd,text,sizeof text-1);close(fd);
 if(got<=0)return 0;
 text[got]=0;
 unsigned value=0;
 return sscanf(text,"%u",&value)==1?value:0;
}
static inline void hd_min_build_write(const char *root,unsigned build) {
 char path[220];snprintf(path,sizeof path,"%s/min-build",root);
 char text[32];int length=snprintf(text,sizeof text,"%u\n",build);
 int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW,0600);
 if(fd<0)return;
 (void)!write(fd,text,(size_t)length);close(fd);
}
static inline const char *hd_shared_status_text(unsigned state) {
 switch(state) {
 case HD_DISCONNECTED:return "Plug in your HD60 S using a USB 3 cable.";
 case HD_INITIALIZING:return "Preparing shared capture…";
 case HD_NO_SIGNAL:return "HD60 S connected. Check your HDMI source and cable.";
 case HD_BUSY:return "Another direct-capture driver owns the card. Close it to use shared capture.";
 case HD_LIVE:return "Receiving shared video and audio";
 default:return "Shared capture interrupted. Retrying automatically…";
 }
}
