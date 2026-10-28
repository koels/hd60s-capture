/* SPDX-License-Identifier: GPL-2.0-only */
#include <obs.h>
#include <media-io/video-scaler.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/display-aspect.h"
int main(void) {
 unsigned widths[]={640,720,1280,1920},heights[]={480,480,720,1080};
 enum video_format formats[]={VIDEO_FORMAT_YUY2,VIDEO_FORMAT_NV12,VIDEO_FORMAT_BGRA};
 uint8_t *in=malloc(1440*480*2);
 /* Neutral limited-range white must remain white and neutral after conversion. */
 for(int i=0;i<1440*480*2;i++) in[i]=i%2?128:235;
 for(unsigned input_width=720;input_width<=1440;input_width+=720)
 for(unsigned r=0;r<4;r++) for(unsigned f=0;f<3;f++) {
  unsigned width,height;hd_display_size(input_width,480,widths[r],heights[r],0,&width,&height);
  struct video_scale_info src={VIDEO_FORMAT_YUY2,input_width,480,VIDEO_RANGE_PARTIAL,VIDEO_CS_601};
  struct video_scale_info dst={formats[f],width,height,f==2?VIDEO_RANGE_FULL:VIDEO_RANGE_PARTIAL,VIDEO_CS_601};
  video_scaler_t *scaler=NULL;
  assert(video_scaler_create(&scaler,&dst,&src,VIDEO_SCALE_BICUBIC)==VIDEO_SCALER_SUCCESS);
  struct obs_source_frame *frame=obs_source_frame_create(formats[f],width,height);assert(frame && frame->data[0]);
  const uint8_t *planes[8]={in}; uint32_t strides[8]={input_width*2};
  assert(video_scaler_scale(scaler,frame->data,frame->linesize,planes,strides));
  if(f==2) { assert(frame->data[0][0]>=250 && frame->data[0][1]>=250 && frame->data[0][2]>=250); }
  else { assert(frame->data[0][0]>=233 && frame->data[0][0]<=237); }
  obs_source_frame_destroy(frame);video_scaler_destroy(scaler);
 }
 free(in);puts("Scaler passed: 720/1440-sample SD aspect correction × four bounds × YUY2/NV12/BGRA, neutral-white preservation");
}
