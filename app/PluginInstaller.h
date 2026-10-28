/* SPDX-License-Identifier: GPL-2.0-only */
#import <Cocoa/Cocoa.h>
/* Brings the OBS source in the user's own plugins folder up to the version this
 * app ships. OBS is never quit: it reaches the updated driver through the service. Does nothing when OBS is absent,
 * when no plugin is bundled, or when the installed copy is already current.
 * announceNoop reports that last case instead of staying quiet, for the button
 * in Settings, where silence would look like a dead control. */
void HDSyncOBSPlugin(NSWindow *parent,BOOL announceNoop);
