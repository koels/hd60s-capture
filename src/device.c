/* SPDX-License-Identifier: GPL-2.0-only
 * Whether an HD60 S is plugged in, without the driver. The app asks this to
 * show a disconnected state; only the service links capture.c and touches USB.
 */
#include "capture.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
int hd_device_find(int *speed) {
 if(speed) *speed=-1;
 io_iterator_t it=0;
 if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&it)) return -1;
 int result=0;io_service_t service;
 while((service=IOIteratorNext(it))) {
  CFTypeRef v=IORegistryEntryCreateCFProperty(service,CFSTR("idVendor"),NULL,0);
  CFTypeRef p=IORegistryEntryCreateCFProperty(service,CFSTR("idProduct"),NULL,0);
  CFTypeRef sp=IORegistryEntryCreateCFProperty(service,CFSTR("Device Speed"),NULL,0);
  int vid=0,pid=0,link=-1;
  if(v && CFGetTypeID(v)==CFNumberGetTypeID()) CFNumberGetValue(v,kCFNumberIntType,&vid);
  if(p && CFGetTypeID(p)==CFNumberGetTypeID()) CFNumberGetValue(p,kCFNumberIntType,&pid);
  if(sp && CFGetTypeID(sp)==CFNumberGetTypeID()) CFNumberGetValue(sp,kCFNumberIntType,&link);
  if(v) CFRelease(v);if(p) CFRelease(p);if(sp) CFRelease(sp);IOObjectRelease(service);
  if(vid==0x0fd9 && (pid==0x76 || pid==0x74 || pid==0x5e || pid==0x4f)) { result=pid;if(speed) *speed=link;break; }
 }
 IOObjectRelease(it);return result;
}
int hd_device_product(void) { return hd_device_find(NULL); }
