/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "capture.h"
#include <stdint.h>
/* BGRA bitmap, top-to-bottom. Caller owns returned allocation. */
uint8_t *hd_status_frame(unsigned width,unsigned height,hd_status status);
/* As above, with the wording replaced. The chain still reflects `status`, so
 * pass a status whose diagram is true for the state being described. */
uint8_t *hd_status_frame_text(unsigned width,unsigned height,hd_status status,
                              const char *title,const char *detail);
