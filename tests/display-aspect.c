/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/display-aspect.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
 assert(fabs(hd_display_aspect(1440,480,0)-4.0/3)<1e-9);
 assert(fabs(hd_display_aspect(720,480,0)-4.0/3)<1e-9);
 assert(fabs(hd_display_aspect(1440,576,0)-4.0/3)<1e-9);
 assert(fabs(hd_display_aspect(1920,1080,0)-16.0/9)<1e-9);
 assert(fabs(hd_display_aspect(1280,720,0)-16.0/9)<1e-9);
 assert(fabs(hd_display_aspect(1440,480,2)-16.0/9)<1e-9);
 assert(fabs(hd_display_aspect(1920,1080,1)-4.0/3)<1e-9);
 unsigned w,h;
 hd_display_size(1440,480,0,0,0,&w,&h);assert(w==640 && h==480);
 hd_display_size(720,480,0,0,0,&w,&h);assert(w==640 && h==480);
 hd_display_size(1440,576,0,0,0,&w,&h);assert(w==768 && h==576);
 hd_display_size(1920,1080,0,0,0,&w,&h);assert(w==1920 && h==1080);
 hd_display_size(1440,480,1280,720,0,&w,&h);assert(w==960 && h==720);
 hd_display_size(1440,480,640,480,2,&w,&h);assert(w==640 && h==360);
 hd_display_size(1440,480,0,0,2,&w,&h);assert(w==854 && h==480);
 puts("Display aspect passed: SD 4:3, HD square pixels, explicit overrides");
}
