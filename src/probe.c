#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>

/* Original diagnostic: only standard descriptor queries; no vendor commands. */
int main(void) {
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    int vendor = 0x0fd9;
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "USB enumeration failed: 0x%x\n", kr); return 1; }
    unsigned found = 0, inspected = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        IOCFPlugInInterface **plugin = NULL;
        IOUSBDeviceInterface **device = NULL;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                              kIOCFPlugInInterfaceID, &plugin, &score);
        IOObjectRelease(service);
        if (kr != KERN_SUCCESS || !plugin) { fprintf(stderr, "USB plugin unavailable: 0x%x\n", kr); continue; }
        HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (void **)&device);
        (*plugin)->Release(plugin);
        if (hr || !device) { fprintf(stderr, "USB interface unavailable: 0x%x\n", (unsigned)hr); continue; }
        UInt16 actual_vendor = 0, pid = 0;
        (*device)->GetDeviceVendor(device, &actual_vendor);
        kr = (*device)->GetDeviceProduct(device, &pid);
        if (kr != KERN_SUCCESS || actual_vendor != vendor || (pid != 0x004f && pid != 0x005e && pid != 0x0074 && pid != 0x0076)) {
            (*device)->Release(device); continue;
        }
        ++found;
        printf("HD60 S %04x:%04x (%s)\n", vendor, pid, pid == 0x0076 ? "revision 4" : "revision 1–3");
        kr = (*device)->USBDeviceOpen(device);
        printf("User-space device open: 0x%08x\n", kr);
        if (kr != kIOReturnSuccess) { (*device)->Release(device); continue; }
        UInt8 configurations = 0;
        kr = (*device)->GetNumberOfConfigurations(device, &configurations);
        int valid = kr == kIOReturnSuccess && configurations > 0;
        for (UInt8 c = 0; c < configurations && valid; ++c) {
            IOUSBConfigurationDescriptorPtr config = NULL;
            kr = (*device)->GetConfigurationDescriptorPtr(device, c, &config);
            if (kr != kIOReturnSuccess || !config) { fprintf(stderr, "Descriptor read failed: 0x%x\n", kr); valid = 0; break; }
            const unsigned char *bytes = (const unsigned char *)config;
            unsigned total = bytes[2] | ((unsigned)bytes[3] << 8);
            printf("Configuration %u: %u bytes\n", c, total);
            for (unsigned off = 0; off < total;) {
                if (total - off < 2 || bytes[off] < 2 || bytes[off] > total - off) {
                    fprintf(stderr, "Malformed descriptor at %u\n", off); valid = 0; break;
                }
                unsigned len = bytes[off], type = bytes[off + 1];
                const unsigned char *d = bytes + off;
                if (type == 4 && len >= 9)
                    printf("  Interface %u alt %u: class %02x, %u endpoints\n", d[2], d[3], d[5], d[4]);
                if (type == 5 && len >= 7)
                    printf("    Endpoint 0x%02x: type %u, max packet %u, interval %u\n", d[2], d[3] & 3, d[4] | (d[5] << 8), d[6]);
                if (type == 48 && len >= 6)
                    printf("      SuperSpeed: burst %u, attributes 0x%02x, bytes/interval %u\n", d[2], d[3], d[4] | (d[5] << 8));
                off += len;
            }
        }
        (*device)->USBDeviceClose(device);
        (*device)->Release(device);
        if (valid) ++inspected;
    }
    IOObjectRelease(iter);
    if (!found) fprintf(stderr, "No supported HD60 S found.\n");
    return inspected ? 0 : 1;
}
