/* SPDX-License-Identifier: GPL-2.0-only
 * Native macOS transport and parser. Protocol reference: Doug Brown's
 * GPL-2.0 hd60s-linux-driver (2026). No hd60s-arm64 code used.
 */
#include "capture.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#define RING 64
#define CHUNK 131072

typedef struct state state;
typedef struct { state *s; uint8_t *data; UInt32 size; IOReturn result; bool ready; } transfer;
struct state {
 IOUSBDeviceInterface320 **dev;
 IOUSBInterfaceInterface300 **usb;
 CFRunLoopSourceRef events;
 hd_callbacks cb;
 hd_mode mode;
 bool active,gate,opened,blank,frame_started,interlaced,detected_interlaced;
 transfer ring[RING]; unsigned pending,next;
 uint8_t header[4],audio[1020],pcm[4096],*frame;
 unsigned pcm_n;
 unsigned header_n,remaining,kind,line_n,rows,audio_n;
 uint64_t epoch,audio_epoch,delivered,frames,samples,bytes,bad,partial;
 bool failed;
 unsigned suspect; uint64_t suspect_frames;
 hd_status last_status;
 int product;
};
static uint64_t now_ns(void) { mach_timebase_info_data_t t; mach_timebase_info(&t); return (uint64_t)((long double)mach_absolute_time()*t.numer/t.denom); }
static void logmsg(state *s,const char *fmt,...) {
 char text[512]; va_list ap; va_start(ap,fmt); vsnprintf(text,sizeof text,fmt,ap); va_end(ap);
 if(s->cb.message) s->cb.message(s->cb.context,text); else fprintf(stderr,"%s\n",text);
}
static void status_event(state *s,hd_status value,const char *detail) {
 s->last_status=value;
 if(s->cb.status) s->cb.status(s->cb.context,value,detail);
}
static IOReturn control(state *s,bool in,unsigned req,unsigned val,unsigned index,void *data,unsigned len) {
 IOUSBDevRequestTO r={.bmRequestType=in?0xc0:0x40,.bRequest=req,.wValue=val,.wIndex=index,.wLength=len,.pData=data,.noDataTimeout=5000,.completionTimeout=5000};
 IOReturn e=(*s->dev)->DeviceRequestTO(s->dev,&r);
 if(!e && in && r.wLenDone!=len) e=kIOReturnUnderrun;
 if(e) logmsg(s,"Control %s %02x value %04x index %04x: 0x%08x",in?"IN":"OUT",req,val,index,e);
 return e;
}
static int write_reg(state *s,unsigned reg,uint8_t value) { return control(s,false,0xc0,0x64,reg,&value,1); }
static int disarm(state *s) { return control(s,false,0xc6,0,0x100,NULL,0); }
static int open_device(state *s) {
 io_iterator_t it=0; IOReturn e=IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&it);
 if(e) return -1;
 io_service_t service;
 while((service=IOIteratorNext(it))) {
  CFTypeRef v=IORegistryEntryCreateCFProperty(service,CFSTR("idVendor"),NULL,0);
  CFTypeRef p=IORegistryEntryCreateCFProperty(service,CFSTR("idProduct"),NULL,0);
  int vid=0,pid=0;
  if(v && CFGetTypeID(v)==CFNumberGetTypeID()) CFNumberGetValue(v,kCFNumberIntType,&vid);
  if(p && CFGetTypeID(p)==CFNumberGetTypeID()) CFNumberGetValue(p,kCFNumberIntType,&pid);
  if(v) CFRelease(v); if(p) CFRelease(p);
  /* Built and tested on revision 4 (0x0076). Revisions 1 to 3 are tried with
   * the same protocol in case they share it; nobody has confirmed that, so a
   * failure on one of them is reported as that rather than as a fault. */
  if(vid!=0x0fd9 || (pid!=0x0076 && pid!=0x0074 && pid!=0x005e && pid!=0x004f)) { IOObjectRelease(service); continue; }
  if(pid!=0x0076) logmsg(s,"HD60 S %04x is an untested revision; trying the revision-4 protocol",pid);
  s->product=pid;
  IOCFPlugInInterface **plugin=NULL; SInt32 score=0;
  e=IOCreatePlugInInterfaceForService(service,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&plugin,&score);
  IOObjectRelease(service);
  if(e || !plugin) { status_event(s,HD_ERROR,"USB access is unavailable. Close other capture apps or reconnect the card."); logmsg(s,"Cannot access USB device: 0x%08x",e); break; }
  HRESULT hr=(*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320),(void **)&s->dev);
  (*plugin)->Release(plugin);
  if(hr || !s->dev) break;
  e=(*s->dev)->USBDeviceOpen(s->dev);
  if(e) { status_event(s,e==kIOReturnExclusiveAccess?HD_BUSY:HD_ERROR,"Close another application using the card, then retry."); logmsg(s,"Device open: 0x%08x (close other capture applications)",e); break; }
  s->opened=true;
  UInt8 cfg=0; e=(*s->dev)->GetConfiguration(s->dev,&cfg);
  if(e) break;
  if(!cfg) { e=(*s->dev)->SetConfiguration(s->dev,1); if(e) break; }
  IOUSBFindInterfaceRequest request={0xffff,0xffff,0xffff,0xffff};
  io_iterator_t interfaces=0;
  e=(*s->dev)->CreateInterfaceIterator(s->dev,&request,&interfaces);
  if(e) break;
  io_service_t iface;
  while((iface=IOIteratorNext(interfaces))) {
   plugin=NULL;
   e=IOCreatePlugInInterfaceForService(iface,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&plugin,&score);
   IOObjectRelease(iface);
   if(e || !plugin) continue;
   IOUSBInterfaceInterface300 **u=NULL;
   hr=(*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300),(void **)&u);
   (*plugin)->Release(plugin);
   if(hr || !u) continue;
   UInt8 n=255; (*u)->GetInterfaceNumber(u,&n);
   if(n==0) { s->usb=u; break; } (*u)->Release(u);
  }
  IOObjectRelease(interfaces);
  if(s->usb) {
   e=(*s->usb)->USBInterfaceOpen(s->usb);
   if(!e) { IOObjectRelease(it); return 0; }
   status_event(s,e==kIOReturnExclusiveAccess?HD_BUSY:HD_ERROR,"The capture interface is unavailable. Close other capture apps.");
   logmsg(s,"Interface open: 0x%08x",e);
   (*s->usb)->Release(s->usb); s->usb=NULL;
  }
  break;
 }
 IOObjectRelease(it); logmsg(s,"No accessible HD60 S"); return -1;
}
static int init_device(state *s,const atomic_bool *stop) {
 usleep(600000);
 uint8_t cold[]={0xb8,0x22,0,0,0,0xc0,0,0,0x50,0xca};
 IOReturn e=control(s,false,0xec,0,0,cold,sizeof cold);
 if(e && e!=kIOUSBPipeStalled) return -1;
 if(control(s,false,0xc1,0xc039,0,NULL,0)) return -1;
 usleep(2500);
 if(control(s,false,0xc1,0x4134,0,NULL,0)) return -1;
 usleep(12000);
 uint8_t pin=0;
 for(int i=0;i<100 && !atomic_load(stop);i++) {
  if(control(s,true,0xc1,0x39,0,&pin,1) || pin==1) break;
  usleep(10000);
 }
 uint8_t ready[]={0xab,3,0x12,0x34,0x57},reply[3]={0};
 if(control(s,false,0xc0,0x5066,0,ready,sizeof ready)) return -1;
 bool ok=false;
 for(int i=0;i<200 && !atomic_load(stop);i++) {
  if(control(s,true,0xc0,0x5066,0,reply,3)) return -1;
  if(reply[0]==0x51 && reply[1]==0x10) { ok=true; break; }
  usleep(101000);
 }
 if(!ok) { logmsg(s,"MCU not ready: %02x %02x %02x",reply[0],reply[1],reply[2]); return -1; }
 logmsg(s,"MCU ready");
 e=control(s,false,0xc2,0,0,NULL,0); if(e && e!=kIOUSBPipeStalled) return -1;
 e=control(s,false,0xc7,0x64,0,NULL,0); if(e && e!=kIOUSBPipeStalled) return -1;
 if(disarm(s) || disarm(s)) return -1;
 uint8_t picture[]={128,128,128,128};
 if(control(s,false,0xc0,0x64,0x13,picture,4) || write_reg(s,0x3a,0)) return -1;
 usleep(6000);
 if(write_reg(s,0x3b,128) || write_reg(s,0x12,0)) return -1;
 return 0;
}
static unsigned le16(const uint8_t *d) { return d[0]|(d[1]<<8); }
enum { MODE_READY=0, MODE_ABSENT=1 };
static int decode_mode(state *s,hd_mode *m,const uint8_t b[32]) {
 unsigned w=le16(b+10),h=le16(b+8),ht=le16(b+6),vt=le16(b+4);
 if(!w && !h) return 1;
 if(w<320 || w>1920 || h<200 || h>1080 || w%2 || ht<w || vt<h || b[12]<20 || b[12]>130) {
  logmsg(s,"Invalid/unsupported timing: %ux%u @ %u, totals %ux%u",w,h,b[12],ht,vt); return -1;
 }
 /* Revision 4 reports active lines per FIELD and field rate (upstream
  * hd60s-core.c). Publish bob-deinterlaced progressive frames at that rate. */
 bool interlaced=(b[13]&1) || h==240 || h==288 || h==540;
 if(interlaced && h>540) { logmsg(s,"Unsupported interlaced field height: %u",h);return -1; }
 s->detected_interlaced=interlaced;
 if(interlaced) h*=2;
 if(b[13]&8) { logmsg(s,"HDCP-protected input cannot be captured"); return -1; }
 *m=(hd_mode){.width=w,.height=h,.rate=b[12],.fractional=!!(b[13]&2),.audio_rate=b[14]*1000,.bt709=((b[13]>>4)&3)!=2};
 if(m->audio_rate!=32000 && m->audio_rate!=44100 && m->audio_rate!=48000 && m->audio_rate!=96000 && m->audio_rate!=192000) m->audio_rate=0;
 return 0;
}
static int detect(state *s,hd_mode *m) {
 uint8_t b[32]={0};
 if(control(s,true,0xc0,0x64,0,b,32)) return -1;
 return decode_mode(s,m,b);
}
/* Rebase after lost USB data instead of letting nominal clocks fall permanently
 * behind. Keep normal packet jitter out of the clock, and never move backwards. */
static uint64_t paced_time(uint64_t *epoch,uint64_t offset,uint64_t now) {
 uint64_t pts=*epoch+offset;
 if(now>pts && now-pts>100000000ULL) { *epoch+=now-pts-30000000ULL;pts=*epoch+offset; }
 return pts;
}
static uint64_t video_time(state *s) {
 return paced_time(&s->epoch,(uint64_t)((long double)s->frames*1000000000*(s->mode.fractional?1001:1000)/(s->mode.rate*1000)),now_ns());
}
static void parse(state *s,const uint8_t *data,size_t count) {
 s->bytes+=count;
 while(count) {
  if(!s->remaining) {
   size_t take=4-s->header_n; if(take>count) take=count;
   memcpy(s->header+s->header_n,data,take); s->header_n+=take; data+=take; count-=take;
   if(s->header_n<4) break;
   s->header_n=0; const uint8_t *h=s->header;
   if(h[0]!=255 || h[1]!=0) { ++s->bad; s->frame_started=false; continue; }
   if(h[2]==255) { s->remaining=h[3]*4; s->kind=2; s->audio_n=0; continue; }
   if(h[2]!=0 || (h[3]!=0x80 && h[3]!=0xab && (!s->interlaced || (h[3]!=0xc7 && h[3]!=0xec)))) { ++s->bad; s->frame_started=false; continue; }
   bool blank=(h[3]&0x20)!=0;
   if(blank && !s->blank && s->frame_started) {
    if(s->rows==s->mode.height/(s->interlaced?2:1)) {
     if(!s->frames) status_event(s,HD_LIVE,"Receiving video and audio");
     if(s->cb.scan) s->cb.scan(s->cb.context,s->interlaced?2:1);
     if(s->cb.video) s->cb.video(s->cb.context,s->frame,&s->mode,video_time(s));
     ++s->frames; ++s->delivered;
    } else { ++s->partial; ++s->frames; }
    s->frame_started=false;
   }
   if(!blank && s->blank) {
    if(!s->epoch) s->audio_epoch=s->epoch=now_ns();
    s->frame_started=true; s->rows=0;
   }
   s->blank=blank; s->line_n=0;
   s->remaining=s->mode.width*2; s->kind=blank?3:1;
   continue;
  }
  size_t take=count<s->remaining?count:s->remaining;
  if(s->kind==1 && s->frame_started && s->rows<s->mode.height/(s->interlaced?2:1)) {
   size_t stride=s->mode.width*2;
   size_t row=s->rows*(s->interlaced?2:1);
   memcpy(s->frame+row*stride+s->line_n,data,take);
   if(s->interlaced) memcpy(s->frame+(row+1)*stride+s->line_n,data,take);
  }
  if(s->kind==2) { memcpy(s->audio+s->audio_n,data,take); s->audio_n+=take; }
  s->line_n+=take; s->remaining-=take; data+=take; count-=take;
  if(!s->remaining) {
   if(s->kind==1) ++s->rows;
   if(s->kind==2 && s->epoch && s->mode.audio_rate) {
    uint64_t ts=s->epoch+(uint64_t)((long double)s->samples*1000000000/s->mode.audio_rate);
    (void)ts;
    unsigned target=(s->mode.audio_rate/100)*4;
    if(target>sizeof s->pcm) target=sizeof s->pcm;
    unsigned offset=0;
    while(offset<s->audio_n) {
     unsigned n=s->audio_n-offset;
     if(n>target-s->pcm_n) n=target-s->pcm_n;
     memcpy(s->pcm+s->pcm_n,s->audio+offset,n); s->pcm_n+=n; offset+=n;
     if(s->pcm_n==target) {
      uint64_t pts=paced_time(&s->audio_epoch,(uint64_t)((long double)s->samples*1000000000/s->mode.audio_rate),now_ns());
      if(s->cb.audio) s->cb.audio(s->cb.context,s->pcm,s->pcm_n,s->mode.audio_rate,pts);
      s->samples+=s->pcm_n/4; s->pcm_n=0;
     }
    }
   }
  }
 }
}
static IOReturn submit(transfer *t);
static void complete(void *ctx,IOReturn result,void *arg) {
 transfer *t=ctx; state *s=t->s;
 t->result=result; t->size=(UInt32)(uintptr_t)arg; t->ready=true; --s->pending;
 /* USB byte order is submission order, independent of callback delivery order. */
 while(s->ring[s->next].ready) {
  transfer *cur=&s->ring[s->next]; cur->ready=false;
  if(s->active) {
   if(cur->result || cur->size>CHUNK) { logmsg(s,"USB read failed: 0x%08x",cur->result); s->active=false; s->failed=true; }
   else parse(s,cur->data,cur->size);
   if(s->active && submit(cur)) { s->active=false; s->failed=true; }
  }
  s->next=(s->next+1)%RING;
 }
}
static IOReturn submit(transfer *t) {
 state *s=t->s;
 IOReturn e=(*s->usb)->ReadPipeAsyncTO(s->usb,1,t->data,CHUNK,3000,5000,complete,t);
 if(!e) ++s->pending; else logmsg(s,"Read submission failed: 0x%08x",e);
 return e;
}
int hd_capture(const hd_callbacks *cb,const atomic_bool *stop,double seconds) {
 state *s=calloc(1,sizeof *s); if(!s) return -1; s->cb=*cb;
 int rc=-1;
 if(open_device(s)) {
  if(!hd_device_product()) status_event(s,HD_DISCONNECTED,"Plug in your HD60 S using a USB 3 cable.");
  goto done;
 }
 status_event(s,HD_INITIALIZING,"Preparing your HD60 S…");
 if(init_device(s,stop)) {
  status_event(s,HD_ERROR,s->product==0x0076?"Initialization failed. Retrying; reconnect the card if this continues."
   :"This HD60 S revision did not answer the way revision 4 does, so it is probably not supported yet. Retrying.");
  goto done;
 }
 int detected=MODE_ABSENT;
 for(unsigned i=0;!atomic_load(stop);i++) {
  detected=detect(s,&s->mode); if(detected<=0) break;
  if(!i) status_event(s,HD_NO_SIGNAL,"HD60 S connected. Turn on your HDMI source and check its cable.");
  if(i%10==0) logmsg(s,"Waiting for HDMI signal");
  usleep(200000);
 }
 s->interlaced=s->detected_interlaced;
 if(detected || atomic_load(stop)) { if(detected<0) status_event(s,HD_ERROR,"Unable to read a supported HDMI mode. Try a progressive, unprotected source."); goto done; }
 logmsg(s,"Input: %ux%u @ %u%s, audio %u Hz, %s limited range",s->mode.width,s->mode.height,s->mode.rate,s->mode.fractional?"/1.001":"",s->mode.audio_rate,s->mode.bt709?"BT.709":"BT.601");
 if(s->interlaced) logmsg(s,"Interlaced input: bob deinterlacing each field to %ux%u progressive",s->mode.width,s->mode.height);
 s->frame=malloc((size_t)s->mode.width*s->mode.height*2); if(!s->frame) goto done;
 for(unsigned i=0;i<RING;i++) { s->ring[i].s=s; if(posix_memalign((void **)&s->ring[i].data,CHUNK,CHUNK)) goto done; }
 if((*s->usb)->CreateInterfaceAsyncEventSource(s->usb,&s->events)) goto done;
 CFRunLoopAddSource(CFRunLoopGetCurrent(),s->events,kCFRunLoopDefaultMode);
 if(disarm(s) || write_reg(s,0x10,1)) goto done;
 s->gate=true; usleep(105000);
 IOReturn e=(*s->usb)->SetAlternateInterface(s->usb,4);
 if(e) { logmsg(s,"Bulk alternate setting failed: 0x%08x",e); goto done; }
 s->active=true;
 for(unsigned i=0;i<RING;i++) if(submit(&s->ring[i])) { s->active=false; s->failed=true; break; }
 uint64_t start=now_ns(),report=start,poll=start,interval=2000000000ULL,progress=start,seen_bytes=0,frame_progress=start,seen_frames=0;
 while(s->active && !atomic_load(stop) && (seconds<=0 || now_ns()-start<seconds*1e9)) {
  CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.02,false);
  uint64_t now=now_ns();
  /* A source that renegotiates its output (a Wii loader launching a game, a
   * console changing mode) can leave the bulk pipe wedged while the timing
   * registers keep reporting the old mode perfectly. Nothing below notices
   * that, so capture sits live and silent until the process is restarted.
   * The byte counter is the one signal that always stops in that case. */
  if(s->bytes!=seen_bytes) { seen_bytes=s->bytes;progress=now; }
  else if(now-progress>2000000000ULL) {
   logmsg(s,"No USB data for 2 s; restarting capture");
   status_event(s,HD_INITIALIZING,"The HDMI source changed. Reconnecting…");
   s->failed=true;break;
  }
  if(s->delivered!=seen_frames) { seen_frames=s->delivered;frame_progress=now; }
  else if(now-frame_progress>3000000000ULL) {
   logmsg(s,"No complete video frames for 3 s; resetting capture");s->failed=true;break;
  }
  if(now-report>1000000000) { logmsg(s,"Frames %llu, partial %llu, bad headers %llu, USB %.1f MB, audio samples %llu",s->frames,s->partial,s->bad,s->bytes/1e6,s->samples); report=now; }
  if(now-poll>interval) {
   hd_mode current={0}; int status=detect(s,&current); poll=now;
   bool lost=status==MODE_ABSENT,bad_read=status<0;
   bool changed=!status && (s->detected_interlaced!=s->interlaced || current.width!=s->mode.width || current.height!=s->mode.height || current.rate!=s->mode.rate || current.fractional!=s->mode.fractional || current.audio_rate!=s->mode.audio_rate || current.bt709!=s->mode.bt709);
   /* The timing registers can read back zero or half-updated while the MCU
    * refreshes them, even with a perfectly stable source. Acting on a single
    * sample tears down a healthy capture, so confirm with quicker re-reads
    * before reporting signal loss or restarting. */
   if(lost || bad_read || changed) {
    if(!s->suspect) s->suspect_frames=s->frames;
    s->suspect++;
    uint64_t delivered=s->frames-s->suspect_frames;
    /* The bulk stream is the ground truth. A register reporting no signal while
     * complete frames keep arriving is the register being wrong, not the source
     * going away, so believe the frames. A changed or unreadable mode gets no
     * such benefit: those keep delivering frames at a geometry we can no longer
     * parse, so they still have to restart. */
    if(lost && delivered) {
     logmsg(s,"Ignored a no-signal timing read; %llu frames arrived while it claimed no signal",(unsigned long long)delivered);
     s->suspect=0; interval=2000000000ULL;
    } else if(s->suspect<3) {
     interval=250000000ULL;
    } else {
     if(lost) status_event(s,HD_NO_SIGNAL,"HDMI signal lost. Check your source and HDMI cable.");
     logmsg(s,"HDMI signal %s after %u confirmations, %llu frames delivered during them; restarting capture",
      lost?"lost":bad_read?"unreadable":"changed",s->suspect,(unsigned long long)delivered);
     s->failed=true; break;
    }
   } else {
    if(s->suspect) { logmsg(s,"Ignored a transient HDMI timing glitch (%u sample%s)",s->suspect,s->suspect==1?"":"s"); s->suspect=0; }
    interval=2000000000ULL;
   }
  }
 }
 rc=(!s->failed && s->frames>0)?0:-1;
 done:
 if(s->failed && s->last_status!=HD_NO_SIGNAL && !atomic_load(stop)) status_event(s,HD_ERROR,"Capture interrupted. Reconnecting automatically…");
 if(!atomic_load(stop) && !hd_device_product()) status_event(s,HD_DISCONNECTED,"Plug in your HD60 S using a USB 3 cable.");
 /* Drain the FX3 before canceling reads. Never issue control writes from callbacks. */
 s->active=false;
 if(s->usb && s->gate) {
  IOReturn end=(*s->usb)->SetAlternateInterface(s->usb,0);
  logmsg(s,"Stop alternate setting: 0x%08x",end);
  (*s->usb)->AbortPipe(s->usb,1);
  uint64_t deadline=now_ns()+6000000000ULL;
  while(s->pending && now_ns()<deadline) CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.02,false);
  usleep(35000);
  /* The gate has to come down even when the alternate setting could not be
   * restored: a card left armed keeps streaming into a pipe nobody reads, and
   * the next init_device then times out waiting for an MCU that is busy. The
   * pipe is cleared first so those control writes are not answered with the
   * stall that wedged the stream in the first place. */
  if(end) (*s->usb)->ClearPipeStallBothEnds(s->usb,1);
  disarm(s); write_reg(s,0x10,0);
 }
 if(s->events) { CFRunLoopRemoveSource(CFRunLoopGetCurrent(),s->events,kCFRunLoopDefaultMode); CFRelease(s->events); }
 if(s->usb) { (*s->usb)->USBInterfaceClose(s->usb); (*s->usb)->Release(s->usb); }
 /* A wedged FX3 may survive interface close/reopen. Reset this device on
  * failed streams before the service retries the full initialization sequence. */
 if(s->dev && s->opened && s->failed && !atomic_load(stop)) {
  IOReturn reset=(*s->dev)->ResetDevice(s->dev);
  logmsg(s,"Recovery USB reset: 0x%08x",reset);
 }
 if(s->dev) { if(s->opened) (*s->dev)->USBDeviceClose(s->dev); (*s->dev)->Release(s->dev); }
 logmsg(s,"Capture ended: %llu frames, %llu partial, %llu bad headers",s->frames,s->partial,s->bad);
 /* Closing the interface cancels outstanding reads; avoid releasing callback storage if delivery did not drain. */
 if(!s->pending) { for(unsigned i=0;i<RING;i++) free(s->ring[i].data); free(s->frame); free(s); }
 return rc;
}
