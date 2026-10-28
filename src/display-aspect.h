/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* HDMI SD sample counts are not square-pixel display dimensions. SD defaults
 * to 4:3; anamorphic sources can explicitly select 16:9 in Video settings. */
static inline double hd_display_aspect(unsigned width,unsigned height,int choice) {
 if(choice==1)return 4.0/3;
 if(choice==2)return 16.0/9;
 if((height==480 || height==576) && (width==720 || width==1440))return 4.0/3;
 return height?(double)width/height:16.0/9;
}
/* OBS emits square-pixel frames. Preserve input height when matching input;
 * explicit resolution presets are bounds, with the chosen aspect fitted inside. */
static inline void hd_display_size(unsigned input_width,unsigned input_height,
 unsigned limit_width,unsigned limit_height,int choice,unsigned *width,unsigned *height) {
 double aspect=hd_display_aspect(input_width,input_height,choice);
 unsigned h=limit_height?limit_height:input_height;
 unsigned w=(unsigned)(h*aspect/2+0.5)*2;
 if(limit_width && w>limit_width) { w=limit_width;h=(unsigned)(w/aspect/2+0.5)*2; }
 *width=w>1?w&~1u:2;*height=h>1?h&~1u:2;
}
