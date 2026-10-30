/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
typedef struct {
 unsigned width,height,rate,audio_rate; int fractional,bt709;
} hd_mode;
typedef enum {
 HD_DISCONNECTED, HD_INITIALIZING, HD_NO_SIGNAL, HD_LIVE, HD_BUSY, HD_ERROR
} hd_status;
/* Returns 0 when absent, a supported/known product ID when attached, -1 on error. */
int hd_device_product(void);
/* Same, and fills *speed with the card's negotiated USB link speed: the
 * IOUSBHostDevice "Device Speed" value, where 3 is SuperSpeed (USB 3) and
 * anything lower is too slow to carry the stream. Not "USBSpeed": that reads 3
 * on a USB 2.0 cable. -1 when unknown or absent. */
int hd_device_find(int *speed);
#define HD_SPEED_SUPER 3
typedef struct {
 void (*video)(void *,const uint8_t *,const hd_mode *,uint64_t);
 void (*audio)(void *,const uint8_t *,size_t,unsigned,uint64_t);
 void (*message)(void *,const char *);
 void *context;
 void (*status)(void *,hd_status,const char *);
 /* Source scan type, delivered immediately before each video callback.
  * 0 unknown (older service), 1 progressive, 2 interlaced/bob output. */
 void (*scan)(void *,unsigned);
} hd_callbacks;
/* Blocking worker. Stop is set by caller; callbacks occur on worker thread. */
int hd_capture(const hd_callbacks *,const atomic_bool *stop,double seconds);
