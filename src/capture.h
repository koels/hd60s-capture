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
typedef struct {
 void (*video)(void *,const uint8_t *,const hd_mode *,uint64_t);
 void (*audio)(void *,const uint8_t *,size_t,unsigned,uint64_t);
 void (*message)(void *,const char *);
 void *context;
 void (*status)(void *,hd_status,const char *);
} hd_callbacks;
/* Blocking worker. Stop is set by caller; callbacks occur on worker thread. */
int hd_capture(const hd_callbacks *,const atomic_bool *stop,double seconds);
