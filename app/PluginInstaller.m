/* SPDX-License-Identifier: GPL-2.0-only */
#import "PluginInstaller.h"

#define OBS_BUNDLE_ID @"com.obsproject.obs-studio"

static NSString *installedPath(void) {
 return [NSHomeDirectory() stringByAppendingPathComponent:
  @"Library/Application Support/obs-studio/plugins/hd60s-native.plugin"];
}
static NSString *bundledPath(void) {
 return [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"hd60s-native.plugin"];
}
static NSString *versionOf(NSString *bundle) {
 NSDictionary *plist=[NSDictionary dictionaryWithContentsOfFile:
  [bundle stringByAppendingPathComponent:@"Contents/Info.plist"]];
 id version=plist[@"CFBundleVersion"];
 return [version isKindOfClass:NSString.class]?version:nil;
}
/* The config folder counts: OBS installed anywhere but /Applications still has one. */
static BOOL obsPresent(void) {
 NSFileManager *files=NSFileManager.defaultManager;
 return [files fileExistsAtPath:@"/Applications/OBS.app"] ||
        [files fileExistsAtPath:[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/obs-studio"]];
}
static NSArray<NSRunningApplication *> *runningOBS(void) {
 return [NSRunningApplication runningApplicationsWithBundleIdentifier:OBS_BUNDLE_ID];
}
static void report(NSWindow *parent,NSString *message,NSString *detail,BOOL failure) {
 NSAlert *alert=[[NSAlert alloc] init];
 alert.messageText=message;alert.informativeText=detail;
 alert.alertStyle=failure?NSAlertStyleWarning:NSAlertStyleInformational;
 [alert addButtonWithTitle:@"OK"];
 if(parent) [alert beginSheetModalForWindow:parent completionHandler:nil]; else [alert runModal];
}
void HDSyncOBSPlugin(NSWindow *parent,BOOL announceNoop) {
 NSFileManager *files=NSFileManager.defaultManager;
 NSString *bundled=bundledPath(),*installed=installedPath();
 if(![files fileExistsAtPath:bundled] || !obsPresent()) {
  if(announceNoop) report(parent,@"Nothing to install",
   [files fileExistsAtPath:bundled]?@"OBS was not found on this Mac."
    :@"This build of the app has no OBS source bundled with it. Build it with make bundle.",NO);
  return;
 }
 NSString *shipped=versionOf(bundled),*present=versionOf(installed);
 BOOL already=[files fileExistsAtPath:installed];
 if(already && shipped && present && [shipped isEqualToString:present]) {
  if(announceNoop) report(parent,@"The OBS source is already current",
   [NSString stringWithFormat:@"OBS has version %@, the same one this app ships.",present],NO);
  return;
 }
 NSError *error=nil;
 if(![files createDirectoryAtPath:installed.stringByDeletingLastPathComponent
       withIntermediateDirectories:YES attributes:nil error:&error]) {
  report(parent,@"Could not create the plugins folder",error.localizedDescription,YES);return;
 }
 if(already && ![files removeItemAtPath:installed error:&error]) {
  report(parent,@"Could not replace the existing source",error.localizedDescription,YES);return;
 }
 if(![files copyItemAtPath:bundled toPath:installed error:&error]) {
  report(parent,@"Could not install the source",error.localizedDescription,YES);return;
 }
 // OBS is left alone. The driver lives in the service, which the new app has
 // already taken over, so OBS shows a short gap and carries on with the fix;
 // the copied file is only the thin client, which OBS reads at its next start.
 if(announceNoop) report(parent,already?@"OBS source updated":@"OBS source installed",
  runningOBS().count?@"OBS keeps capturing through the updated driver. The source file itself is picked up the next time OBS starts."
   :@"Open OBS, then choose Sources, Add, HD60 S Native (Revision 4).",NO);
}
