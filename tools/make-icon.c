/* SPDX-License-Identifier: GPL-2.0-only
 * Renders the app icon into an .iconset directory. The mark is the signal path
 * from DESIGN.md, reduced to its middle: a lead in, the card, a lead out.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rounded(CGContextRef cg,CGRect r,double radius) {
 CGPathRef p=CGPathCreateWithRoundedRect(r,radius,radius,NULL);
 CGContextAddPath(cg,p);CGPathRelease(p);
}
static void draw(CGContextRef cg,double s) {
 double inset=s*.09,box=s-inset*2,cx=s/2,cy=s/2;
 CGRect face=CGRectMake(inset,inset,box,box);
 double corner=box*.2237;
 CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();

 /* Lit from above: a vertical fall from a lighter top, a specular arc clipped to
  * the top edge, so the tile reads as a solid object rather than a flat swatch. */
 CGContextSaveGState(cg);
 rounded(cg,face,corner);CGContextClip(cg);
 CGFloat body[]={.227,.227,.251,1, .106,.106,.125,1};CGFloat stops[]={0,1};
 CGGradientRef fill=CGGradientCreateWithColorComponents(rgb,body,stops,2);
 if(fill) { CGContextDrawLinearGradient(cg,fill,CGPointMake(0,inset+box),CGPointMake(0,inset),0);CGGradientRelease(fill); }
 else { CGContextSetRGBFillColor(cg,.149,.149,.168,1);CGContextFillRect(cg,face); }
 rounded(cg,CGRectOffset(CGRectInset(face,box*.012,box*.012),0,box*.012),corner);
 CGContextSetLineWidth(cg,fmax(1,box*.02));
 CGContextReplacePathWithStrokedPath(cg);CGContextClip(cg);
 CGFloat spec[]={1,1,1,.34, 1,1,1,0};
 CGGradientRef sheen=CGGradientCreateWithColorComponents(rgb,spec,stops,2);
 if(sheen) { CGContextDrawLinearGradient(cg,sheen,CGPointMake(0,inset+box),CGPointMake(0,inset+box*.52),0);CGGradientRelease(sheen); }
 CGContextRestoreGState(cg);
 CGColorSpaceRelease(rgb);

 double stroke=box*.058;if(stroke<1)stroke=1;
 double nw=box*.42,nh=box*.32,radius=box*.08;
 CGContextSetLineCap(cg,kCGLineCapRound);
 CGContextSetLineWidth(cg,stroke);

 /* Below 32px the leads collapse into mush, so the card alone carries the mark. */
 if(s>32) {
  CGContextSetRGBStrokeColor(cg,.847,.847,.875,1);
  CGContextMoveToPoint(cg,inset+box*.11,cy);CGContextAddLineToPoint(cg,cx-nw/2,cy);
  CGContextMoveToPoint(cg,cx+nw/2,cy);CGContextAddLineToPoint(cg,inset+box*.89,cy);
  CGContextStrokePath(cg);
 }
 /* Fill the card before stroking so the leads stop at its edge. */
 CGContextSetRGBFillColor(cg,.149,.149,.168,1);
 rounded(cg,CGRectMake(cx-nw/2,cy-nh/2,nw,nh),radius);CGContextFillPath(cg);
 CGContextSetRGBStrokeColor(cg,1,1,1,1);
 rounded(cg,CGRectMake(cx-nw/2,cy-nh/2,nw,nh),radius);CGContextStrokePath(cg);

 /* The one accent: the path continues through the card, which is what the card
  * does. Same weight as the leads so it reads as the same line, not a button. */
 CGContextSetRGBStrokeColor(cg,.910,.639,.239,1);
 CGContextMoveToPoint(cg,cx-nw*.22,cy);CGContextAddLineToPoint(cg,cx+nw*.22,cy);
 CGContextStrokePath(cg);
}
static int emit(const char *dir,const char *name,int pixels) {
 CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
 CGContextRef cg=CGBitmapContextCreate(NULL,pixels,pixels,8,0,rgb,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
 CGColorSpaceRelease(rgb);
 if(!cg) return -1;
 draw(cg,pixels);
 CGImageRef image=CGBitmapContextCreateImage(cg);
 char path[1024];snprintf(path,sizeof path,"%s/%s",dir,name);
 CFStringRef sp=CFStringCreateWithCString(NULL,path,kCFStringEncodingUTF8);
 CFURLRef url=CFURLCreateWithFileSystemPath(NULL,sp,kCFURLPOSIXPathStyle,false);
 CGImageDestinationRef out=CGImageDestinationCreateWithURL(url,CFSTR("public.png"),1,NULL);
 int rc=-1;
 if(out) { CGImageDestinationAddImage(out,image,NULL);rc=CGImageDestinationFinalize(out)?0:-1;CFRelease(out); }
 CFRelease(url);CFRelease(sp);CGImageRelease(image);CGContextRelease(cg);
 if(rc) fprintf(stderr,"could not write %s\n",path);
 return rc;
}
int main(int argc,char **argv) {
 if(argc!=2) { fprintf(stderr,"usage: %s <output.iconset>\n",argv[0]);return 2; }
 struct { const char *name;int pixels; } wanted[]={
  {"icon_16x16.png",16},{"icon_16x16@2x.png",32},
  {"icon_32x32.png",32},{"icon_32x32@2x.png",64},
  {"icon_128x128.png",128},{"icon_128x128@2x.png",256},
  {"icon_256x256.png",256},{"icon_256x256@2x.png",512},
  {"icon_512x512.png",512},{"icon_512x512@2x.png",1024},
 };
 for(unsigned i=0;i<sizeof wanted/sizeof *wanted;i++)
  if(emit(argv[1],wanted[i].name,wanted[i].pixels)) return 1;
 return 0;
}
