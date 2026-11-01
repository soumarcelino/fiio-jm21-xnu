/* Capacity metadata for MobileGestalt NANDSize()/TotalDiskCapacity.
 * Adapted from txorav/m20lte-y2sios RedwoodStorage.cpp, commit
 * b19f54b2cfe57fed255daa3412b6c37e82543f1e. No controller or I/O code.
 * A plain IOService avoids publishing a second mountable block device.
 * Size is physical eMMC capacity; GPT reservations are not free Data space.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSNumber.h>

class HakiosMMCCapacity : public IOService {
    OSDeclareDefaultStructors(HakiosMMCCapacity);
};
OSDefineMetaClassAndStructors(HakiosMMCCapacity, IOService);

extern "C" void HakiosMMCReportCapacity(unsigned long long bytes);
void HakiosMMCReportCapacity(unsigned long long bytes)
{
    static HakiosMMCCapacity *published;
    IOService *platform = IOService::getPlatform();
    if (published || !bytes || !platform) return;
    HakiosMMCCapacity *service = OSTypeAlloc(HakiosMMCCapacity);
    if (!service) return;
    if (!service->init()) { service->release(); return; }
    OSNumber *size = OSNumber::withNumber(bytes, 64);
    if (!size) { service->release(); return; }
    service->setProperty("BSD Name", "disk0");
    service->setProperty("Size", size);
    size->release();
    if (!service->attach(platform)) { service->release(); return; }
    service->setName("HakiosMMCCapacity");
    if (!service->start(platform)) {
        service->detach(platform); service->release(); return;
    }
    service->registerService();
    published = service;
    IOLog("mmc: disk0 capacity metadata %llu bytes\n", bytes);
}
