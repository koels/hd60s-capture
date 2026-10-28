/* SPDX-License-Identifier: GPL-2.0-only */
#include "shared-internal.h"
#include <sys/mman.h>
#include <sys/file.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>
static atomic_bool ending=false,core_stop=true,core_finished=true;
static hd_shared_map *shared;
static uint64_t frame_serial;
static void terminate_service(int number) { (void)number;atomic_store(&ending,true); }
static void status(void *ctx,hd_status value,const char *detail) { (void)ctx;(void)detail;if(value==HD_LIVE)return;atomic_store_explicit(&shared->status,value,memory_order_release); }
static void video(void *ctx,const uint8_t *data,const hd_mode *mode,uint64_t timestamp) {
 (void)ctx;size_t bytes=(size_t)mode->width*mode->height*2;
 if(bytes>HD_SHARED_FRAME)return;
 uint64_t token=atomic_load_explicit(&shared->video_token,memory_order_relaxed);unsigned current=token?token%4:HD_NO_FRAME,index=HD_NO_FRAME;uint64_t now=hd_shared_now();
 for(unsigned candidate=0;candidate<3;candidate++) {
  if(candidate==current)continue;bool used=false;
  for(unsigned n=0;n<HD_SHARED_CLIENTS;n++) {
   hd_lease *lease=&shared->clients[n];
   if(atomic_load_explicit(&lease->pid,memory_order_acquire)>0 && hd_shared_age(now,atomic_load(&lease->heartbeat))<3000000000ULL && atomic_load_explicit(&lease->reading,memory_order_seq_cst)==candidate) { used=true;break; }
  }
  if(!used) { index=candidate;break; }
 }
 if(index==HD_NO_FRAME){atomic_fetch_add(&shared->dropped_video,1);return;}
 hd_shared_frame *frame=&shared->frames[index];frame->mode=*mode;frame->timestamp=timestamp;frame->bytes=(uint32_t)bytes;memcpy(frame->data,data,bytes);
 atomic_store_explicit(&shared->video_token,(++frame_serial)*4+index,memory_order_seq_cst);
 atomic_store(&shared->status,HD_LIVE);
}
static void audio(void *ctx,const uint8_t *data,size_t bytes,unsigned rate,uint64_t timestamp) {
 (void)ctx;if(bytes>HD_SHARED_PCM || bytes%4)return;
 uint64_t sequence=atomic_load(&shared->audio_serial)+1;hd_shared_pcm *block=&shared->audio[sequence%HD_SHARED_AUDIO];
 atomic_store_explicit(&block->serial,0,memory_order_seq_cst);
 block->timestamp=timestamp;block->bytes=(uint32_t)bytes;block->rate=rate;memcpy(block->data,data,bytes);
 atomic_store_explicit(&block->serial,sequence,memory_order_release);atomic_store_explicit(&shared->audio_serial,sequence,memory_order_release);
}
static void message(void *ctx,const char *text) {
 (void)ctx;if(!strncmp(text,"Frames ",7))return;fprintf(stderr,"[HD60 S service] %s\n",text);
}
static void *capture(void *unused) {
 (void)unused;hd_callbacks cb={.video=video,.audio=audio,.message=message,.status=status};
 while(!atomic_load(&core_stop)) {
  int product=hd_device_product();
  if(!product)status(NULL,HD_DISCONNECTED,"");
  else if(product!=0x76)status(NULL,HD_ERROR,"");
  else hd_capture(&cb,&core_stop,0);
  for(unsigned i=0;i<20 && !atomic_load(&core_stop);i++)usleep(100000);
 }
 atomic_store(&core_finished,true);return NULL;
}
int main(int argc,char **argv) {
 bool synthetic=argc==2 && !strcmp(argv[1],"--synthetic");
 bool daemonize=argc==2 && !strcmp(argv[1],"--daemon");
 if(argc>1 && !synthetic && !daemonize)return 2;
 if(daemonize) { pid_t child=fork();if(child<0)return 1;if(child>0)return 0;setsid(); }
 if(synthetic && !getenv("HD60S_TEST_NAMESPACE"))return 2;
 char root[160],path[220];if(hd_shared_paths(root,sizeof root))return 1;
 // An updated client has been here and wants a newer service than this one.
 if(HD_SHARED_BUILD<hd_min_build_read(root)) { fprintf(stderr,"[HD60 S service] Build %u stands down; %u or newer is wanted\n",(unsigned)HD_SHARED_BUILD,hd_min_build_read(root));return 0; }
 snprintf(path,sizeof path,"%s/service.lock",root);int lock=open(path,O_CREAT|O_RDWR|O_NOFOLLOW,0600);if(lock<0)return 1;
 if(flock(lock,LOCK_EX|LOCK_NB))return 0; // Another service already owns this namespace.
 // Before anything is visible to a client, say which build owns the card.
 hd_service_identity_write(root,HD_SHARED_BUILD,getpid());
 snprintf(path,sizeof path,"%s/stream-v1",root);int fd=open(path,O_CREAT|O_RDWR|O_NOFOLLOW,0600);if(fd<0 || ftruncate(fd,sizeof *shared))return 1;
 shared=mmap(NULL,sizeof *shared,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(shared==MAP_FAILED)return 1;close(fd);
 memset(shared,0,sizeof *shared);shared->version=HD_SHARED_VERSION;shared->size=sizeof *shared;
 for(unsigned i=0;i<HD_SHARED_CLIENTS;i++)atomic_init(&shared->clients[i].reading,HD_NO_FRAME);
 atomic_store(&shared->service_pid,getpid());atomic_store(&shared->status,HD_INITIALIZING);atomic_store(&shared->heartbeat,hd_shared_now());atomic_store_explicit(&shared->magic,HD_SHARED_MAGIC,memory_order_release);
 signal(SIGTERM,terminate_service);signal(SIGINT,terminate_service);
 pthread_t thread;bool running=false;uint64_t idle=hd_shared_now(),last_frame=0,last_audio=0,epoch=hd_shared_now(),sample=0;
 uint8_t *fake=NULL;if(synthetic){fake=malloc(320*240*2);if(!fake)return 1;for(unsigned i=0;i<320*240*2;i++)fake[i]=i%2?128:80;}
 while(!atomic_load(&ending)) {
  uint64_t now=hd_shared_now();atomic_store(&shared->heartbeat,now);unsigned clients=0;
  for(unsigned i=0;i<HD_SHARED_CLIENTS;i++) {
   hd_lease *lease=&shared->clients[i];int pid=atomic_load(&lease->pid);
   if(pid>0) {
    if(hd_shared_age(now,atomic_load(&lease->heartbeat))>3000000000ULL && (kill(pid,0) || hd_shared_age(now,atomic_load(&lease->heartbeat))>10000000000ULL)) { int expected=pid;atomic_compare_exchange_strong(&lease->pid,&expected,0); }
    else clients++;
   }
  }
  atomic_store(&shared->active_clients,clients);
  if(clients)idle=now;
  if(!synthetic) {
   if(running && atomic_load(&core_finished)){pthread_join(thread,NULL);running=false;}
   if(clients && !running) { atomic_store(&core_stop,false);atomic_store(&core_finished,false);status(NULL,HD_INITIALIZING,"");if(!pthread_create(&thread,NULL,capture,NULL))running=true;else status(NULL,HD_ERROR,""); }
   if(!clients && running)atomic_store(&core_stop,true);
  } else if(clients) {
   if(now-last_frame>=33333333ULL) { hd_mode m={.width=320,.height=240,.rate=30,.audio_rate=48000};video(NULL,fake,&m,now);last_frame=now; }
   if(now-last_audio>=10000000ULL) {int16_t pcm[960];for(unsigned i=0;i<480;i++){int16_t v=(int16_t)(sin((sample+i)*2*3.141592653589793*440/48000)*2000);pcm[2*i]=pcm[2*i+1]=v;}audio(NULL,(uint8_t *)pcm,sizeof pcm,48000,epoch+sample*1000000000ULL/48000);sample+=480;last_audio=now;}
  }
  if(!clients && now-idle>20000000000ULL)break;
  usleep(5000);
 }
 atomic_store(&core_stop,true);if(running)pthread_join(thread,NULL);
 atomic_store(&shared->service_pid,0);atomic_store(&shared->heartbeat,0);munmap(shared,sizeof *shared);free(fake);close(lock);return 0;
}
