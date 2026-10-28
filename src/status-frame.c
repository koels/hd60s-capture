/* SPDX-License-Identifier: GPL-2.0-only */
#include "status-frame.h"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Palette and the signal-chain motif are specified in DESIGN.md. */
typedef struct { double r,g,b; } ink;
static const ink SURFACE={.071,.071,.078};
static const ink PRIMARY={.961,.961,.969};
static const ink SECONDARY={.631,.631,.659};
static const ink CHAIN={.333,.333,.369};
static const ink ATTENTION={.910,.639,.239};
static const ink FAULT={.898,.282,.302};

static void stroke_with(CGContextRef cg,ink c,double alpha,double width) {
 CGContextSetRGBStrokeColor(cg,c.r,c.g,c.b,alpha);CGContextSetLineWidth(cg,width);
 CGContextSetLineCap(cg,kCGLineCapRound);CGContextSetLineJoin(cg,kCGLineJoinRound);
}
static void centered(CGContextRef cg,const char *text,CTFontRef font,ink c,double y,unsigned width) {
 CFStringRef string=CFStringCreateWithCString(NULL,text,kCFStringEncodingUTF8);
 if(!string) return;
 CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
 CGFloat parts[]={c.r,c.g,c.b,1};CGColorRef color=CGColorCreate(rgb,parts);
 const void *keys[]={kCTFontAttributeName,kCTForegroundColorAttributeName};
 const void *values[]={font,color};
 CFDictionaryRef attrs=CFDictionaryCreate(NULL,keys,values,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
 CFAttributedStringRef attributed=CFAttributedStringCreate(NULL,string,attrs);
 CTLineRef line=CTLineCreateWithAttributedString(attributed);
 double span=CTLineGetTypographicBounds(line,NULL,NULL,NULL);
 CGContextSetTextPosition(cg,(width-span)/2,y);CTLineDraw(line,cg);
 CFRelease(line);CFRelease(attributed);CFRelease(attrs);CGColorRelease(color);CGColorSpaceRelease(rgb);CFRelease(string);
}
static void at(CGContextRef cg,const char *text,CTFontRef font,ink c,double x,double y) {
 CFStringRef string=CFStringCreateWithCString(NULL,text,kCFStringEncodingUTF8);
 if(!string) return;
 CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
 CGFloat parts[]={c.r,c.g,c.b,1};CGColorRef color=CGColorCreate(rgb,parts);
 const void *keys[]={kCTFontAttributeName,kCTForegroundColorAttributeName};
 const void *values[]={font,color};
 CFDictionaryRef attrs=CFDictionaryCreate(NULL,keys,values,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
 CFAttributedStringRef attributed=CFAttributedStringCreate(NULL,string,attrs);
 CTLineRef line=CTLineCreateWithAttributedString(attributed);
 double span=CTLineGetTypographicBounds(line,NULL,NULL,NULL);
 CGContextSetTextPosition(cg,x-span/2,y);CTLineDraw(line,cg);
 CFRelease(line);CFRelease(attributed);CFRelease(attrs);CGColorRelease(color);CGColorSpaceRelease(rgb);CFRelease(string);
}

static void rounded_rect(CGContextRef cg,CGRect r,double radius) {
 CGPathRef path=CGPathCreateWithRoundedRect(r,radius,radius,NULL);
 CGContextAddPath(cg,path);CGPathRelease(path);
}
/* A chain node, drawn as glass: a layered shadow beneath, a translucent fill,
 * a border, then a specular edge clipped to the top of the shape. `held` draws
 * the bar that marks the card as owned elsewhere. */
static void node(CGContextRef cg,double x,double y,double w,double h,ink c,double line,int held,double alpha) {
 CGRect r=CGRectMake(x-w/2,y-h/2,w,h);
 double radius=line*2.4;
 CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
 CGFloat shade[]={0,0,0,.62*alpha};CGColorRef shadow=CGColorCreate(rgb,shade);
 CGContextSaveGState(cg);
 CGContextSetShadowWithColor(cg,CGSizeMake(0,-line*1.5),line*4.5,shadow);
 CGContextSetRGBFillColor(cg,1,1,1,.055*alpha);
 rounded_rect(cg,r,radius);CGContextFillPath(cg);
 CGContextRestoreGState(cg);
 CGColorRelease(shadow);CGColorSpaceRelease(rgb);

 stroke_with(cg,c,alpha,line);
 rounded_rect(cg,r,radius);CGContextStrokePath(cg);

 /* Clipped to the upper half so the highlight sits on the top edge only; a full
  * inset stroke reads as a second border rather than as light. */
 CGContextSaveGState(cg);
 rounded_rect(cg,r,radius);CGContextClip(cg);
 rounded_rect(cg,CGRectOffset(CGRectInset(r,line*.4,line*.4),0,line*.5),radius);
 CGContextSetLineWidth(cg,line*.7);
 CGContextReplacePathWithStrokedPath(cg);CGContextClip(cg);
 CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
 CGFloat spec[]={1,1,1,.38*alpha, 1,1,1,0};CGFloat ends[]={0,1};
 CGGradientRef sheen=CGGradientCreateWithColorComponents(space,spec,ends,2);
 if(sheen) { CGContextDrawLinearGradient(cg,sheen,CGPointMake(0,r.origin.y+r.size.height),CGPointMake(0,r.origin.y+r.size.height*.45),0);CGGradientRelease(sheen); }
 CGColorSpaceRelease(space);
 CGContextRestoreGState(cg);

 if(held) {
  CGContextSetRGBFillColor(cg,c.r,c.g,c.b,alpha);
  CGContextFillRect(cg,CGRectMake(x-w*.22,y-h*.16,w*.44,h*.32));
 }
}
/* A link. `broken` opens a gap in the middle; `pending` dashes it. */
static void segment(CGContextRef cg,double x0,double x1,double y,ink c,double line,int broken,int pending,double alpha,int bloom) {
 CGContextSaveGState(cg);
 if(bloom) {
  CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
  CGFloat parts[]={c.r,c.g,c.b,.75};CGColorRef halo=CGColorCreate(rgb,parts);
  CGContextSetShadowWithColor(cg,CGSizeZero,line*3.2,halo);
  CGColorRelease(halo);CGColorSpaceRelease(rgb);
 }
 stroke_with(cg,c,alpha,line);
 if(pending) { CGFloat dash[]={line*1.6,line*2.4};CGContextSetLineDash(cg,0,dash,2); }
 if(broken) {
  double gap=(x1-x0)*.26;
  CGContextMoveToPoint(cg,x0,y);CGContextAddLineToPoint(cg,x0+((x1-x0)-gap)/2,y);
  CGContextMoveToPoint(cg,x1-((x1-x0)-gap)/2,y);CGContextAddLineToPoint(cg,x1,y);
 } else {
  CGContextMoveToPoint(cg,x0,y);CGContextAddLineToPoint(cg,x1,y);
 }
 CGContextStrokePath(cg);
 CGContextSetLineDash(cg,0,NULL,0);
 CGContextRestoreGState(cg);
}

uint8_t *hd_status_frame(unsigned width,unsigned height,hd_status status) {
 return hd_status_frame_text(width,height,status,NULL,NULL);
}
uint8_t *hd_status_frame_text(unsigned width,unsigned height,hd_status status,
                              const char *title_override,const char *detail_override) {
 if(width<160 || height<120 || width>4096 || height>2160) return NULL;
 uint8_t *pixels=calloc((size_t)width*height,4);if(!pixels)return NULL;
 CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
 CGContextRef cg=CGBitmapContextCreate(pixels,width,height,8,width*4,rgb,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
 if(!cg) { CGColorSpaceRelease(rgb);free(pixels);return NULL; }
 CGContextSetRGBFillColor(cg,SURFACE.r,SURFACE.g,SURFACE.b,1);
 CGContextFillRect(cg,CGRectMake(0,0,width,height));
 /* Ambient pool of light behind the chain, then a vignette at the edges: depth
  * without colour, so the frame reads as a lit surface rather than a dead fill. */
 { CGFloat lift[]={1,1,1,.055, 1,1,1,0};CGFloat stops[]={0,1};
   CGGradientRef pool=CGGradientCreateWithColorComponents(rgb,lift,stops,2);
   if(pool) { CGContextDrawRadialGradient(cg,pool,CGPointMake(width/2.,height*.56),0,CGPointMake(width/2.,height*.56),width*.52,kCGGradientDrawsAfterEndLocation);CGGradientRelease(pool); }
   CGFloat dusk[]={0,0,0,0, 0,0,0,.5};CGFloat edges[]={.42,1};
   CGGradientRef shade=CGGradientCreateWithColorComponents(rgb,dusk,edges,2);
   if(shade) { CGContextDrawRadialGradient(cg,shade,CGPointMake(width/2.,height/2.),0,CGPointMake(width/2.,height/2.),hypot(width,height)/2,kCGGradientDrawsAfterEndLocation);CGGradientRelease(shade); } }

 const char *title="Starting up",*detail="Opening the capture card";
 ink accent=CHAIN;
 int break_hdmi=0,break_usb=0,held=0,pending=0,fault=0;
 switch(status) {
 case HD_DISCONNECTED:
  title="No capture card";detail="Connect the HD60 S to this Mac with a USB 3 cable";
  accent=ATTENTION;break_usb=1;break;
 case HD_NO_SIGNAL:
  title="No signal";detail="The card is connected. Turn on your console and check the HDMI cable";
  accent=ATTENTION;break_hdmi=1;break;
 case HD_BUSY:
  title="Card already in use";detail="Another app has the HD60 S open. Close it to capture here";
  accent=ATTENTION;held=1;break;
 case HD_ERROR:
  title="Capture stopped";detail="Reconnecting. Reseat the USB cable if this keeps happening";
  accent=FAULT;break_hdmi=1;break_usb=1;fault=1;break;
 case HD_INITIALIZING:case HD_LIVE:pending=1;break;
 }
 if(title_override)title=title_override;
 if(detail_override)detail=detail_override;

 double scale=width/1280.;if(height/720.<scale)scale=height/720.;if(scale<.3)scale=.3;
 double cx=width/2.,cy=height/2.;
 /* The chain is the focal point, so it carries the size; the text sits under it.
  * Everything is offset so the whole block, not the chain alone, is centred. */
 double line=4*scale,span=214*scale,nw=72*scale,nh=52*scale,chain=cy+50*scale;

 /* Chain: source -- HDMI -- HD60 S -- USB -- Mac. The broken link is the fault. */
 ink hdmi=break_hdmi?accent:CHAIN,usb=break_usb?accent:CHAIN;
 /* With no card attached, nothing past the USB break is known, so the card and
  * its HDMI link are dimmed rather than drawn as if they were working. */
 double unknown=status==HD_DISCONNECTED?.3:1;
 segment(cg,cx-span+nw/2,cx-nw/2,chain,hdmi,line,break_hdmi,pending,unknown,break_hdmi);
 segment(cg,cx+nw/2,cx+span-nw/2,chain,usb,line,break_usb,pending,1,break_usb);
 node(cg,cx-span,chain,nw*.78,nh,CHAIN,line,0,unknown);
 node(cg,cx,chain,nw,nh,(held||fault)?accent:CHAIN,line,held,unknown);
 node(cg,cx+span,chain,nw*.78,nh,CHAIN,line,0,1);

 CTFontRef small=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,12*scale,NULL);
 CTFontRef heading=CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem,42*scale,NULL);
 CTFontRef body=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,17*scale,NULL);
 if(small) {
  at(cg,"Source",small,SECONDARY,cx-span,chain-nh/2-18*scale);
  at(cg,"HD60 S",small,SECONDARY,cx,chain-nh/2-18*scale);
  at(cg,"Mac",small,SECONDARY,cx+span,chain-nh/2-18*scale);
  at(cg,"HDMI",small,break_hdmi?accent:SECONDARY,cx-span/2,chain+nh/2+10*scale);
  at(cg,"USB",small,break_usb?accent:SECONDARY,cx+span/2,chain+nh/2+10*scale);
 }
 if(heading) centered(cg,title,heading,PRIMARY,cy-52*scale,width);
 if(body) centered(cg,detail,body,SECONDARY,cy-90*scale,width);
 if(small)CFRelease(small);if(heading)CFRelease(heading);if(body)CFRelease(body);

 CGContextRelease(cg);CGColorSpaceRelease(rgb);
 return pixels;
}
