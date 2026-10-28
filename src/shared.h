/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "capture.h"
/* Consume the shared local service. Starts the sibling hd60s-service if needed. */
int hd_shared_capture(const hd_callbacks *callbacks,const atomic_bool *stop,double seconds);
