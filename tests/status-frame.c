/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/status-frame.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <assert.h>
#include <stdlib.h>
int main(void) {
 assert(!hd_status_frame(0,0,HD_NO_SIGNAL));
 for(int state=HD_DISCONNECTED;state<=HD_ERROR;state++) {
  unsigned w=1280,h=720;uint8_t *pixels=hd_status_frame(w,h,state);assert(pixels);
  unsigned lit=0;
  for(unsigned p=0;p<w*h;p++) { assert(pixels[p*4+3]==255); if(pixels[p*4]>150) lit++; }
  assert(lit>300);
  if(state==HD_NO_SIGNAL) {
   CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();CGContextRef context=CGBitmapContextCreate(pixels,w,h,8,w*4,rgb,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
   CGImageRef image=CGBitmapContextCreateImage(context);
   CFURLRef url=CFURLCreateWithFileSystemPath(NULL,CFSTR("build/obs-no-signal.png"),kCFURLPOSIXPathStyle,false);
   CGImageDestinationRef destination=CGImageDestinationCreateWithURL(url,CFSTR("public.png"),1,NULL);assert(destination);CGImageDestinationAddImage(destination,image,NULL);assert(CGImageDestinationFinalize(destination));
   CFRelease(destination);CFRelease(url);CGImageRelease(image);CGContextRelease(context);CGColorSpaceRelease(rgb);
  }
  free(pixels);
 }
}
