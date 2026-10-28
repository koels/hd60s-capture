/* SPDX-License-Identifier: GPL-2.0-only */
#include "shared.h"
#include "shared-internal.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <copyfile.h>
#include <signal.h>
extern char **environ;
static void report(const hd_callbacks *cb,unsigned status) { if(cb->status)cb->status(cb->context,status,hd_shared_status_text(status)); }
static int run_launchctl(char *const arguments[]) {
 posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
 posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
 posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);posix_spawn_file_actions_adddup2(&actions,1,2);
 posix_spawnattr_t attrs;posix_spawnattr_init(&attrs);posix_spawnattr_setflags(&attrs,POSIX_SPAWN_CLOEXEC_DEFAULT);
 pid_t pid;int error=posix_spawn(&pid,"/bin/launchctl",&actions,&attrs,arguments,environ);
 posix_spawnattr_destroy(&attrs);posix_spawn_file_actions_destroy(&actions);
 if(error)return -1;int status=0;while(waitpid(pid,&status,0)<0)if(errno!=EINTR)return -1;
 return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
/* The build of the bundle whose Contents/MacOS is dir: 0.4.7 is 407, as in the Makefile. */
static unsigned bundle_build(const char *dir) {
 char path[PATH_MAX];snprintf(path,sizeof path,"%s/../..",dir);
 CFURLRef url=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)path,(CFIndex)strlen(path),true);
 if(!url)return 0;
 CFBundleRef bundle=CFBundleCreate(NULL,url);CFRelease(url);
 if(!bundle)return 0;
 unsigned build=0,major=0,minor=0,patch=0;char text[32];
 CFTypeRef version=CFBundleGetValueForInfoDictionaryKey(bundle,kCFBundleVersionKey);
 if(version && CFGetTypeID(version)==CFStringGetTypeID() && CFStringGetCString(version,text,sizeof text,kCFStringEncodingUTF8)
    && sscanf(text,"%u.%u.%u",&major,&minor,&patch)==3) build=major*10000+minor*100+patch;
 CFRelease(bundle);return build;
}
static void consider(const char *dir,char *best,size_t capacity,unsigned *best_build) {
 char path[PATH_MAX];snprintf(path,sizeof path,"%s/hd60s-service",dir);
 if(access(path,X_OK))return;
 unsigned build=bundle_build(dir);
 if(build>*best_build && strlen(path)<capacity) { *best_build=build;strcpy(best,path); }
}
/* The service is the driver. OBS loads this client once and keeps it until it
 * restarts, so if it could only start the copy it shipped with, an updated app
 * would need OBS restarted to deliver a driver fix. Instead every client starts
 * the newest service installed anywhere, its own or the app's, and an app
 * update reaches OBS as a short gap in the picture rather than a restart. */
static unsigned newest_service(char *best,size_t capacity) {
 unsigned best_build=0;
 Dl_info info;char self[PATH_MAX];
 if(dladdr((void *)hd_shared_capture,&info) && info.dli_fname && realpath(info.dli_fname,self)) {
  char *slash=strrchr(self,'/');if(slash) { *slash=0;consider(self,best,capacity,&best_build); }
 }
 CFArrayRef apps=LSCopyApplicationURLsForBundleIdentifier(CFSTR("local.hd60s.capture"),NULL);
 if(apps) {
  for(CFIndex i=0;i<CFArrayGetCount(apps);i++) {
   char dir[PATH_MAX];
   if(CFURLGetFileSystemRepresentation(CFArrayGetValueAtIndex(apps,i),true,(UInt8 *)dir,sizeof dir-20)) {
    strcat(dir,"/Contents/MacOS");consider(dir,best,capacity,&best_build);
   }
  }
  CFRelease(apps);
 }
 return best_build;
}
static int start_service(const char *root) {
 // Synthetic tests never fall back to opening real USB hardware.
 if(getenv("HD60S_TEST_NAMESPACE"))return -1;
 char executable[PATH_MAX];unsigned build=newest_service(executable,sizeof executable);
 if(!build)return -1;
 char cached[220],plist[220],logfile[220],label[64];
 snprintf(cached,sizeof cached,"%s/service-%u",root,build);
 snprintf(plist,sizeof plist,"%s/launch-%u.plist",root,build);
 snprintf(logfile,sizeof logfile,"%s/service.log",root);
 snprintf(label,sizeof label,"local.hd60s.capture-service.%u",build);
 struct stat st;
 if(lstat(cached,&st)) {
  if(errno!=ENOENT)return -1;
  if(copyfile(executable,cached,NULL,COPYFILE_ALL|COPYFILE_EXCL) && errno!=EEXIST)return -1;
  if(chmod(cached,0700))return -1;
 }
 if(lstat(cached,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)||access(cached,X_OK))return -1;
 int fd=open(plist,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600);
 if(fd>=0) {
  CFStringRef binary=CFStringCreateWithCString(NULL,cached,kCFStringEncodingUTF8),log=CFStringCreateWithCString(NULL,logfile,kCFStringEncodingUTF8);
  const void *args[]={binary};CFArrayRef program=CFArrayCreate(NULL,args,1,&kCFTypeArrayCallBacks);
  const void *keys[]={CFSTR("Label"),CFSTR("ProgramArguments"),CFSTR("RunAtLoad"),CFSTR("KeepAlive"),CFSTR("ProcessType"),CFSTR("StandardOutPath"),CFSTR("StandardErrorPath")};
  CFStringRef job_label=CFStringCreateWithCString(NULL,label,kCFStringEncodingUTF8);
  const void *values[]={job_label,program,kCFBooleanTrue,kCFBooleanFalse,CFSTR("Interactive"),log,log};
  CFDictionaryRef job=CFDictionaryCreate(NULL,keys,values,7,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
  CFDataRef data=CFPropertyListCreateData(NULL,job,kCFPropertyListXMLFormat_v1_0,0,NULL);
  bool ok=data!=NULL;
  if(data) { const UInt8 *bytes=CFDataGetBytePtr(data);CFIndex remaining=CFDataGetLength(data);while(remaining>0){ssize_t n=write(fd,bytes,remaining);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}bytes+=n;remaining-=n;}CFRelease(data); }
  close(fd);CFRelease(job);CFRelease(program);CFRelease(binary);CFRelease(log);CFRelease(job_label);if(!ok)return -1;
 } else if(errno!=EEXIST)return -1;
 if(lstat(plist,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077))return -1;
 char domain[64],target[200];snprintf(domain,sizeof domain,"gui/%u",getuid());snprintf(target,sizeof target,"%s/%s",domain,label);
 char *bootstrap[]={"/bin/launchctl","bootstrap",domain,plist,NULL};
 (void)run_launchctl(bootstrap); // Already bootstrapped is expected on subsequent starts.
 char *kickstart[]={"/bin/launchctl","kickstart",target,NULL};return run_launchctl(kickstart);
}
/* An older service keeps the card and keeps its bugs, and a new client would
 * otherwise attach to it happily: the map's layout is the same, so nothing in
 * the attach path notices. Retire it first, then start ours.
 *
 * The map says who holds the card, and only that; the identity file says which
 * build, and is believed only when it names the same process, because a stale
 * file left by a service that has already exited would otherwise hide a live
 * older one. No identity for the holder means it predates the identity file,
 * which makes it older by definition. */
static int serving_pid(const char *root) {
 char path[220];snprintf(path,sizeof path,"%s/stream-v1",root);
 int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return 0;
 struct stat st;
 if(fstat(fd,&st) || st.st_uid!=getuid() || !S_ISREG(st.st_mode) || st.st_size!=sizeof(hd_shared_map)) { close(fd);return 0; }
 hd_shared_map *map=mmap(NULL,sizeof *map,PROT_READ,MAP_SHARED,fd,0);close(fd);
 if(map==MAP_FAILED)return 0;
 int pid=0;uint64_t beat=atomic_load(&map->heartbeat);
 if(atomic_load_explicit(&map->magic,memory_order_acquire)==HD_SHARED_MAGIC && beat && hd_shared_age(hd_shared_now(),beat)<3000000000ULL)
  pid=atomic_load(&map->service_pid);
 munmap(map,sizeof *map);
 return pid>0?pid:0;
}
static bool retire_older_service(const hd_callbacks *cb,const char *root) {
 int pid=serving_pid(root);
 if(pid<=0 || pid==getpid() || kill(pid,0)) return false;
 unsigned build=0;int identified=0;
 bool known=!hd_service_identity_read(root,&build,&identified) && identified==pid;
 if(known && build>=HD_SHARED_BUILD) return false;
 if(cb->message) {
  char text[200],which[64];
  if(known) snprintf(which,sizeof which,"build %u",build);
  else snprintf(which,sizeof which,"a build too old to name itself");
  snprintf(text,sizeof text,"The shared service is %s and this one is %u; taking the card over.",which,(unsigned)HD_SHARED_BUILD);
  cb->message(cb->context,text);
 }
 // Raise the floor first, so the old client restarting its old service loses.
 hd_min_build_write(root,HD_SHARED_BUILD);
 if(kill(pid,SIGTERM)) return false;
 for(int i=0;i<100 && !kill(pid,0);i++) usleep(50000);
 return kill(pid,0)!=0;
}
/* A deliberate downgrade: this build is below the floor and nothing is serving,
 * so the floor is a leftover from a version that is no longer installed. */
static void reclaim_floor_if_alone(const char *root) {
 if(hd_min_build_read(root)<=HD_SHARED_BUILD) return;
 if(serving_pid(root)>0) return;
 hd_min_build_write(root,HD_SHARED_BUILD);
}
static hd_shared_map *attach(const char *root,int *slot,int *pid_out) {
 char path[220];snprintf(path,sizeof path,"%s/stream-v1",root);int fd=open(path,O_RDWR|O_NOFOLLOW);if(fd<0)return NULL;
 struct stat st;if(fstat(fd,&st)||st.st_uid!=getuid()||(st.st_mode&077)||!S_ISREG(st.st_mode)||st.st_size!=sizeof(hd_shared_map)) { close(fd);return NULL; }
 hd_shared_map *map=mmap(NULL,sizeof *map,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);if(map==MAP_FAILED)return NULL;
 uint64_t now=hd_shared_now();int pid=atomic_load(&map->service_pid);
 if(atomic_load_explicit(&map->magic,memory_order_acquire)!=HD_SHARED_MAGIC || map->version!=HD_SHARED_VERSION || map->size!=sizeof *map || pid<=0 || !atomic_load(&map->heartbeat) || hd_shared_age(now,atomic_load(&map->heartbeat))>3000000000ULL) {munmap(map,sizeof *map);return NULL;}
 for(unsigned i=0;i<HD_SHARED_CLIENTS;i++) {
  int expected=0;
  if(atomic_compare_exchange_strong(&map->clients[i].pid,&expected,-getpid())) {
   atomic_store(&map->clients[i].reading,HD_NO_FRAME);atomic_store(&map->clients[i].heartbeat,now);atomic_store_explicit(&map->clients[i].pid,getpid(),memory_order_release);
   *slot=i;*pid_out=pid;return map;
  }
 }
 munmap(map,sizeof *map);return NULL;
}
int hd_shared_capture(const hd_callbacks *cb,const atomic_bool *stop,double seconds) {
 char root[160];if(hd_shared_paths(root,sizeof root)) { report(cb,HD_ERROR);return -1; }
 if(!getenv("HD60S_TEST_NAMESPACE")) { reclaim_floor_if_alone(root);retire_older_service(cb,root); }
 uint8_t *pixels=malloc(HD_SHARED_FRAME);if(!pixels)return -1;
 hd_shared_map *map=NULL;int slot=-1,service_pid=0;unsigned old_status=UINT_MAX;uint64_t video_token=0,audio_next=0,spawn_time=0,start=hd_shared_now(),frames=0;
 uint64_t stale=0,stale_report=0,worst_age=0;
 while(!atomic_load(stop) && (seconds<=0 || hd_shared_now()-start<seconds*1e9)) {
  uint64_t now=hd_shared_now();
  if(!map) {
   map=attach(root,&slot,&service_pid);
   if(!map) {
    if(old_status!=HD_INITIALIZING){report(cb,HD_INITIALIZING);old_status=HD_INITIALIZING;}
    if(!spawn_time || now-spawn_time>2000000000ULL) {
     if(start_service(root) && cb->message)cb->message(cb->context,"Shared service could not start; check the bundled hd60s-service executable.");spawn_time=now;
    }
    usleep(50000);continue;
   }
   video_token=0;audio_next=atomic_load_explicit(&map->audio_serial,memory_order_acquire)+1;old_status=UINT_MAX;
  }
  hd_lease *lease=&map->clients[slot];
  if(atomic_load(&map->service_pid)!=service_pid || atomic_load(&lease->pid)!=getpid() || hd_shared_age(now,atomic_load(&map->heartbeat))>3000000000ULL) {
   int expected=getpid();atomic_compare_exchange_strong(&lease->pid,&expected,0);munmap(map,sizeof *map);map=NULL;continue;
  }
  atomic_store(&lease->heartbeat,now);
  unsigned status=atomic_load_explicit(&map->status,memory_order_acquire);if(status>HD_ERROR)status=HD_ERROR;
  if(status!=old_status) {report(cb,status);old_status=status;}
  uint64_t token=atomic_load_explicit(&map->video_token,memory_order_seq_cst);
  if(status==HD_LIVE && token && token!=video_token) {
   unsigned index=token%4;
   if(index<3) {
    atomic_store_explicit(&lease->reading,index,memory_order_seq_cst);
    if(token==atomic_load_explicit(&map->video_token,memory_order_seq_cst)) {
     hd_shared_frame *f=&map->frames[index];hd_mode mode=f->mode;uint64_t timestamp=f->timestamp;unsigned bytes=f->bytes;
     if(mode.width>=320 && mode.width<=1920 && mode.height>=200 && mode.height<=1080 && !(mode.width%2) && bytes==(size_t)mode.width*mode.height*2 && bytes<=HD_SHARED_FRAME) {
      memcpy(pixels,f->data,bytes);atomic_store_explicit(&lease->reading,HD_NO_FRAME,memory_order_seq_cst);
      /* Frame timestamps are synthesized from the nominal frame rate, so lost frames
       * make them lag the real clock. Past one second every frame is discarded, which
       * looks exactly like a blank picture; say so instead of failing silently. */
      uint64_t age=hd_shared_age(now,timestamp);
      if(age<1000000000ULL) {
       if(stale && cb->message) {
        char text[192];snprintf(text,sizeof text,"Stale-frame drops ended after %llu frame%s; worst lag %.2f s",(unsigned long long)stale,stale==1?"":"s",worst_age/1e9);
        cb->message(cb->context,text);stale=0;worst_age=0;
       }
       if(cb->video)cb->video(cb->context,pixels,&mode,timestamp);frames++;
      } else {
       stale++;if(age>worst_age)worst_age=age;
       if(cb->message && now-stale_report>2000000000ULL) {
        char text[192];snprintf(text,sizeof text,"Dropping frames as stale: %llu so far, newest is %.2f s behind the clock",(unsigned long long)stale,age/1e9);
        cb->message(cb->context,text);stale_report=now;
       }
      }
      video_token=token;
     }
    }
    atomic_store_explicit(&lease->reading,HD_NO_FRAME,memory_order_seq_cst);
   }
  }
  uint64_t latest=atomic_load_explicit(&map->audio_serial,memory_order_acquire);
  if(status!=HD_LIVE)audio_next=latest+1;
  if(latest>=audio_next && latest-audio_next>=HD_SHARED_AUDIO) {
   if(cb->message)cb->message(cb->context,"Shared audio consumer fell behind; skipping stale samples.");audio_next=latest-HD_SHARED_AUDIO+1;
  }
  while(audio_next<=latest) {
   hd_shared_pcm *block=&map->audio[audio_next%HD_SHARED_AUDIO];uint8_t pcm[HD_SHARED_PCM];
   uint64_t serial=atomic_load_explicit(&block->serial,memory_order_acquire);
   if(serial==audio_next) {
    unsigned bytes=block->bytes,rate=block->rate;uint64_t timestamp=block->timestamp;
    if(bytes<=HD_SHARED_PCM && bytes%4==0 && rate>=8000 && rate<=192000) {
     memcpy(pcm,block->data,bytes);atomic_thread_fence(memory_order_acquire);
     if(atomic_load_explicit(&block->serial,memory_order_acquire)==serial && cb->audio)cb->audio(cb->context,pcm,bytes,rate,timestamp);
    }
   }
   audio_next++;
  }
  usleep(2000);
 }
 if(map) {atomic_store(&map->clients[slot].reading,HD_NO_FRAME);int expected=getpid();atomic_compare_exchange_strong(&map->clients[slot].pid,&expected,0);munmap(map,sizeof *map);}
 free(pixels);return frames?0:-1;
}
