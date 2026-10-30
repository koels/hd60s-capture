/* SPDX-License-Identifier: GPL-2.0-only */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include "capture.h"
#include "shared.h"
#include "processing.h"
#include "display-aspect.h"
#include "status-frame.h"
#import <Sparkle/Sparkle.h>
#import "PluginInstaller.h"

#define AUDIO_CAPACITY 192000
#define OVERLAY_IDLE 2.5
#define SETTINGS_WIDTH 470
#define SETTINGS_INSET 24

@interface CaptureApp : NSObject <NSApplicationDelegate,NSWindowDelegate,SPUUpdaterDelegate> {
@public
 atomic_bool stop,framePending,monitor,muted,limiterOn,recording;
 atomic_int gainCentiBel; // dB × 100, read on the capture thread.
 _Atomic float meterPeak;
 atomic_uint_fast64_t session;
 pthread_t worker; BOOL workerStarted;
 pthread_mutex_t audioLock;
 uint8_t audioRing[AUDIO_CAPACITY]; size_t audioRead,audioWrite,audioCount;
 AudioQueueRef audioQueue; unsigned audioRate;
 hd_audio_state audioState;
 // Writer state is touched by the capture thread and the main thread; writerLock covers all of it.
 pthread_mutex_t writerLock;
 CMAudioFormatDescriptionRef recordAudioFormat;
 uint64_t recordEpoch; BOOL recordSessionStarted;
 hd_mode recordMode,liveMode; BOOL haveLiveMode;
 unsigned sourceScan; // Capture worker only; snapshot with each frame for UI.
}
- (void)fitWindowToInput;
- (void)layoutPicture;
@property NSWindow *window;
@property NSView *preview;
@property CALayer *picture;
@property NSImageView *placeholder;
@property NSString *statusKey;
@property NSString *shownTitle,*shownDetail;
@property hd_status shownStatus;
@property NSView *header;
@property NSTextField *badge;
@property NSView *badgeDot;
@property NSTextField *mode;
@property NSView *overlay;
@property NSButton *audioButton;
@property NSButton *pauseButton;
@property NSButton *recordButton;
@property NSTextField *elapsed;
@property NSButton *fullScreenButton;
@property AVAssetWriter *writer;
@property AVAssetWriterInput *videoInput,*audioInput;
@property AVAssetWriterInputPixelBufferAdaptor *adaptor;
@property NSURL *recordingURL;
@property NSDate *recordingStart;
@property NSLevelIndicator *meter;
@property NSTextField *sizeChip,*rateChip,*audioChip;
@property NSStackView *chips;
@property NSWindow *settingsWindow;
@property NSTextField *peakReadout;
@property NSTextField *recordingFolderLabel;
@property NSTextField *captureSummary;
// The mode line has one owner: a note outranks the live or status text until it expires.
@property NSString *statusHint,*note;
@property SPUStandardUpdaterController *updater;
@property NSString *launchVersion;
@property NSView *updateBar;
@property NSTextField *updateLabel;
@property unsigned updateTicks;
@property NSDate *noteUntil;
@property BOOL noteIsError;
@property NSSlider *gainSlider;
@property NSTextField *gainReadout;
@property NSArray<NSLayoutConstraint *> *windowedLayout,*fullScreenLayout;
@property CIContext *imageContext;
@property BOOL paused;
@property (readonly) BOOL overlayPinned;
@property BOOL closing;
@property float meterLevel;
@property NSDate *lastFrame;
@property NSDate *lastPointer;
@property NSTimer *watchdog;
@property NSTimer *meterTimer;
- (void)displayStatus:(hd_status)status detail:(NSString *)detail;
- (void)startCapture;
- (void)stopCapture;
- (void)pushAudio:(const uint8_t *)data bytes:(size_t)bytes rate:(unsigned)rate timestamp:(uint64_t)timestamp;
- (void)recordVideo:(CVPixelBufferRef)pixel timestamp:(uint64_t)timestamp mode:(const hd_mode *)mode;
- (void)recordAudio:(const uint8_t *)pcm bytes:(size_t)bytes rate:(unsigned)rate timestamp:(uint64_t)timestamp;
- (void)stopRecording:(NSString *)reason completion:(void (^)(void))done;
@end

static NSTextField *label(NSString *text,CGFloat size,NSFontWeight weight,NSColor *color) {
 NSTextField *v=[NSTextField labelWithString:text]; v.font=[NSFont systemFontOfSize:size weight:weight];v.textColor=color;v.lineBreakMode=NSLineBreakByWordWrapping;return v;
}
/* The header's line about the card. Revision 4 is what this was built and
 * tested on; the older ones are tried, and say so. */
static NSString *cardLine(void) {
 int speed=-1,product=hd_device_find(&speed);
 /* The link is whatever the cable, hub and port negotiated, not what the card
  * can do: a USB 2.0 path says so here instead of claiming USB 3. */
 NSString *link=speed<0?@"USB":speed<HD_SPEED_SUPER?@"USB 2.0, too slow":@"USB 3";
 return product>0 && product!=0x76?[NSString stringWithFormat:@"Older revision %04x over %@, untested",product,link]:[NSString stringWithFormat:@"Revision 4 over %@",link];
}
/* Below SuperSpeed the card enumerates and then starves, so say which part of
 * the chain is wrong rather than connecting forever. nil when the link is fine. */
static NSString *slowLinkDetail(void) { return hd_slow_link()?@HD_SLOW_LINK_DETAIL:nil; }
/* One fact in a soft container. Returns the field; its superview is the chip. */
static NSTextField *chip(NSString *text) {
 NSTextField *value=label(text,11,NSFontWeightMedium,[NSColor colorWithWhite:1 alpha:.82]);
 value.font=[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium];
 NSStackView *box=[NSStackView stackViewWithViews:@[value]];
 box.edgeInsets=NSEdgeInsetsMake(3,8,3,8);box.wantsLayer=YES;
 box.layer.cornerRadius=6;box.layer.backgroundColor=[NSColor colorWithWhite:1 alpha:.06].CGColor;
 return value;
}
static NSButton *iconButton(NSString *symbol,NSString *tip,id target,SEL action) {
 NSButton *b=[NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:tip]?:[NSImage new] target:target action:action];
 b.bezelStyle=NSBezelStyleRegularSquare;b.bordered=NO;b.imageScaling=NSImageScaleProportionallyDown;b.contentTintColor=NSColor.whiteColor;b.toolTip=tip;
 // Lifts the glyph off the glass so it reads at any brightness behind the bar.
 b.wantsLayer=YES;
 NSShadow *depth=[[NSShadow alloc] init];
 depth.shadowColor=[NSColor colorWithWhite:0 alpha:.55];depth.shadowOffset=NSMakeSize(0,-1);depth.shadowBlurRadius=3;
 b.shadow=depth;
 [b.widthAnchor constraintEqualToConstant:30].active=YES;[b.heightAnchor constraintEqualToConstant:26].active=YES;
 return b;
}
static void setSymbol(NSButton *button,NSString *symbol,NSString *tip) {
 NSImage *image=[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:tip];
 if(image)button.image=image;
 button.toolTip=tip;
}
/* Escape closes a settings window only if something in it answers for Escape. */
@interface HDSettingsWindow : NSWindow
@end
@implementation HDSettingsWindow
- (void)cancelOperation:(id)sender { (void)sender;[self performClose:nil]; }
@end

static void audioFill(void *ctx,AudioQueueRef queue,AudioQueueBufferRef buffer) {
 CaptureApp *app=(__bridge CaptureApp *)ctx;
 pthread_mutex_lock(&app->audioLock);
 size_t bytes=buffer->mAudioDataBytesCapacity;
 uint8_t *dst=buffer->mAudioData;
 for(size_t i=0;i<bytes;i++) {
  if(app->audioCount) { dst[i]=app->audioRing[app->audioRead];app->audioRead=(app->audioRead+1)%AUDIO_CAPACITY;app->audioCount--; }
  else dst[i]=0;
 }
 pthread_mutex_unlock(&app->audioLock);
 buffer->mAudioDataByteSize=(UInt32)bytes;AudioQueueEnqueueBuffer(queue,buffer,0,NULL);
}
static void captureMessage(void *ctx,const char *message) { (void)ctx;fprintf(stderr,"[HD60 S] %s\n",message); }
static void captureStatus(void *ctx,hd_status status,const char *detail) {
 CaptureApp *app=(__bridge CaptureApp *)ctx;NSString *text=[NSString stringWithUTF8String:detail];uint64_t generation=atomic_load(&app->session);
 dispatch_async(dispatch_get_main_queue(),^{ if(!app.closing && generation==atomic_load(&app->session)) [app displayStatus:status detail:text]; });
}
static void captureVideo(void *ctx,const uint8_t *bytes,const hd_mode *mode,uint64_t timestamp) {
 CaptureApp *app=(__bridge CaptureApp *)ctx;
 bool rec=atomic_load(&app->recording);
 // Display is throttled to one in-flight frame; recording must still see every frame.
 bool display=!atomic_exchange(&app->framePending,true);
 if(!rec && !display) return;
 @autoreleasepool {
  CVPixelBufferRef pixel=NULL;
  NSDictionary *attrs=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
  CVReturn result=CVPixelBufferCreate(NULL,mode->width,mode->height,kCVPixelFormatType_422YpCbCr8_yuvs,(__bridge CFDictionaryRef)attrs,&pixel);
  if(result) { if(display)atomic_store(&app->framePending,false);return; }
  CVPixelBufferLockBaseAddress(pixel,0);
  uint8_t *dst=CVPixelBufferGetBaseAddress(pixel);size_t stride=CVPixelBufferGetBytesPerRow(pixel);
  for(unsigned row=0;row<mode->height;row++) memcpy(dst+row*stride,bytes+(size_t)row*mode->width*2,mode->width*2);
  CVPixelBufferUnlockBaseAddress(pixel,0);
  CVBufferSetAttachment(pixel,kCVImageBufferYCbCrMatrixKey,mode->bt709?kCVImageBufferYCbCrMatrix_ITU_R_709_2:kCVImageBufferYCbCrMatrix_ITU_R_601_4,kCVAttachmentMode_ShouldPropagate);
  if(rec) [app recordVideo:pixel timestamp:timestamp mode:mode];
  if(!display) { CVPixelBufferRelease(pixel);return; }
  CIImage *image=[CIImage imageWithCVPixelBuffer:pixel];
  CGImageRef rendered=[app.imageContext createCGImage:image fromRect:CGRectMake(0,0,mode->width,mode->height)];
  CVPixelBufferRelease(pixel);
  if(!rendered) { atomic_store(&app->framePending,false);return; }
  NSString *scan=app->sourceScan==2?@"Interlaced":app->sourceScan==1?@"Progressive":@"Scan unknown";
  NSString *size=[NSString stringWithFormat:@"%u × %u · %@",mode->width,mode->height,scan];
  NSString *rate=[NSString stringWithFormat:@"%.2f %@",mode->rate/(mode->fractional?1.001:1),app->sourceScan==2?@"fields/s":@"fps"];
  NSString *sound=mode->audio_rate?[NSString stringWithFormat:@"%u kHz stereo",mode->audio_rate/1000]:@"no audio";
  hd_mode current=*mode;
  uint64_t generation=atomic_load(&app->session);
  dispatch_async(dispatch_get_main_queue(),^{
   if(!app.closing && generation==atomic_load(&app->session) && !atomic_load(&app->stop)) {
    BOOL geometryChanged=!app->haveLiveMode || app->liveMode.width!=current.width || app->liveMode.height!=current.height;
    app->liveMode=current;app->haveLiveMode=YES;
    if(geometryChanged) [app fitWindowToInput];
    [app layoutPicture];
    app.picture.contents=(__bridge id)rendered;app.placeholder.hidden=YES;app.statusKey=nil;
    app.badge.stringValue=@"Live";app.badge.textColor=NSColor.whiteColor;app.badgeDot.layer.backgroundColor=NSColor.systemGreenColor.CGColor;
    app.sizeChip.stringValue=size;app.rateChip.stringValue=rate;app.audioChip.stringValue=sound;app.chips.hidden=NO;
    app.statusHint=cardLine();app.lastFrame=[NSDate date];
   }
   CGImageRelease(rendered);atomic_store(&app->framePending,false);
  });
 }
}
static void captureScan(void *ctx,unsigned scan) { ((__bridge CaptureApp *)ctx)->sourceScan=scan; }
static void captureAudio(void *ctx,const uint8_t *data,size_t bytes,unsigned rate,uint64_t timestamp) {
 [(__bridge CaptureApp *)ctx pushAudio:data bytes:bytes rate:rate timestamp:timestamp];
}
static void *captureWorker(void *ctx) {
 CaptureApp *app=(__bridge CaptureApp *)ctx;
 @autoreleasepool {
  hd_callbacks callbacks={.video=captureVideo,.audio=captureAudio,.message=captureMessage,.context=ctx,.status=captureStatus,.scan=captureScan};
  hd_shared_capture(&callbacks,&app->stop,0);
 }
 return NULL;
}

@implementation CaptureApp
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
 (void)notification;
 NSUserDefaults *defaults=[NSUserDefaults standardUserDefaults];
 [defaults registerDefaults:@{@"playCaptureAudio":@YES,@"audioGain":@-6.0,@"audioMuted":@NO,@"audioLimiter":@YES,@"previewFit":@0,@"overlayAlwaysVisible":@NO,@"recordQuality":@0}];
 atomic_init(&session,0);atomic_init(&stop,false);atomic_init(&framePending,false);
 atomic_init(&monitor,[defaults boolForKey:@"playCaptureAudio"]);
 atomic_init(&muted,[defaults boolForKey:@"audioMuted"]);
 atomic_init(&limiterOn,[defaults boolForKey:@"audioLimiter"]);
 atomic_init(&gainCentiBel,(int)lround([defaults doubleForKey:@"audioGain"]*100));
 atomic_init(&meterPeak,0.0f);
 atomic_init(&recording,false);
 pthread_mutex_init(&audioLock,NULL);pthread_mutex_init(&writerLock,NULL);
 self.imageContext=[CIContext contextWithOptions:@{kCIContextWorkingColorSpace:[NSNull null]}];

 self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1100,740) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable|NSWindowStyleMaskFullSizeContentView backing:NSBackingStoreBuffered defer:NO];
 self.window.collectionBehavior=NSWindowCollectionBehaviorFullScreenPrimary;
 self.window.title=@"HD60 S Capture";self.window.minSize=NSMakeSize(720,520);self.window.delegate=self;
 self.window.titlebarAppearsTransparent=YES;self.window.titleVisibility=NSWindowTitleHidden;
 self.window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
 self.window.acceptsMouseMovedEvents=YES;
 NSView *root=self.window.contentView;root.wantsLayer=YES;root.layer.backgroundColor=[NSColor colorWithSRGBRed:.071 green:.071 blue:.078 alpha:1].CGColor;

 // Header: the card's identity on the left, what it is doing on the right.
 // Hidden in full screen, where the picture is the whole point.
 self.header=[[NSView alloc] init];
 NSImageView *mark=[NSImageView imageViewWithImage:[NSImage imageNamed:NSImageNameApplicationIcon]?:[NSImage new]];
 mark.imageScaling=NSImageScaleProportionallyUpOrDown;
 [mark.widthAnchor constraintEqualToConstant:32].active=YES;[mark.heightAnchor constraintEqualToConstant:32].active=YES;
 NSTextField *heading=label(@"HD60 S Capture",16,NSFontWeightSemibold,NSColor.whiteColor);
 self.mode=label(cardLine(),11,NSFontWeightRegular,NSColor.secondaryLabelColor);
 self.mode.font=[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
 NSStackView *titles=[NSStackView stackViewWithViews:@[heading,self.mode]];
 titles.orientation=NSUserInterfaceLayoutOrientationVertical;titles.alignment=NSLayoutAttributeLeading;titles.spacing=2;
 NSStackView *identity=[NSStackView stackViewWithViews:@[mark,titles]];identity.spacing=11;identity.alignment=NSLayoutAttributeCenterY;

 self.badgeDot=[[NSView alloc] init];self.badgeDot.wantsLayer=YES;self.badgeDot.layer.cornerRadius=3.5;self.badgeDot.layer.backgroundColor=NSColor.systemOrangeColor.CGColor;
 [self.badgeDot.widthAnchor constraintEqualToConstant:7].active=YES;[self.badgeDot.heightAnchor constraintEqualToConstant:7].active=YES;
 self.badge=label(@"Connecting",11,NSFontWeightSemibold,NSColor.whiteColor);
 NSStackView *badgeRow=[NSStackView stackViewWithViews:@[self.badgeDot,self.badge]];badgeRow.spacing=6;badgeRow.alignment=NSLayoutAttributeCenterY;
 badgeRow.edgeInsets=NSEdgeInsetsMake(4,9,4,11);badgeRow.wantsLayer=YES;badgeRow.layer.cornerRadius=9;badgeRow.layer.backgroundColor=[NSColor colorWithWhite:1 alpha:.08].CGColor;

 // The input's facts, one per chip. Separate chips because they are separate
 // facts: resolution can change while the audio rate holds, and a single
 // run-together line hides that.
 self.sizeChip=chip(@"");self.rateChip=chip(@"");self.audioChip=chip(@"");
 self.chips=[NSStackView stackViewWithViews:@[self.sizeChip.superview,self.rateChip.superview,self.audioChip.superview]];
 self.chips.spacing=6;self.chips.alignment=NSLayoutAttributeCenterY;self.chips.hidden=YES;
 NSStackView *state=[NSStackView stackViewWithViews:@[badgeRow,self.chips]];
 state.orientation=NSUserInterfaceLayoutOrientationVertical;state.alignment=NSLayoutAttributeTrailing;state.spacing=7;

 for(NSView *v in @[identity,state]) { [self.header addSubview:v];v.translatesAutoresizingMaskIntoConstraints=NO; }
 // The header is given its height outright. Deriving it from the two stacks
 // leaves it free to grow, because nothing underneath insists the picture keep
 // any height, and the picture then collapses to a line.
 [NSLayoutConstraint activateConstraints:@[
  [self.header.heightAnchor constraintEqualToConstant:56],
  [identity.leadingAnchor constraintEqualToAnchor:self.header.leadingAnchor],
  [identity.centerYAnchor constraintEqualToAnchor:self.header.centerYAnchor],
  [state.trailingAnchor constraintEqualToAnchor:self.header.trailingAnchor],
  [state.centerYAnchor constraintEqualToAnchor:self.header.centerYAnchor],
  [state.leadingAnchor constraintGreaterThanOrEqualToAnchor:identity.trailingAnchor constant:20]]];

 // Preview surface.
 self.preview=[[NSView alloc] init];self.preview.wantsLayer=YES;
 self.preview.layer.backgroundColor=[NSColor colorWithSRGBRed:.071 green:.071 blue:.078 alpha:1].CGColor;self.preview.layer.cornerRadius=12;self.preview.layer.masksToBounds=YES;
 self.preview.layer.borderWidth=1;self.preview.layer.borderColor=[NSColor colorWithWhite:1 alpha:.08].CGColor;
 self.picture=[CALayer layer];self.picture.actions=@{@"contents":[NSNull null],@"bounds":[NSNull null],@"position":[NSNull null]};
 self.picture.contentsGravity=kCAGravityResizeAspect;[self.preview.layer addSublayer:self.picture];
 [self.preview addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseMoved|NSTrackingMouseEnteredAndExited|NSTrackingActiveAlways|NSTrackingInVisibleRect owner:self userInfo:nil]];
 NSClickGestureRecognizer *doubleClick=[[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(toggleFullScreen:)];
 doubleClick.numberOfClicksRequired=2;[self.preview addGestureRecognizer:doubleClick];

 // Status placeholder: the same signal-chain frame the OBS source draws.
 self.placeholder=[[NSImageView alloc] init];
 self.placeholder.imageScaling=NSImageScaleAxesIndependently;
 [self.preview addSubview:self.placeholder];self.placeholder.translatesAutoresizingMaskIntoConstraints=NO;
 [NSLayoutConstraint activateConstraints:@[[self.placeholder.leadingAnchor constraintEqualToAnchor:self.preview.leadingAnchor],[self.placeholder.trailingAnchor constraintEqualToAnchor:self.preview.trailingAnchor],[self.placeholder.topAnchor constraintEqualToAnchor:self.preview.topAnchor],[self.placeholder.bottomAnchor constraintEqualToAnchor:self.preview.bottomAnchor]]];

 // Floating control bar over the video.
 self.recordButton=iconButton(@"record.circle",@"Record",self,@selector(toggleRecording:));
 self.elapsed=label(@"00:00",12,NSFontWeightMedium,NSColor.whiteColor);
 self.elapsed.font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium];
 [self.elapsed.widthAnchor constraintGreaterThanOrEqualToConstant:40].active=YES;self.elapsed.hidden=YES;
 self.pauseButton=iconButton(@"pause.fill",@"Pause capture",self,@selector(toggleCapture:));
 self.audioButton=iconButton(atomic_load(&monitor)?@"speaker.wave.2.fill":@"speaker.slash.fill",@"Play capture audio",self,@selector(toggleAudio:));
 self.meter=[[NSLevelIndicator alloc] init];self.meter.levelIndicatorStyle=NSLevelIndicatorStyleContinuousCapacity;
 self.meter.minValue=0;self.meter.maxValue=1;self.meter.warningValue=.75;self.meter.criticalValue=.95;self.meter.doubleValue=0;
 self.meter.toolTip=@"Input peak level (before gain)";
 [self.meter.widthAnchor constraintEqualToConstant:96].active=YES;
 NSButton *gear=iconButton(@"slider.horizontal.3",@"Capture settings",self,@selector(showSettings:));
 self.fullScreenButton=iconButton(@"arrow.up.left.and.arrow.down.right",@"Full screen",self,@selector(toggleFullScreen:));
 NSBox *divider=[[NSBox alloc] init],*divider2=[[NSBox alloc] init];
 for(NSBox *line in @[divider,divider2]) { line.boxType=NSBoxSeparator;[line.heightAnchor constraintEqualToConstant:18].active=YES; }
 NSStackView *controls=[NSStackView stackViewWithViews:@[self.recordButton,self.elapsed,divider,self.pauseButton,self.audioButton,self.meter,divider2,gear,self.fullScreenButton]];
 controls.spacing=6;controls.alignment=NSLayoutAttributeCenterY;controls.edgeInsets=NSEdgeInsetsMake(8,12,8,12);
 // Three groups, spaced apart: what is being saved, what is playing, how it is shown.
 [controls setCustomSpacing:10 afterView:self.elapsed];[controls setCustomSpacing:10 afterView:divider];
 [controls setCustomSpacing:10 afterView:self.meter];[controls setCustomSpacing:10 afterView:divider2];
 controls.translatesAutoresizingMaskIntoConstraints=NO;
 if(@available(macOS 26.0,*)) {
  NSGlassEffectView *glass=[[NSGlassEffectView alloc] init];
  glass.cornerRadius=18;glass.style=NSGlassEffectViewStyleRegular;
  if(@available(macOS 27.0,*)) glass.effectIsInteractive=YES;
  glass.contentView=controls;
  self.overlay=glass;
 } else {
  NSVisualEffectView *fallback=[[NSVisualEffectView alloc] init];
  fallback.material=NSVisualEffectMaterialHUDWindow;fallback.blendingMode=NSVisualEffectBlendingModeWithinWindow;
  fallback.state=NSVisualEffectStateActive;fallback.wantsLayer=YES;
  fallback.layer.cornerRadius=18;fallback.layer.masksToBounds=YES;
  [fallback addSubview:controls];
  self.overlay=fallback;
 }
 [NSLayoutConstraint activateConstraints:@[[controls.leadingAnchor constraintEqualToAnchor:self.overlay.leadingAnchor],[controls.trailingAnchor constraintEqualToAnchor:self.overlay.trailingAnchor],[controls.topAnchor constraintEqualToAnchor:self.overlay.topAnchor],[controls.bottomAnchor constraintEqualToAnchor:self.overlay.bottomAnchor]]];
 [self.preview addSubview:self.overlay];self.overlay.translatesAutoresizingMaskIntoConstraints=NO;
 [NSLayoutConstraint activateConstraints:@[[self.overlay.centerXAnchor constraintEqualToAnchor:self.preview.centerXAnchor],[self.overlay.bottomAnchor constraintEqualToAnchor:self.preview.bottomAnchor constant:-18]]];

 // A rebuilt app on disk keeps running as the old one until it is relaunched,
 // which is how an old binary quietly kept serving after an update. Say so in
 // the picture, where it cannot be missed, and offer the one action that fixes it.
 self.updateLabel=label(@"",12,NSFontWeightMedium,NSColor.whiteColor);
 NSButton *relaunch=[NSButton buttonWithTitle:@"Relaunch" target:self action:@selector(relaunchForUpdate:)];
 relaunch.controlSize=NSControlSizeSmall;relaunch.keyEquivalent=@"\r";
 NSStackView *updateRow=[NSStackView stackViewWithViews:@[self.updateLabel,relaunch]];
 updateRow.spacing=12;updateRow.alignment=NSLayoutAttributeCenterY;
 updateRow.edgeInsets=NSEdgeInsetsMake(8,14,8,10);
 updateRow.wantsLayer=YES;updateRow.layer.cornerRadius=10;
 updateRow.layer.backgroundColor=[NSColor colorWithSRGBRed:.91 green:.64 blue:.24 alpha:.92].CGColor;
 self.updateLabel.textColor=[NSColor colorWithWhite:.08 alpha:1];
 self.updateBar=updateRow;self.updateBar.hidden=YES;
 [self.preview addSubview:self.updateBar];self.updateBar.translatesAutoresizingMaskIntoConstraints=NO;
 [NSLayoutConstraint activateConstraints:@[[self.updateBar.centerXAnchor constraintEqualToAnchor:self.preview.centerXAnchor],[self.updateBar.topAnchor constraintEqualToAnchor:self.preview.topAnchor constant:16]]];

 for(NSView *v in @[self.header,self.preview]) { [root addSubview:v];v.translatesAutoresizingMaskIntoConstraints=NO; }
 self.windowedLayout=@[
  [self.header.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:20],[self.header.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-20],
  [self.header.topAnchor constraintEqualToAnchor:root.topAnchor constant:26],
  [self.preview.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:18],[self.preview.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-18],
  [self.preview.topAnchor constraintEqualToAnchor:self.header.bottomAnchor constant:14],[self.preview.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-18]];
 self.fullScreenLayout=@[
  [self.preview.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],[self.preview.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
  [self.preview.topAnchor constraintEqualToAnchor:root.topAnchor],[self.preview.bottomAnchor constraintEqualToAnchor:root.bottomAnchor]];
 [NSLayoutConstraint activateConstraints:self.windowedLayout];

 self.launchVersion=[self versionOnDisk];
 // Started only with a feed to check. Sparkle logs an error and schedules
 // nothing without one, and a build with no feed configured is the normal
 // state of a working copy.
 self.updater=[[SPUStandardUpdaterController alloc] initWithStartingUpdater:NO updaterDelegate:self userDriverDelegate:nil];
 // Checked at every launch as well as on Sparkle's three-hour schedule: the
 // app is opened to capture something, and that is when an old driver hurts.
 if([self updateFeed].length) { [self.updater startUpdater];[self.updater.updater checkForUpdatesInBackground]; }
 [self buildMenu];
 [self applyFit:(int)[defaults integerForKey:@"previewFit"]];
 [self.window center];[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
 self.watchdog=[NSTimer scheduledTimerWithTimeInterval:.5 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
 self.meterTimer=[NSTimer scheduledTimerWithTimeInterval:1/20.0 target:self selector:@selector(meterTick:) userInfo:nil repeats:YES];
 [self startCapture];
 // Offered once the window is up, so the sheet has something to attach to.
 // After an update the app comes back as a new version, so this is where the
 // OBS source is brought along with it. It is a no-op once the two agree.
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.2*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
  if(!self.closing) HDSyncOBSPlugin(self.window,NO);
 });
}
- (void)buildMenu {
 NSMenu *menu=[[NSMenu alloc] init];
 NSMenuItem *appItem=[[NSMenuItem alloc] init];[menu addItem:appItem];
 NSMenu *appMenu=[[NSMenu alloc] init];
 NSMenuItem *updates=[[NSMenuItem alloc] initWithTitle:@"Check for Updates\u2026" action:@selector(checkForUpdates:) keyEquivalent:@""];
 updates.target=self;[appMenu addItem:updates];
 NSMenuItem *settings=[[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","];settings.target=self;[appMenu insertItem:settings atIndex:0];
 [appMenu insertItem:[NSMenuItem separatorItem] atIndex:1];
 [appMenu addItem:[NSMenuItem separatorItem]];
 [appMenu addItemWithTitle:@"Quit HD60 S Capture" action:@selector(terminate:) keyEquivalent:@"q"];appItem.submenu=appMenu;
 NSMenuItem *viewItem=[NSMenuItem new];[menu addItem:viewItem];
 NSMenu *viewMenu=[[NSMenu alloc] initWithTitle:@"View"];viewItem.title=@"View";viewItem.submenu=viewMenu;
 NSMenuItem *full=[[NSMenuItem alloc] initWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
 full.target=self;full.keyEquivalentModifierMask=NSEventModifierFlagControl|NSEventModifierFlagCommand;[viewMenu addItem:full];
 NSMenuItem *pause=[[NSMenuItem alloc] initWithTitle:@"Pause Capture" action:@selector(toggleCapture:) keyEquivalent:@" "];pause.target=self;pause.keyEquivalentModifierMask=0;[viewMenu addItem:pause];
 [viewMenu addItem:[NSMenuItem separatorItem]];
 NSMenuItem *record=[[NSMenuItem alloc] initWithTitle:@"Start Recording" action:@selector(toggleRecording:) keyEquivalent:@"r"];record.target=self;[viewMenu addItem:record];
 NSMenuItem *reveal=[[NSMenuItem alloc] initWithTitle:@"Show Recordings" action:@selector(showRecordings:) keyEquivalent:@"R"];reveal.target=self;[viewMenu addItem:reveal];
 [viewMenu addItem:[NSMenuItem separatorItem]];
 NSMenuItem *prefs=[[NSMenuItem alloc] initWithTitle:@"Capture Settings" action:@selector(showSettings:) keyEquivalent:@""];prefs.target=self;[viewMenu addItem:prefs];
 NSApp.mainMenu=menu;
}

#pragma mark - Settings

- (void)layoutPicture {
 NSRect bounds=self.preview.bounds,rect=bounds;
 if(haveLiveMode) {
  NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
  NSInteger fit=[defaults integerForKey:@"previewFit"];
  double aspect=hd_display_aspect(liveMode.width,liveMode.height,(int)[defaults integerForKey:@"displayAspect"]);
  if(fit!=2 && bounds.size.height>0) {
   BOOL byWidth=fit==1?bounds.size.width/bounds.size.height>aspect:bounds.size.width/bounds.size.height<aspect;
   if(byWidth)rect.size.height=rect.size.width/aspect;
   else rect.size.width=rect.size.height*aspect;
   rect.origin.x+=(bounds.size.width-rect.size.width)/2;
   rect.origin.y+=(bounds.size.height-rect.size.height)/2;
  }
 }
 [CATransaction begin];[CATransaction setDisableActions:YES];
 self.picture.contentsGravity=kCAGravityResize;self.picture.frame=rect;
 [CATransaction commit];
}
- (void)applyFit:(int)index {
 [[NSUserDefaults standardUserDefaults] setInteger:index forKey:@"previewFit"];
 [self layoutPicture];
}
- (void)aspectChanged:(NSSegmentedControl *)sender {
 [[NSUserDefaults standardUserDefaults] setInteger:sender.selectedSegment forKey:@"displayAspect"];
 [self fitWindowToInput];[self layoutPicture];
}
- (void)fitChanged:(NSSegmentedControl *)sender { [self applyFit:(int)sender.selectedSegment]; }
- (void)gainChanged:(NSSlider *)sender {
 atomic_store(&gainCentiBel,(int)lround(sender.doubleValue*100));
 [[NSUserDefaults standardUserDefaults] setDouble:sender.doubleValue forKey:@"audioGain"];
 self.gainReadout.stringValue=[NSString stringWithFormat:@"%+.1f dB",sender.doubleValue];
}
- (void)muteChanged:(NSButton *)sender {
 atomic_store(&muted,sender.state==NSControlStateValueOn);
 [[NSUserDefaults standardUserDefaults] setBool:atomic_load(&muted) forKey:@"audioMuted"];
}
- (void)limiterChanged:(NSButton *)sender {
 atomic_store(&limiterOn,sender.state==NSControlStateValueOn);
 [[NSUserDefaults standardUserDefaults] setBool:atomic_load(&limiterOn) forKey:@"audioLimiter"];
}
- (void)monitorChanged:(NSButton *)sender {
 bool on=sender.state==NSControlStateValueOn;
 if(on!=atomic_load(&monitor)) [self toggleAudio:nil];
}
- (void)overlayAlwaysChanged:(NSButton *)sender {
 [[NSUserDefaults standardUserDefaults] setBool:sender.state==NSControlStateValueOn forKey:@"overlayAlwaysVisible"];
 if(sender.state==NSControlStateValueOn) [self showOverlay];
}
- (void)qualityChanged:(NSSegmentedControl *)sender {
 [[NSUserDefaults standardUserDefaults] setInteger:sender.selectedSegment forKey:@"recordQuality"];
}
- (void)chooseRecordingFolder:(id)sender {
 (void)sender;
 NSOpenPanel *panel=[NSOpenPanel openPanel];
 panel.canChooseDirectories=YES;panel.canChooseFiles=NO;panel.canCreateDirectories=YES;
 panel.prompt=@"Use Folder";panel.directoryURL=[self recordingsFolder];
 [panel beginSheetModalForWindow:self.settingsWindow completionHandler:^(NSModalResponse response) {
  if(response!=NSModalResponseOK || !panel.URL) return;
  [[NSUserDefaults standardUserDefaults] setURL:panel.URL forKey:@"recordingFolder"];
  self.recordingFolderLabel.stringValue=panel.URL.path;
 }];
}
- (void)addSourceToOBS:(id)sender { (void)sender;HDSyncOBSPlugin(self.settingsWindow?:self.window,YES); }
- (void)refreshCaptureSummary {
 if(!self.settingsWindow.isVisible || !self.captureSummary) return;
 int speed=-1,product=hd_device_find(&speed);
 NSString *card=product==0x76?(speed>=0 && speed<HD_SPEED_SUPER?@"HD60 S revision 4, connected at USB 2.0 speed (too slow)":@"HD60 S revision 4, connected"):product>0?[NSString stringWithFormat:@"HD60 S older revision (product %04x), connected. Untested: it may not work",product]:@"No HD60 S found on USB";
 NSString *signal=self.chips.hidden?[NSString stringWithFormat:@"Signal: %@",self.badge.stringValue.lowercaseString]
  :[NSString stringWithFormat:@"Signal: %@ at %@, %@",self.sizeChip.stringValue,self.rateChip.stringValue,self.audioChip.stringValue];
 self.captureSummary.stringValue=[NSString stringWithFormat:@"%@\n%@",card,signal];
}
static NSTextField *settingsNote(NSString *text) {
 NSTextField *note=label(text,11,NSFontWeightRegular,NSColor.tertiaryLabelColor);
 // The width a pane actually lays this out at, so the height the pane reports
 // is the height the text really needs and the last line is not cut off.
 note.preferredMaxLayoutWidth=SETTINGS_WIDTH-2*SETTINGS_INSET;
 return note;
}
- (NSViewController *)paneTitled:(NSString *)title symbol:(NSString *)symbol rows:(NSArray<NSView *> *)rows {
 NSStackView *pane=[NSStackView stackViewWithViews:rows];
 pane.orientation=NSUserInterfaceLayoutOrientationVertical;pane.alignment=NSLayoutAttributeLeading;
 pane.spacing=12;pane.edgeInsets=NSEdgeInsetsMake(22,SETTINGS_INSET,22,SETTINGS_INSET);
 NSViewController *controller=[[NSViewController alloc] init];
 controller.view=pane;controller.title=title;
 // Each pane is only as tall as its own rows, so the window is not mostly empty.
 // Measured at the width it will really have: a wrapped label reports a
 // one-line height until something pins the width, and the last lines of the
 // explanatory notes then fall outside the window.
 NSLayoutConstraint *width=[pane.widthAnchor constraintEqualToConstant:SETTINGS_WIDTH];
 width.active=YES;
 [pane layoutSubtreeIfNeeded];
 controller.preferredContentSize=NSMakeSize(SETTINGS_WIDTH,ceil(pane.fittingSize.height));
 width.active=NO;
 // Carried here because NSTabViewController makes the tab item, not this method.
 controller.representedObject=symbol;
 return controller;
}
- (NSViewController *)buildAudioPane {
 NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
 NSButton *play=[NSButton checkboxWithTitle:@"Play this capture through the Mac" target:self action:@selector(monitorChanged:)];
 play.state=atomic_load(&monitor)?NSControlStateValueOn:NSControlStateValueOff;
 double gain=[defaults doubleForKey:@"audioGain"];
 self.gainSlider=[NSSlider sliderWithValue:gain minValue:-30 maxValue:6 target:self action:@selector(gainChanged:)];
 self.gainReadout=label([NSString stringWithFormat:@"%+.1f dB",gain],12,NSFontWeightRegular,NSColor.secondaryLabelColor);
 self.gainReadout.font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
 [self.gainReadout.widthAnchor constraintEqualToConstant:62].active=YES;self.gainReadout.alignment=NSTextAlignmentRight;
 [self.gainSlider.widthAnchor constraintEqualToConstant:220].active=YES;
 NSStackView *gainRow=[NSStackView stackViewWithViews:@[label(@"Gain",12,NSFontWeightRegular,NSColor.labelColor),self.gainSlider,self.gainReadout]];
 gainRow.spacing=10;gainRow.alignment=NSLayoutAttributeCenterY;
 NSButton *mute=[NSButton checkboxWithTitle:@"Mute playback (the recording keeps its sound)" target:self action:@selector(muteChanged:)];
 mute.state=atomic_load(&muted)?NSControlStateValueOn:NSControlStateValueOff;
 NSButton *limiter=[NSButton checkboxWithTitle:@"Hold peaks under −1 dBFS" target:self action:@selector(limiterChanged:)];
 limiter.state=atomic_load(&limiterOn)?NSControlStateValueOn:NSControlStateValueOff;
 self.peakReadout=label(@"Peak: none",11,NSFontWeightRegular,NSColor.tertiaryLabelColor);
 self.peakReadout.font=[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
 return [self paneTitled:@"Audio" symbol:@"speaker.wave.2" rows:@[play,gainRow,mute,limiter,self.peakReadout,
  settingsNote(@"Gain is applied before the meter reading above, to playback, and to recordings.")]];
}
- (NSViewController *)buildVideoPane {
 NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
 NSSegmentedControl *fit=[NSSegmentedControl segmentedControlWithLabels:@[@"Fit",@"Fill",@"Stretch"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(fitChanged:)];
 fit.selectedSegment=[defaults integerForKey:@"previewFit"];
 NSStackView *fitRow=[NSStackView stackViewWithViews:@[label(@"Picture",12,NSFontWeightRegular,NSColor.labelColor),fit]];
 fitRow.spacing=10;fitRow.alignment=NSLayoutAttributeCenterY;
 NSSegmentedControl *aspect=[NSSegmentedControl segmentedControlWithLabels:@[@"Auto",@"4:3",@"16:9"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(aspectChanged:)];
 aspect.selectedSegment=[defaults integerForKey:@"displayAspect"];
 NSStackView *aspectRow=[NSStackView stackViewWithViews:@[label(@"Aspect ratio",12,NSFontWeightRegular,NSColor.labelColor),aspect]];
 aspectRow.spacing=10;aspectRow.alignment=NSLayoutAttributeCenterY;
 NSButton *keepBar=[NSButton checkboxWithTitle:@"Keep the control bar on screen in full screen" target:self action:@selector(overlayAlwaysChanged:)];
 keepBar.state=[defaults boolForKey:@"overlayAlwaysVisible"]?NSControlStateValueOn:NSControlStateValueOff;
 return [self paneTitled:@"Video" symbol:@"rectangle.on.rectangle" rows:@[fitRow,aspectRow,keepBar,
  settingsNote(@"Auto displays SD video as 4:3. Select 16:9 for anamorphic widescreen. Fit preserves that shape, Fill crops, and Stretch fills the window. Aspect ratio applies to preview.")]];
}
- (NSViewController *)buildRecordingPane {
 NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
 self.recordingFolderLabel=label([self recordingsFolder].path,11,NSFontWeightRegular,NSColor.secondaryLabelColor);
 self.recordingFolderLabel.lineBreakMode=NSLineBreakByTruncatingMiddle;
 [self.recordingFolderLabel.widthAnchor constraintEqualToConstant:230].active=YES;
 NSButton *change=[NSButton buttonWithTitle:@"Change…" target:self action:@selector(chooseRecordingFolder:)];
 NSButton *reveal=[NSButton buttonWithTitle:@"Show" target:self action:@selector(showRecordings:)];
 NSStackView *folderRow=[NSStackView stackViewWithViews:@[self.recordingFolderLabel,change,reveal]];
 folderRow.spacing=8;folderRow.alignment=NSLayoutAttributeCenterY;
 NSSegmentedControl *quality=[NSSegmentedControl segmentedControlWithLabels:@[@"Standard",@"High"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(qualityChanged:)];
 quality.selectedSegment=[defaults integerForKey:@"recordQuality"];
 NSStackView *qualityRow=[NSStackView stackViewWithViews:@[label(@"Quality",12,NSFontWeightRegular,NSColor.labelColor),quality]];
 qualityRow.spacing=10;qualityRow.alignment=NSLayoutAttributeCenterY;
 return [self paneTitled:@"Recording" symbol:@"record.circle" rows:@[label(@"Save movies to",12,NSFontWeightRegular,NSColor.labelColor),folderRow,qualityRow,
  settingsNote(@"H.264 and AAC in a QuickTime movie, at the input's own resolution and frame rate, through the Apple silicon encoder. High roughly doubles the bitrate, and the setting applies to the next recording.")]];
}
- (NSViewController *)buildCapturePane {
 self.captureSummary=label(@"Checking the card…",12,NSFontWeightRegular,NSColor.labelColor);
 self.captureSummary.font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
 self.captureSummary.preferredMaxLayoutWidth=SETTINGS_WIDTH-2*SETTINGS_INSET;
 NSButton *obs=[NSButton buttonWithTitle:@"Update the source in OBS now" target:self action:@selector(addSourceToOBS:)];
 return [self paneTitled:@"Capture" symbol:@"cable.connector" rows:@[self.captureSummary,obs,
  settingsNote(@"One bundled service owns the card and is the driver, so this app and the OBS source both receive from it. When this app updates, OBS shows a short gap while the new driver takes over, then carries on; it is never closed.")]];
}
- (void)showSettings:(id)sender {
 (void)sender;
 [self showOverlay];
 if(!self.settingsWindow) {
  NSTabViewController *tabs=[[NSTabViewController alloc] init];
  tabs.tabStyle=NSTabViewControllerTabStyleToolbar;
  NSArray<NSViewController *> *panes=@[[self buildAudioPane],[self buildVideoPane],[self buildRecordingPane],[self buildCapturePane]];
  // The window takes its height once and keeps it, so every pane is given the
  // tallest pane's height: otherwise the longest note is cut off, and the
  // window would jump about on each tab if it did resize.
  CGFloat tallest=0;
  for(NSViewController *pane in panes) tallest=fmax(tallest,pane.preferredContentSize.height);
  for(NSViewController *pane in panes) {
   pane.preferredContentSize=NSMakeSize(SETTINGS_WIDTH,tallest);
   [tabs addChildViewController:pane];
   NSImage *image=[NSImage imageWithSystemSymbolName:pane.representedObject accessibilityDescription:pane.title];
   if(image) [tabs tabViewItemForViewController:pane].image=image;
  }
  tabs.selectedTabViewItemIndex=0;
  NSWindow *window=[HDSettingsWindow windowWithContentViewController:tabs];
  window.title=@"HD60 S Capture Settings";
  window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  window.releasedWhenClosed=NO;
  [window center];
  self.settingsWindow=window;
 }
 [self refreshCaptureSummary];
 [self.settingsWindow makeKeyAndOrderFront:nil];
}

#pragma mark - Updates

- (NSString *)updateFeed {
 id feed=NSBundle.mainBundle.infoDictionary[@"SUFeedURL"];
 return [feed isKindOfClass:NSString.class]?feed:nil;
}
/* Sparkle's automatic mode downloads silently and then waits for the app to
 * quit, which a capture window left open rarely does, so "install
 * automatically" looked like it did nothing. Install now and relaunch, unless
 * a recording is running; then it still waits for quit. */
- (BOOL)updater:(SPUUpdater *)updater willInstallUpdateOnQuit:(SUAppcastItem *)item immediateInstallationBlock:(void (^)(void))install {
 (void)updater;(void)item;
 if(atomic_load(&recording)) return NO;
 install();return YES;
}
- (void)checkForUpdates:(id)sender {
 if([self updateFeed].length) { [self.updater checkForUpdates:sender];return; }
 // No dead menu item: say why nothing happens rather than failing silently.
 NSAlert *alert=[[NSAlert alloc] init];
 alert.messageText=@"This build has no update feed";
 alert.informativeText=@"It was built from source rather than installed from a release, so there is nowhere to check. Run make bundle to rebuild it, and the window will offer to relaunch.";
 [alert addButtonWithTitle:@"OK"];
 [alert beginSheetModalForWindow:self.window completionHandler:nil];
}
- (NSString *)versionOnDisk {
 NSDictionary *plist=[NSDictionary dictionaryWithContentsOfURL:[NSBundle.mainBundle.bundleURL URLByAppendingPathComponent:@"Contents/Info.plist"]];
 id version=plist[@"CFBundleVersion"];
 return [version isKindOfClass:NSString.class]?version:nil;
}
- (void)checkForUpdate {
 NSString *onDisk=[self versionOnDisk];
 BOOL newer=onDisk && self.launchVersion && ![onDisk isEqualToString:self.launchVersion];
 if(newer) self.updateLabel.stringValue=[NSString stringWithFormat:@"Version %@ is installed. This window is still running %@.",onDisk,self.launchVersion];
 self.updateBar.hidden=!newer;
}
- (BOOL)obsIsRunning { return [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.obsproject.obs-studio"].count>0; }
/* Quitting is the only way a running binary picks up a new one, for this app and
 * for the OBS plugin alike, so the relaunch is handed to a detached shell that
 * outlives both of us. */
- (void)relaunchApps:(BOOL)withOBS {
 NSString *script=[NSString stringWithFormat:@"sleep 2; /usr/bin/open %@\"%@\"",
  withOBS?@"-b com.obsproject.obs-studio; sleep 1; /usr/bin/open ":@"",NSBundle.mainBundle.bundlePath];
 NSTask *task=[[NSTask alloc] init];
 task.executableURL=[NSURL fileURLWithPath:@"/bin/sh"];task.arguments=@[@"-c",script];
 NSError *error=nil;
 if(![task launchAndReturnError:&error]) { [self showNote:[NSString stringWithFormat:@"Could not relaunch: %@",error.localizedDescription] error:YES];return; }
 if(withOBS) for(NSRunningApplication *obs in [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.obsproject.obs-studio"]) [obs terminate];
 [NSApp terminate:nil];
}
- (void)relaunchForUpdate:(id)sender {
 (void)sender;
 NSAlert *alert=[[NSAlert alloc] init];
 alert.messageText=@"Relaunch to use the installed version?";
 if(atomic_load(&recording)) alert.informativeText=@"The recording in progress will be finished and saved first.";
 if([self obsIsRunning]) {
  alert.informativeText=[NSString stringWithFormat:@"%@OBS is running. Its copy of the source keeps the old code until OBS itself restarts, and quitting OBS stops anything it is streaming or recording.",
   atomic_load(&recording)?@"The recording in progress will be finished and saved first. ":@""];
  [alert addButtonWithTitle:@"Relaunch This App"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert addButtonWithTitle:@"Quit OBS and Relaunch Both"];
 } else {
  [alert addButtonWithTitle:@"Relaunch"];
  [alert addButtonWithTitle:@"Cancel"];
 }
 [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
  if(response==NSAlertSecondButtonReturn) return;
  BOOL withOBS=response==NSAlertThirdButtonReturn;
  if(atomic_load(&self->recording)) [self stopRecording:nil completion:^{ [self relaunchApps:withOBS]; }];
  else [self relaunchApps:withOBS];
 }];
}

#pragma mark - Overlay auto-hide

- (BOOL)isFullScreen { return (self.window.styleMask&NSWindowStyleMaskFullScreen)!=0; }
- (BOOL)overlayPinned { return [NSUserDefaults.standardUserDefaults boolForKey:@"overlayAlwaysVisible"]; }
- (void)showOverlay {
 self.lastPointer=[NSDate date];
 if(self.overlay.hidden) { self.overlay.hidden=NO;self.overlay.alphaValue=0; }
 [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) { context.duration=.15;self.overlay.animator.alphaValue=1; }];
}
- (void)hideOverlay {
 if(self.overlay.hidden || self.overlay.alphaValue==0) return;
 [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) { context.duration=.35;self.overlay.animator.alphaValue=0; }
  completionHandler:^{ if(self.overlay.alphaValue==0) self.overlay.hidden=YES; }];
 [NSCursor setHiddenUntilMouseMoves:YES];
}
- (void)mouseMoved:(NSEvent *)event { (void)event;[self showOverlay]; }
- (void)mouseEntered:(NSEvent *)event { (void)event;[self showOverlay]; }
- (void)mouseExited:(NSEvent *)event { (void)event;if([self isFullScreen])self.lastPointer=[NSDate dateWithTimeIntervalSinceNow:-OVERLAY_IDLE]; }

#pragma mark - Timers

- (void)showNote:(NSString *)text error:(BOOL)error {
 self.note=text;self.noteIsError=error;self.noteUntil=[NSDate dateWithTimeIntervalSinceNow:6];
 [self updateModeLine];
}
- (void)updateModeLine {
 BOOL noteLives=self.note && self.noteUntil && self.noteUntil.timeIntervalSinceNow>0;
 self.mode.stringValue=noteLives?self.note:(self.statusHint?:cardLine());
 self.mode.textColor=noteLives&&self.noteIsError?NSColor.systemRedColor:NSColor.secondaryLabelColor;
}
- (void)tick:(NSTimer *)timer {
 (void)timer;[self updateModeLine];[self refreshCaptureSummary];
 if(++self.updateTicks%20==0) [self checkForUpdate];
 [CATransaction begin];[CATransaction setDisableActions:YES];[self layoutPicture];[CATransaction commit];
 if(!self.placeholder.hidden && self.statusKey && self.shownTitle) {
  NSArray *parts=[self.statusKey componentsSeparatedByString:@"|"];
  NSSize p=self.preview.bounds.size;CGFloat b=self.window.backingScaleFactor>0?self.window.backingScaleFactor:2;
  if(parts.count==4 && ((unsigned)lround(fmin(p.width*b,4096))!=(unsigned)[parts[2] intValue] || (unsigned)lround(fmin(p.height*b,2160))!=(unsigned)[parts[3] intValue])) {
   self.statusKey=nil;[self showStatus:self.shownStatus title:self.shownTitle detail:self.shownDetail];
  }
 }
 NSString *slow=self.paused?nil:slowLinkDetail();
 if(!self.paused && hd_device_product()==0) [self displayStatus:HD_DISCONNECTED detail:@"Connect your HD60 S using a USB 3 cable. It will start automatically."];
 else if(slow) [self displayStatus:HD_ERROR detail:slow];
 else if(self.lastFrame && -self.lastFrame.timeIntervalSinceNow>2 && self.placeholder.hidden) [self displayStatus:HD_INITIALIZING detail:@"Waiting for fresh video. Checking the HDMI signal…"];
}
- (void)meterTick:(NSTimer *)timer {
 (void)timer;
 float peak=atomic_exchange(&meterPeak,0.0f);
 // Instant attack, ~1.5 s visual decay, so brief transients stay readable.
 self.meterLevel=peak>self.meterLevel?peak:fmaxf(0,self.meterLevel-.035f);
 self.meter.doubleValue=self.meterLevel;
 if(atomic_load(&recording) && self.recordingStart) {
  NSString *text=elapsedText(-self.recordingStart.timeIntervalSinceNow);
  if(![self.elapsed.stringValue isEqualToString:text]) self.elapsed.stringValue=text;
 }
 if(self.settingsWindow.isVisible) self.peakReadout.stringValue=self.meterLevel>0.0002?[NSString stringWithFormat:@"Peak: %.1f dBFS",20*log10f(self.meterLevel)]:@"Peak: none";
 // Auto-hide the control bar only in full screen; windowed keeps it visible.
 if([self isFullScreen] && !self.settingsWindow.isVisible && !self.overlayPinned && self.lastPointer && -self.lastPointer.timeIntervalSinceNow>OVERLAY_IDLE) [self hideOverlay];
}

#pragma mark - Status

- (void)showStatus:(hd_status)status title:(NSString *)title detail:(NSString *)detail {
 NSSize points=self.preview.bounds.size;
 if(points.width<160 || points.height<120) return;
 CGFloat backing=self.window.backingScaleFactor>0?self.window.backingScaleFactor:2;
 unsigned w=(unsigned)lround(fmin(points.width*backing,4096)),h=(unsigned)lround(fmin(points.height*backing,2160));
 NSString *key=[NSString stringWithFormat:@"%d|%@|%u|%u",(int)status,title,w,h];
 if([key isEqualToString:self.statusKey]) return;
 self.shownStatus=status;self.shownTitle=title;self.shownDetail=detail;
 uint8_t *bytes=hd_status_frame_text(w,h,status,title.UTF8String,detail.UTF8String);
 if(!bytes) return;
 CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
 CGContextRef cg=CGBitmapContextCreate(bytes,w,h,8,w*4,rgb,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
 CGImageRef image=cg?CGBitmapContextCreateImage(cg):NULL;
 if(cg)CGContextRelease(cg);
 CGColorSpaceRelease(rgb);free(bytes);
 if(!image) return;
 self.placeholder.image=[[NSImage alloc] initWithCGImage:image size:points];
 CGImageRelease(image);self.statusKey=key;
}
- (void)displayStatus:(hd_status)value detail:(NSString *)detail {
 if(self.paused || self.closing) return;
 if(value==HD_LIVE) return; // Only a delivered frame removes the placeholder.
 self.picture.contents=nil;self.placeholder.hidden=NO;self.lastFrame=nil;
 NSArray *titles=@[@"No capture card",@"Starting up",@"No signal",@"Live",@"Card already in use",@"Capture stopped"];
 [self showStatus:value title:titles[value] detail:detail];
 self.badge.stringValue=value==HD_NO_SIGNAL?@"No signal":value==HD_DISCONNECTED?@"Disconnected":value==HD_BUSY?@"In use":value==HD_ERROR?@"Error":@"Connecting";
 NSColor *tint=value==HD_DISCONNECTED?NSColor.secondaryLabelColor:value==HD_ERROR?NSColor.systemRedColor:NSColor.systemOrangeColor;
 self.badgeDot.layer.backgroundColor=tint.CGColor;self.badge.textColor=value==HD_DISCONNECTED?NSColor.secondaryLabelColor:NSColor.whiteColor;
 self.chips.hidden=YES;
 self.statusHint=value==HD_NO_SIGNAL?@"Connected, waiting for an HDMI source":cardLine();
 [self updateModeLine];
 pthread_mutex_lock(&audioLock);audioCount=0;audioRead=audioWrite=0;pthread_mutex_unlock(&audioLock);
}

#pragma mark - Capture control

- (void)startCapture { atomic_store(&stop,false);atomic_fetch_add(&session,1);workerStarted=pthread_create(&worker,NULL,captureWorker,(__bridge void *)self)==0; }
- (void)stopCapture { atomic_store(&stop,true);atomic_fetch_add(&session,1);if(workerStarted) { pthread_join(worker,NULL);workerStarted=NO; } }
- (BOOL)validateMenuItem:(NSMenuItem *)item {
 if(item.action==@selector(toggleRecording:)) item.title=atomic_load(&recording)?@"Stop Recording":@"Start Recording";
 else if(item.action==@selector(toggleCapture:)) item.title=self.paused?@"Resume Capture":@"Pause Capture";
 return YES;
}
- (void)toggleCapture:(id)sender {
 (void)sender;
 if(atomic_load(&recording)) [self stopRecording:@"capture was paused" completion:nil];
 if(self.paused) {
  self.paused=NO;setSymbol(self.pauseButton,@"pause.fill",@"Pause capture");
  [self displayStatus:HD_INITIALIZING detail:@"Preparing your capture card…"];[self startCapture];
 } else {
  self.paused=YES;setSymbol(self.pauseButton,@"play.fill",@"Resume capture");[self stopCapture];
  self.picture.contents=nil;self.placeholder.hidden=NO;
  // The card really is released while paused, so the chain shows idle, not held.
  [self showStatus:HD_INITIALIZING title:@"Capture paused" detail:@"Resume to preview here. OBS and other apps keep receiving video and audio."];
  self.badge.stringValue=@"PAUSED";self.badge.textColor=NSColor.secondaryLabelColor;self.badgeDot.layer.backgroundColor=NSColor.tertiaryLabelColor.CGColor;
  pthread_mutex_lock(&audioLock);audioCount=0;pthread_mutex_unlock(&audioLock);
 }
}
- (void)toggleFullScreen:(id)sender { (void)sender;[self.window toggleFullScreen:nil]; }
// Preview insets are 36 horizontally and 114 vertically, including the header.
- (NSSize)windowWillResize:(NSWindow *)window toSize:(NSSize)size {
 if(haveLiveMode && ![self isFullScreen]) {
  CGFloat overhead=window.frame.size.height-self.preview.bounds.size.height;
  size.height=(size.width-36)/hd_display_aspect(liveMode.width,liveMode.height,(int)[NSUserDefaults.standardUserDefaults integerForKey:@"displayAspect"])+overhead;
 }
 return size;
}
- (void)fitWindowToInput {
 if(!haveLiveMode || [self isFullScreen]) return;
 [self.window.contentView layoutSubtreeIfNeeded];
 NSRect frame=self.window.frame;
 NSSize size=[self windowWillResize:self.window toSize:frame.size];
 NSRect visible=self.window.screen.visibleFrame;
 if(size.height>visible.size.height) {
  CGFloat overhead=frame.size.height-self.preview.bounds.size.height;
  size.height=visible.size.height;
  size.width=(size.height-overhead)*hd_display_aspect(liveMode.width,liveMode.height,(int)[NSUserDefaults.standardUserDefaults integerForKey:@"displayAspect"])+36;
 }
 frame.origin.y+=frame.size.height-size.height;frame.size=size;
 [self.window setFrame:frame display:YES];
 [self.window.contentView layoutSubtreeIfNeeded];
 [self layoutPicture];
}
- (void)windowDidEnterFullScreen:(NSNotification *)notification {
 (void)notification;
 self.header.hidden=YES;
 [NSLayoutConstraint deactivateConstraints:self.windowedLayout];[NSLayoutConstraint activateConstraints:self.fullScreenLayout];
 self.preview.layer.cornerRadius=0;self.preview.layer.borderWidth=0;
 setSymbol(self.fullScreenButton,@"arrow.down.right.and.arrow.up.left",@"Exit full screen");
 [self showOverlay];
}
- (void)windowDidExitFullScreen:(NSNotification *)notification {
 (void)notification;
 self.header.hidden=NO;
 [NSLayoutConstraint deactivateConstraints:self.fullScreenLayout];[NSLayoutConstraint activateConstraints:self.windowedLayout];
 self.preview.layer.cornerRadius=12;self.preview.layer.borderWidth=1;
 [self fitWindowToInput];
 setSymbol(self.fullScreenButton,@"arrow.up.left.and.arrow.down.right",@"Full screen");
 [self showOverlay];
}
- (void)cancelOperation:(id)sender { (void)sender;if([self isFullScreen])[self.window toggleFullScreen:nil]; }
- (void)toggleAudio:(id)sender {
 (void)sender;
 bool on=!atomic_load(&monitor);atomic_store(&monitor,on);
 [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"playCaptureAudio"];
 setSymbol(self.audioButton,on?@"speaker.wave.2.fill":@"speaker.slash.fill",on?@"Mute playback":@"Play capture audio");
 pthread_mutex_lock(&audioLock);audioCount=0;audioRead=audioWrite=0;pthread_mutex_unlock(&audioLock);
}

#pragma mark - Audio

- (void)pushAudio:(const uint8_t *)data bytes:(size_t)bytes rate:(unsigned)rate timestamp:(uint64_t)timestamp {
 if(bytes>4096 || bytes%4 || rate<8000) return;
 // Always process so the level meter works with monitoring switched off. Mute is applied
 // to playback only, below: it silences monitoring, not the recording.
 float output[2048];
 size_t samples=bytes/2;
 hd_audio_process(&audioState,data,output,samples,atomic_load(&gainCentiBel)/100.0,0,atomic_load(&limiterOn),rate);
 float previous=atomic_load(&meterPeak);
 if(audioState.peak>previous) atomic_store(&meterPeak,audioState.peak);
 int16_t pcm[2048];
 for(size_t i=0;i<samples;i++) pcm[i]=(int16_t)lrintf(fmaxf(-1,fminf(1,output[i]))*32767);
 if(atomic_load(&recording)) [self recordAudio:(const uint8_t *)pcm bytes:bytes rate:rate timestamp:timestamp];
 if(!atomic_load(&monitor)) {
  if(audioQueue) { AudioQueueDispose(audioQueue,true);audioQueue=NULL;audioRate=0; }
  return;
 }
 if(audioRate!=rate) {
  if(audioQueue) { AudioQueueDispose(audioQueue,true);audioQueue=NULL; }
  audioRate=rate;AudioStreamBasicDescription format={.mSampleRate=rate,.mFormatID=kAudioFormatLinearPCM,.mFormatFlags=kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,.mBytesPerPacket=4,.mFramesPerPacket=1,.mBytesPerFrame=4,.mChannelsPerFrame=2,.mBitsPerChannel=16};
  OSStatus created=AudioQueueNewOutput(&format,audioFill,(__bridge void *)self,NULL,NULL,0,&audioQueue);
  if(created) { audioQueue=NULL;audioRate=0;dispatch_async(dispatch_get_main_queue(),^{ self.audioButton.toolTip=[NSString stringWithFormat:@"Audio unavailable (%d)",(int)created]; });return; }
  for(int i=0;i<3;i++) { AudioQueueBufferRef buffer=NULL;if(!AudioQueueAllocateBuffer(audioQueue,rate/100*4,&buffer)) { memset(buffer->mAudioData,0,buffer->mAudioDataBytesCapacity);buffer->mAudioDataByteSize=buffer->mAudioDataBytesCapacity;AudioQueueEnqueueBuffer(audioQueue,buffer,0,NULL); } }
  OSStatus started=AudioQueueStart(audioQueue,NULL);
  if(started) { AudioQueueDispose(audioQueue,true);audioQueue=NULL;audioRate=0; }
  dispatch_async(dispatch_get_main_queue(),^{ self.audioButton.toolTip=started?[NSString stringWithFormat:@"Audio unavailable (%d)",(int)started]:@"Mute playback"; });
  if(started) return;
 }
 BOOL silent=atomic_load(&muted);
 pthread_mutex_lock(&audioLock);
 // Bound monitoring latency to 100 ms; discard stale audio after an output stall.
 if(audioCount+bytes>rate/10*4) { audioCount=0;audioRead=audioWrite=0; }
 for(size_t i=0;i<samples;i++) {
  uint16_t raw=silent?0:(uint16_t)pcm[i];
  audioRing[audioWrite]=raw&255;audioWrite=(audioWrite+1)%AUDIO_CAPACITY;
  audioRing[audioWrite]=raw>>8;audioWrite=(audioWrite+1)%AUDIO_CAPACITY;audioCount+=2;
 }
 pthread_mutex_unlock(&audioLock);
}

#pragma mark - Recording

static NSString *elapsedText(NSTimeInterval seconds) {
 int total=(int)seconds;
 return total>=3600?[NSString stringWithFormat:@"%d:%02d:%02d",total/3600,total/60%60,total%60]
                   :[NSString stringWithFormat:@"%02d:%02d",total/60,total%60];
}
- (void)recordingFailed:(NSString *)why {
 [self showNote:[NSString stringWithFormat:@"Recording failed: %@",why] error:YES];
}
- (NSURL *)recordingsFolder {
 NSURL *chosen=[NSUserDefaults.standardUserDefaults URLForKey:@"recordingFolder"];
 if(chosen) return chosen;
 NSURL *movies=[NSFileManager.defaultManager URLsForDirectory:NSMoviesDirectory inDomains:NSUserDomainMask].firstObject;
 return [movies URLByAppendingPathComponent:@"HD60 S Capture"];
}
- (void)startRecording {
 if(!haveLiveMode || self.paused) { [self recordingFailed:@"needs a live signal"];return; }
 hd_mode mode=self->liveMode;
 NSError *error=nil;
 NSURL *folder=[self recordingsFolder];
 if(![NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:&error]) { [self recordingFailed:error.localizedDescription];return; }
 NSDateFormatter *stamp=[NSDateFormatter new];stamp.dateFormat=@"yyyy-MM-dd HH-mm-ss";stamp.locale=[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
 NSURL *url=[folder URLByAppendingPathComponent:[NSString stringWithFormat:@"HD60 S %@.mov",[stamp stringFromDate:NSDate.date]]];
 AVAssetWriter *writer=[AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
 if(!writer) { [self recordingFailed:error.localizedDescription];return; }

 double fps=mode.rate/(mode.fractional?1.001:1);if(fps<1||fps>240)fps=60;
 double perPixel=[NSUserDefaults.standardUserDefaults integerForKey:@"recordQuality"]?.38:.2;
 NSInteger bitrate=(NSInteger)fmin(90e6,fmax(4e6,(double)mode.width*mode.height*fps*perPixel));
 NSDictionary *color=@{AVVideoColorPrimariesKey:mode.bt709?AVVideoColorPrimaries_ITU_R_709_2:AVVideoColorPrimaries_SMPTE_C,
  AVVideoTransferFunctionKey:AVVideoTransferFunction_ITU_R_709_2,
  AVVideoYCbCrMatrixKey:mode.bt709?AVVideoYCbCrMatrix_ITU_R_709_2:AVVideoYCbCrMatrix_ITU_R_601_4};
 AVAssetWriterInput *video=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
  AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@(mode.width),AVVideoHeightKey:@(mode.height),
  AVVideoColorPropertiesKey:color,
  AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@(bitrate),AVVideoExpectedSourceFrameRateKey:@((int)lround(fps)),
   AVVideoMaxKeyFrameIntervalKey:@((int)lround(fps*2)),AVVideoProfileLevelKey:AVVideoProfileLevelH264HighAutoLevel}}];
 video.expectsMediaDataInRealTime=YES;
 AVAssetWriterInputPixelBufferAdaptor *adaptor=[AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:video sourcePixelBufferAttributes:@{
  (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_422YpCbCr8_yuvs),(id)kCVPixelBufferWidthKey:@(mode.width),(id)kCVPixelBufferHeightKey:@(mode.height)}];
 unsigned rate=mode.audio_rate>=8000?mode.audio_rate:48000;
 AVAssetWriterInput *audio=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
  AVFormatIDKey:@(kAudioFormatMPEG4AAC),AVNumberOfChannelsKey:@2,AVSampleRateKey:@(rate),AVEncoderBitRateKey:@192000}];
 audio.expectsMediaDataInRealTime=YES;
 if(![writer canAddInput:video] || ![writer canAddInput:audio]) { [self recordingFailed:@"unsupported input format"];return; }
 [writer addInput:video];[writer addInput:audio];
 if(![writer startWriting]) { [self recordingFailed:writer.error.localizedDescription?:@"could not start writing"];return; }

 AudioStreamBasicDescription asbd={.mSampleRate=rate,.mFormatID=kAudioFormatLinearPCM,
  .mFormatFlags=kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,.mBytesPerPacket=4,
  .mFramesPerPacket=1,.mBytesPerFrame=4,.mChannelsPerFrame=2,.mBitsPerChannel=16};
 CMAudioFormatDescriptionRef format=NULL;
 if(CMAudioFormatDescriptionCreate(NULL,&asbd,0,NULL,0,NULL,NULL,&format)) { [writer cancelWriting];[self recordingFailed:@"audio format description"];return; }

 pthread_mutex_lock(&writerLock);
 self.writer=writer;self.videoInput=video;self.audioInput=audio;self.adaptor=adaptor;
 if(recordAudioFormat)CFRelease(recordAudioFormat);
 recordAudioFormat=format;recordMode=mode;recordEpoch=0;recordSessionStarted=NO;
 pthread_mutex_unlock(&writerLock);

 self.recordingURL=url;self.recordingStart=[NSDate date];
 atomic_store(&recording,true);
 setSymbol(self.recordButton,@"stop.fill",@"Stop recording");
 self.recordButton.contentTintColor=NSColor.systemRedColor;
 self.elapsed.hidden=NO;self.elapsed.stringValue=@"00:00";
 [self showNote:[NSString stringWithFormat:@"Recording to %@",url.lastPathComponent] error:NO];
}
- (void)stopRecording:(NSString *)reason completion:(void (^)(void))done {
 if(!atomic_load(&recording)) { if(done)done();return; }
 atomic_store(&recording,false);
 pthread_mutex_lock(&writerLock);
 AVAssetWriter *writer=self.writer;AVAssetWriterInput *video=self.videoInput,*audio=self.audioInput;
 BOOL started=recordSessionStarted;
 self.writer=nil;self.videoInput=nil;self.audioInput=nil;self.adaptor=nil;
 if(recordAudioFormat) { CFRelease(recordAudioFormat);recordAudioFormat=NULL; }
 pthread_mutex_unlock(&writerLock);

 NSURL *url=self.recordingURL;
 setSymbol(self.recordButton,@"record.circle",@"Record");
 self.recordButton.contentTintColor=NSColor.whiteColor;
 self.elapsed.hidden=YES;
 if(!writer || writer.status!=AVAssetWriterStatusWriting || !started) {
  [writer cancelWriting];
  if(!self.closing) [self recordingFailed:reason?:@"no frames were written"];
  if(done)done();return;
 }
 [video markAsFinished];[audio markAsFinished];
 [writer finishWritingWithCompletionHandler:^{
  dispatch_async(dispatch_get_main_queue(),^{
   if(!self.closing) {
    if(writer.status==AVAssetWriterStatusCompleted) [self showNote:[NSString stringWithFormat:@"Saved %@",url.lastPathComponent] error:NO];
    else [self recordingFailed:writer.error.localizedDescription?:@"write did not complete"];
   }
   if(done)done();
  });
 }];
}
- (void)toggleRecording:(id)sender {
 (void)sender;
 if(atomic_load(&recording)) [self stopRecording:nil completion:nil];
 else [self startRecording];
}
- (void)showRecordings:(id)sender {
 (void)sender;NSURL *folder=[self recordingsFolder];
 [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
 [NSWorkspace.sharedWorkspace openURL:folder];
}
- (void)recordVideo:(CVPixelBufferRef)pixel timestamp:(uint64_t)timestamp mode:(const hd_mode *)mode {
 BOOL geometryChanged=NO;
 pthread_mutex_lock(&writerLock);
 if(self.writer.status==AVAssetWriterStatusWriting) {
  if(mode->width!=recordMode.width || mode->height!=recordMode.height) geometryChanged=YES;
  else {
   if(!recordSessionStarted) { recordEpoch=timestamp;[self.writer startSessionAtSourceTime:kCMTimeZero];recordSessionStarted=YES; }
   if(timestamp>=recordEpoch && self.videoInput.readyForMoreMediaData)
    [self.adaptor appendPixelBuffer:pixel withPresentationTime:CMTimeMake((int64_t)(timestamp-recordEpoch),1000000000)];
  }
 }
 pthread_mutex_unlock(&writerLock);
 if(geometryChanged) dispatch_async(dispatch_get_main_queue(),^{ [self stopRecording:@"the input resolution changed" completion:nil]; });
}
- (void)recordAudio:(const uint8_t *)pcm bytes:(size_t)bytes rate:(unsigned)rate timestamp:(uint64_t)timestamp {
 pthread_mutex_lock(&writerLock);
 // Audio before the first video frame has no session to land in; drop it.
 if(self.writer.status==AVAssetWriterStatusWriting && recordSessionStarted && recordAudioFormat
    && timestamp>=recordEpoch && rate==recordMode.audio_rate && self.audioInput.readyForMoreMediaData) {
  CMBlockBufferRef block=NULL;
  if(!CMBlockBufferCreateWithMemoryBlock(NULL,NULL,bytes,kCFAllocatorDefault,NULL,0,bytes,kCMBlockBufferAssureMemoryNowFlag,&block)
     && !CMBlockBufferReplaceDataBytes(pcm,block,0,bytes)) {
   CMSampleBufferRef sample=NULL;
   if(!CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL,block,recordAudioFormat,(CMItemCount)(bytes/4),
        CMTimeMake((int64_t)(timestamp-recordEpoch),1000000000),NULL,&sample)) {
    [self.audioInput appendSampleBuffer:sample];CFRelease(sample);
   }
  }
  if(block)CFRelease(block);
 }
 pthread_mutex_unlock(&writerLock);
}

#pragma mark - Lifecycle

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender;return YES; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
 (void)sender;[self.watchdog invalidate];[self.meterTimer invalidate];
 if(atomic_load(&recording)) {
  // Finish the movie before exiting, otherwise the file is left unplayable.
  [self stopRecording:nil completion:^{
   self.closing=YES;[self stopCapture];
   if(self->audioQueue) { AudioQueueDispose(self->audioQueue,true);self->audioQueue=NULL; }
   [NSApp replyToApplicationShouldTerminate:YES];
  }];
  return NSTerminateLater;
 }
 self.closing=YES;[self stopCapture];
 if(audioQueue) { AudioQueueDispose(audioQueue,true);audioQueue=NULL; }return NSTerminateNow;
}
@end

/* Run by the OBS source when OBS starts, so the app updates even if nobody
 * opens it: no window, no Dock icon, one silent update cycle, then exit. The
 * update is checked against the same signed feed and key as the app's own
 * Check for Updates, so this adds no new way in. */
@interface HDBackgroundUpdater : NSObject <NSApplicationDelegate,SPUUpdaterDelegate>
@property SPUUpdater *updater;
@property BOOL installing;
@end
@implementation HDBackgroundUpdater
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
 (void)notification;
 NSBundle *bundle=NSBundle.mainBundle;
 id feed=bundle.infoDictionary[@"SUFeedURL"];
 // The window, if open, already runs its own updater; two at once would race.
 BOOL appOpen=NO;
 for(NSRunningApplication *other in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle.bundleIdentifier])
  if(other.processIdentifier!=getpid() && other.activationPolicy==NSApplicationActivationPolicyRegular) appOpen=YES;
 if(appOpen || ![feed isKindOfClass:NSString.class] || ![feed length]) { [NSApp terminate:nil];return; }
 SPUStandardUserDriver *driver=[[SPUStandardUserDriver alloc] initWithHostBundle:bundle delegate:nil];
 self.updater=[[SPUUpdater alloc] initWithHostBundle:bundle applicationBundle:bundle userDriver:driver delegate:self];
 NSError *error=nil;
 if(![self.updater startUpdater:&error]) { fprintf(stderr,"[HD60 S updater] %s\n",error.localizedDescription.UTF8String);[NSApp terminate:nil];return; }
 self.updater.automaticallyDownloadsUpdates=YES;
 [self.updater checkForUpdatesInBackground];
 // Nothing can be shown from here, so anything that waits on a person would
 // wait forever. Give the cycle a generous bound and leave.
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(300*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
  fprintf(stderr,"[HD60 S updater] gave up after 5 minutes\n");exit(0);
 });
}
- (BOOL)updater:(SPUUpdater *)updater willInstallUpdateOnQuit:(SUAppcastItem *)item immediateInstallationBlock:(void (^)(void))install {
 (void)updater;fprintf(stderr,"[HD60 S updater] installing %s\n",item.versionString.UTF8String);
 self.installing=YES;install();
 // Sparkle's installer replaces the bundle once this process has gone, and
 // nothing here would otherwise make it go.
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(2*NSEC_PER_SEC)),dispatch_get_main_queue(),^{ exit(0); });
 return YES;
}
// Nothing to relaunch: the OBS source picks up the new service by itself.
- (BOOL)updaterShouldRelaunchApplication:(SPUUpdater *)updater { (void)updater;return NO; }
- (void)updater:(SPUUpdater *)updater didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)check error:(NSError *)error {
 (void)updater;(void)check;
 if(error) fprintf(stderr,"[HD60 S updater] %s\n",error.localizedDescription.UTF8String);
 if(!self.installing) [NSApp terminate:nil];
}
- (void)updater:(SPUUpdater *)updater didAbortWithError:(NSError *)error {
 (void)updater;fprintf(stderr,"[HD60 S updater] aborted: %s\n",error.localizedDescription.UTF8String);[NSApp terminate:nil];
}
@end

int main(int argc,const char *argv[]) {
 @autoreleasepool {
  NSApplication *app=[NSApplication sharedApplication];
  BOOL background=argc>1 && !strcmp(argv[1],"--background-update");
  app.activationPolicy=background?NSApplicationActivationPolicyProhibited:NSApplicationActivationPolicyRegular;
  id delegate=background?[HDBackgroundUpdater new]:[CaptureApp new];
  app.delegate=delegate;[app run];
 }
 return 0;
}
