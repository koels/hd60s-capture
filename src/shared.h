/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "capture.h"
/* Consume the shared local service. Starts the sibling hd60s-service if needed. */
int hd_shared_capture(const hd_callbacks *callbacks,const atomic_bool *stop,double seconds);
/* Called once by the OBS source when OBS starts: runs the installed app's
 * silent updater, then brings this plugin up to the version the app now ships.
 * Returns at once; the work happens on its own thread. */
void hd_shared_update_in_background(void);
