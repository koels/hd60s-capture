/* SPDX-License-Identifier: GPL-2.0-only */
#include <obs-module.h>
#include <util/platform.h>
#include <media-io/video-scaler.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include "capture.h"
#include "shared.h"
#include "processing.h"
#include "display-aspect.h"
#include "status-frame.h"
OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void) { return "Native HD60 S video and audio capture"; }
typedef struct { unsigned width,height,fps; int matrix,range,format,aspect; double gain; bool mute,limiter,release_hidden; } settings;
struct source {
 obs_source_t *source; pthread_t thread; pthread_mutex_t lock; atomic_bool stop; bool started;
 settings config; hd_mode input; unsigned source_scan; hd_audio_state audio_state;
 float pcm[2048]; uint64_t next_video,report; float peak; uint64_t clipped,samples;
 video_scaler_t *scaler; struct obs_source_frame *converted;
 struct video_scale_info old_in,old_out;
 pthread_mutex_t output_lock;
 hd_status connection;
 uint64_t last_video,status_time;
 unsigned last_width,last_height,status_width,status_height;
 hd_status drawn_status;
 uint8_t *status_pixels;

};
static const char *name(void *unused) { (void)unused; return "HD60 S Native"; }
static void message(void *ctx,const char *text) { (void)ctx; blog(LOG_INFO,"[HD60 S Native] %s",text); }
static settings snapshot(struct source *s) { pthread_mutex_lock(&s->lock); settings c=s->config; pthread_mutex_unlock(&s->lock); return c; }
static const char *status_name(hd_status v) {
 switch(v) {
 case HD_DISCONNECTED:return "disconnected";case HD_INITIALIZING:return "starting up";
 case HD_NO_SIGNAL:return "no signal";case HD_LIVE:return "live";
 case HD_BUSY:return "busy";default:return "error";
 }
}
/* Caller holds s->lock. Every visible blank is a transition through here, so log
 * each one with its cause; that is what identifies an unexplained blank later. */
static void set_connection(struct source *s,hd_status value,const char *why) {
 if(s->connection==value) return;
 uint64_t now=os_gettime_ns();
 if(value!=HD_LIVE && s->connection==HD_LIVE)
  blog(LOG_INFO,"[HD60 S Native] Status live -> %s (%s) after %.1f s of video",status_name(value),why,(now-s->last_video)/1e9);
 else if(value==HD_LIVE)
  blog(LOG_INFO,"[HD60 S Native] Status %s -> live (%s)",status_name(s->connection),why);
 else
  blog(LOG_INFO,"[HD60 S Native] Status %s -> %s (%s)",status_name(s->connection),status_name(value),why);
 s->connection=value;
}
static void reset_scaler(struct source *s) {
 if(s->scaler) video_scaler_destroy(s->scaler); s->scaler=NULL;
 if(s->converted) obs_source_frame_destroy(s->converted); s->converted=NULL;
}
static void show_status(struct source *s) {
 pthread_mutex_lock(&s->output_lock);
 pthread_mutex_lock(&s->lock);
 hd_status state=s->connection;settings c=s->config;
 unsigned w=c.width?c.width:s->last_width?s->last_width:1280;
 unsigned h=c.height?c.height:s->last_height?s->last_height:720;
 if(s->input.width)hd_display_size(s->input.width,s->input.height,c.width,c.height,c.aspect,&w,&h);
 pthread_mutex_unlock(&s->lock);
 if(state!=HD_LIVE) {
  if(!s->status_pixels || w!=s->status_width || h!=s->status_height || state!=s->drawn_status) {
   /* The service publishes a status, not its wording, so a USB 2.0 link would
    * read as a generic fault. Only the words come from IOKit here; the state
    * stays the service's, so this cannot flip the source between states. */
   bool slow=state==HD_ERROR && hd_slow_link();
   free(s->status_pixels);s->status_pixels=slow?hd_status_frame_text(w,h,state,NULL,HD_SLOW_LINK_DETAIL):hd_status_frame(w,h,state);s->status_width=w;s->status_height=h;s->drawn_status=state;
  }
  if(s->status_pixels) {
   struct obs_source_frame frame={0};frame.data[0]=s->status_pixels;frame.linesize[0]=w*4;frame.width=w;frame.height=h;
   frame.format=VIDEO_FORMAT_BGRA;frame.full_range=true;frame.timestamp=os_gettime_ns();obs_source_output_video(s->source,&frame);
  }
 }
 pthread_mutex_unlock(&s->output_lock);
}
static void connection_status(void *ctx,hd_status value,const char *detail) {
 struct source *s=ctx;(void)detail;
 if(value==HD_LIVE) return; /* Wait for an actual delivered frame. */
 pthread_mutex_lock(&s->lock);set_connection(s,value,"reported by capture service");pthread_mutex_unlock(&s->lock);show_status(s);
}
static void tick(void *ctx,float seconds) {
 (void)seconds;struct source *s=ctx;uint64_t now=os_gettime_ns();
 pthread_mutex_lock(&s->lock);
 if(now-s->status_time<500000000ULL) { pthread_mutex_unlock(&s->lock);return; }
 s->status_time=now;
 /* The capture service owns device presence and publishes it through
  * connection_status. Polling IOKit here as a second source of truth made the
  * source flip to a status frame whenever the two momentarily disagreed. */
 if(s->connection==HD_LIVE && now-s->last_video>2000000000ULL) set_connection(s,HD_INITIALIZING,"no frame delivered for 2 s");
 pthread_mutex_unlock(&s->lock);show_status(s);
}
static void scan(void *ctx,unsigned value) {
 struct source *s=ctx;pthread_mutex_lock(&s->lock);s->source_scan=value;pthread_mutex_unlock(&s->lock);
}
static void video(void *ctx,const uint8_t *data,const hd_mode *m,uint64_t timestamp) {
 struct source *s=ctx; settings c=snapshot(s);
 pthread_mutex_lock(&s->lock); s->input=*m; pthread_mutex_unlock(&s->lock);
 if(!hd_frame_due(timestamp,c.fps,&s->next_video)) return;
 enum video_colorspace matrix=c.matrix==1?VIDEO_CS_601:c.matrix==2?VIDEO_CS_709:m->bt709?VIDEO_CS_709:VIDEO_CS_601;
 enum video_range_type range=c.range==2?VIDEO_RANGE_FULL:VIDEO_RANGE_PARTIAL;
 unsigned width,height;
 hd_display_size(m->width,m->height,c.width,c.height,c.aspect,&width,&height);
 enum video_format format=c.format==1?VIDEO_FORMAT_NV12:c.format==2?VIDEO_FORMAT_BGRA:VIDEO_FORMAT_YUY2;
 struct obs_source_frame f={0},*out=&f;
 f.data[0]=(uint8_t *)data; f.linesize[0]=m->width*2; f.width=m->width; f.height=m->height; f.format=VIDEO_FORMAT_YUY2;
 if(width!=m->width || height!=m->height || format!=VIDEO_FORMAT_YUY2) {
  struct video_scale_info input={.format=VIDEO_FORMAT_YUY2,.width=m->width,.height=m->height,.range=range,.colorspace=matrix};
  struct video_scale_info output={.format=format,.width=width,.height=height,.range=format==VIDEO_FORMAT_BGRA?VIDEO_RANGE_FULL:range,.colorspace=matrix};
  if(!s->scaler || memcmp(&input,&s->old_in,sizeof input) || memcmp(&output,&s->old_out,sizeof output)) {
   reset_scaler(s); s->old_in=input; s->old_out=output;
   if(video_scaler_create(&s->scaler,&output,&input,VIDEO_SCALE_BICUBIC)!=VIDEO_SCALER_SUCCESS) { message(s,"Cannot create output scaler"); return; }
   s->converted=obs_source_frame_create(format,width,height);
   if(!s->converted || !s->converted->data[0]) { reset_scaler(s); return; }
  }
  const uint8_t *planes[MAX_AV_PLANES]={data};
  if(!video_scaler_scale(s->scaler,s->converted->data,s->converted->linesize,planes,f.linesize)) return;
  out=s->converted;
 }
 out->timestamp=timestamp; out->full_range=format==VIDEO_FORMAT_BGRA || range==VIDEO_RANGE_FULL;
 video_format_get_parameters(matrix,out->full_range?VIDEO_RANGE_FULL:VIDEO_RANGE_PARTIAL,out->color_matrix,out->color_range_min,out->color_range_max);
 pthread_mutex_lock(&s->output_lock);
 pthread_mutex_lock(&s->lock);set_connection(s,HD_LIVE,"frame delivered");s->last_video=os_gettime_ns();s->last_width=out->width;s->last_height=out->height;pthread_mutex_unlock(&s->lock);
 obs_source_output_video(s->source,out);
 pthread_mutex_unlock(&s->output_lock);
}
static void audio(void *ctx,const uint8_t *data,size_t bytes,unsigned rate,uint64_t timestamp) {
 struct source *s=ctx; settings c=snapshot(s);
 if(bytes/2>sizeof(s->pcm)/sizeof(s->pcm[0])) return;
 hd_audio_process(&s->audio_state,data,s->pcm,bytes/2,c.gain,c.mute,c.limiter,rate);
 pthread_mutex_lock(&s->lock);
 if(s->audio_state.peak>s->peak) s->peak=s->audio_state.peak;
 s->clipped=s->audio_state.clipped; s->samples=s->audio_state.samples;
 pthread_mutex_unlock(&s->lock);
 if(timestamp>s->report+5000000000ULL) {
  blog(LOG_INFO,"[HD60 S Native] Raw audio peak %.1f dBFS, near-full-scale samples %llu/%llu; output gain %.1f dB, limiter %s",20*log10(fmax(s->audio_state.peak,1e-9)),(unsigned long long)s->audio_state.clipped,(unsigned long long)s->audio_state.samples,c.gain,c.limiter?"on":"off");
  s->report=timestamp;
 }
 struct obs_source_audio a={0};
 a.data[0]=(uint8_t *)s->pcm; a.frames=(uint32_t)(bytes/4); a.speakers=SPEAKERS_STEREO; a.format=AUDIO_FORMAT_FLOAT; a.samples_per_sec=rate; a.timestamp=timestamp;
 obs_source_output_audio(s->source,&a);
}
static void update(void *ctx,obs_data_t *data) {
 struct source *s=ctx; settings c={0};
 int resolution=(int)obs_data_get_int(data,"resolution");
 switch(resolution) { case 1:c.width=640;c.height=480;break; case 2:c.width=720;c.height=480;break; case 3:c.width=1280;c.height=720;break; case 4:c.width=1920;c.height=1080;break; }
 c.aspect=(int)obs_data_get_int(data,"aspect");if(c.aspect<0 || c.aspect>2)c.aspect=0;
 int fps=(int)obs_data_get_int(data,"fps");
 switch(fps) { case 23976:case 24000:case 25000:case 29970:case 30000:case 50000:case 59940:case 60000:c.fps=fps;break; }
 c.matrix=(int)obs_data_get_int(data,"matrix"); c.range=(int)obs_data_get_int(data,"range"); c.format=(int)obs_data_get_int(data,"format");
 c.gain=fmax(-30,fmin(6,obs_data_get_double(data,"gain_db")));
 c.mute=obs_data_get_bool(data,"mute"); c.limiter=obs_data_get_bool(data,"limiter");
 c.release_hidden=obs_data_get_bool(data,"release_hidden");
 pthread_mutex_lock(&s->lock); s->config=c; pthread_mutex_unlock(&s->lock);
}
static void defaults(obs_data_t *d) {
 obs_data_set_default_int(d,"aspect",0);obs_data_set_default_int(d,"resolution",0); obs_data_set_default_int(d,"fps",0);
 obs_data_set_default_int(d,"matrix",0); obs_data_set_default_int(d,"range",0); obs_data_set_default_int(d,"format",0);
 obs_data_set_default_double(d,"gain_db",-6); obs_data_set_default_bool(d,"limiter",true);
 obs_data_set_default_bool(d,"release_hidden",true);
}
static obs_property_t *list(obs_properties_t *p,const char *key,const char *label) { return obs_properties_add_list(p,key,label,OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT); }
static bool refresh(obs_properties_t *props,obs_property_t *button,void *ctx) {
 (void)button; struct source *s=ctx; if(!s) return false;
 pthread_mutex_lock(&s->lock);
 hd_mode m=s->input;unsigned source_scan=s->source_scan;settings c=s->config; float peak=s->peak; uint64_t clipped=s->clipped,samples=s->samples; s->peak=0;
 pthread_mutex_unlock(&s->lock);
 char text[512];
 if(m.width) {
  unsigned w,h;hd_display_size(m.width,m.height,c.width,c.height,c.aspect,&w,&h);
  snprintf(text,sizeof text,"HDMI input: %u × %u, %s, %.3f %s; output %u × %u; audio %u Hz. Raw peak: %.1f dBFS. Near-full-scale samples: %llu / %llu.",m.width,m.height,source_scan==2?"Interlaced":source_scan==1?"Progressive":"scan unknown",m.rate/(m.fractional?1.001:1),source_scan==2?"fields/s":"fps",w,h,m.audio_rate,20*log10(fmax(peak,1e-9)),(unsigned long long)clipped,(unsigned long long)samples);
 }
 else snprintf(text,sizeof text,"Waiting for HDMI input. Audio diagnostics appear after capture starts.");
 obs_property_set_description(obs_properties_get(props,"status"),text); return true;
}
static obs_properties_t *properties(void *ctx) {
 obs_properties_t *p=obs_properties_create(); obs_property_t *v;
 obs_properties_add_text(p,"status","Input status",OBS_TEXT_INFO);
 obs_properties_add_button2(p,"refresh","Refresh input and levels",refresh,ctx);
 v=list(p,"resolution","Output resolution");
 obs_property_list_add_int(v,"Match HDMI input",0);obs_property_list_add_int(v,"640 × 480 (4:3)",1);obs_property_list_add_int(v,"720 × 480",2);obs_property_list_add_int(v,"1280 × 720",3);obs_property_list_add_int(v,"1920 × 1080",4);
 obs_property_set_long_description(v,"Fits the selected display aspect ratio inside these dimensions without cropping. Match input preserves input height and corrects width for square pixels.");
 v=list(p,"aspect","Display aspect ratio");
 obs_property_list_add_int(v,"Auto (SD 4:3, HD square pixels)",0);obs_property_list_add_int(v,"4:3",1);obs_property_list_add_int(v,"16:9",2);
 obs_property_set_long_description(v,"Corrects SD non-square pixels before sending frames to OBS. Choose 16:9 for anamorphic widescreen. Independent of the app preview setting.");
 v=list(p,"fps","Frame rate");obs_property_list_add_int(v,"Match HDMI input",0);
 const int rates[]={23976,24000,25000,29970,30000,50000,59940,60000};const char *labels[]={"23.976","24","25","29.97","30","50","59.94","60"};
 for(unsigned i=0;i<8;i++) obs_property_list_add_int(v,labels[i],rates[i]);
 obs_property_set_long_description(v,"Drops frames above this limit. It cannot add frames or change what the console sends. The recording frame rate is in OBS Settings, Video.");
 v=list(p,"format","Output pixel format");obs_property_list_add_int(v,"YUY2 (native 4:2:2)",0);obs_property_list_add_int(v,"NV12 (4:2:0)",1);obs_property_list_add_int(v,"BGRA (RGB)",2);
 v=list(p,"matrix","Color space");obs_property_list_add_int(v,"Auto (HDMI timing metadata)",0);obs_property_list_add_int(v,"Rec. 601 (SD)",1);obs_property_list_add_int(v,"Rec. 709 (HD)",2);
 v=list(p,"range","Input range");obs_property_list_add_int(v,"Auto (limited for revision 4)",0);obs_property_list_add_int(v,"Limited (16–235)",1);obs_property_list_add_int(v,"Full (0–255 override)",2);
 obs_property_set_long_description(v,"Revision 4 sends limited range. Full reinterprets those pixels; it does not change what the card sends.");
 obs_properties_add_float_slider(p,"gain_db","Audio gain (dB)",-30,6,.5);
 obs_properties_add_bool(p,"limiter","Limiter (−1 dBFS ceiling)");obs_properties_add_bool(p,"mute","Mute capture audio");
 obs_properties_add_bool(p,"release_hidden","Release the card while this source is hidden");
 refresh(p,NULL,ctx); return p;
}
static void *worker(void *ctx);
static void stop_worker(struct source *s) {
 if(!s->started) return;
 atomic_store(&s->stop,true);pthread_join(s->thread,NULL);s->started=false;
}
static bool start_worker(struct source *s) {
 if(s->started) return true;
 atomic_store(&s->stop,false);
 if(pthread_create(&s->thread,NULL,worker,s)) return false;
 s->started=true;return true;
}
static void source_show(void *ctx) {
 struct source *s=ctx;
 if(s->started) return;
 if(start_worker(s)) { pthread_mutex_lock(&s->lock);set_connection(s,HD_INITIALIZING,"source shown");pthread_mutex_unlock(&s->lock);show_status(s); }
}
static void source_hide(void *ctx) {
 struct source *s=ctx;
 if(!snapshot(s).release_hidden) return;
 stop_worker(s);
 pthread_mutex_lock(&s->lock);set_connection(s,HD_INITIALIZING,"source hidden, card released");pthread_mutex_unlock(&s->lock);
 show_status(s);
}
static void *worker(void *ctx) {
 struct source *s=ctx;
 hd_callbacks cb={.video=video,.audio=audio,.message=message,.context=s,.status=connection_status,.scan=scan};
 s->next_video=0;s->audio_state=(hd_audio_state){0};
 hd_shared_capture(&cb,&s->stop,0);
 reset_scaler(s);
 return NULL;
}
static void *create(obs_data_t *data,obs_source_t *source) {
 struct source *s=bzalloc(sizeof *s); if(!s) return NULL;
 s->source=source; atomic_init(&s->stop,false); pthread_mutex_init(&s->lock,NULL);pthread_mutex_init(&s->output_lock,NULL);s->connection=HD_INITIALIZING; update(s,data);
 obs_source_set_async_unbuffered(source,true);
 if(pthread_create(&s->thread,NULL,worker,s)) { pthread_mutex_destroy(&s->output_lock);pthread_mutex_destroy(&s->lock); bfree(s); return NULL; }
 s->started=true;show_status(s); return s;
}
static void destroy(void *ctx) {
 struct source *s=ctx; atomic_store(&s->stop,true);
 if(s->started) pthread_join(s->thread,NULL); pthread_mutex_destroy(&s->output_lock);pthread_mutex_destroy(&s->lock);free(s->status_pixels); bfree(s);
}
bool obs_module_load(void) {
 struct obs_source_info info={.id="hd60s_native_rev4",.type=OBS_SOURCE_TYPE_INPUT,
  .output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO,.get_name=name,.create=create,.destroy=destroy,
  .get_defaults=defaults,.get_properties=properties,.update=update,.video_tick=tick,
  .show=source_show,.hide=source_hide};
 obs_register_source(&info);
 hd_shared_update_in_background();
 return true;
}
