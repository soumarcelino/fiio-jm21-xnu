/* Opt-in, bounded diagnostics; preserves the stock enumeration and callback. */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <execinfo.h>

static void (*enumerate_original)(id, SEL, NSUInteger, long long, id);
static _Atomic unsigned calls;
static mach_timebase_info_data_t timebase;

static void enumerate_frames(id self, SEL selector, NSUInteger options,
                             long long offset, id callback)
{
    unsigned call = atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed) + 1;
    if (call > 16 && (call % 32768 != 0 || call / 32768 > 8)) {
        enumerate_original(self, selector, options, offset, callback);
        return;
    }
    @autoreleasepool {
        NSString *path = ((id (*)(id, SEL))objc_msgSend)(self,
                                                      @selector(segmentPath));
        unsigned long long bytes = ((unsigned long long (*)(id, SEL))objc_msgSend)(
            self, @selector(bytesUsed));
        uint64_t start = mach_absolute_time();
        fprintf(stderr, "JM21 Biome enum begin call=%u store=%p path=%s options=0x%lx offset=%lld bytes=%llu caller=%p\n",
                call, self, path.UTF8String ?: "(nil)", (unsigned long)options,
                offset, bytes, __builtin_return_address(0));
        fflush(stderr);
        if (call % 32768 == 0) {
            void *frames[24];
            int count = backtrace(frames, 24);
            fprintf(stderr, "JM21 Biome enum caller stack call=%u:", call);
            for (int i = 0; i < count; i++) fprintf(stderr, " %p", frames[i]);
            fprintf(stderr, "\n");
        }
        @try {
            enumerate_original(self, selector, options, offset, callback);
        } @finally {
            double ms = (double)(mach_absolute_time() - start) * timebase.numer /
                        timebase.denom / 1000000.0;
            fprintf(stderr, "JM21 Biome enum end call=%u elapsed_ms=%.3f\n", call, ms);
            fflush(stderr);
        }
    }
}

__attribute__((constructor)) static void install_frame_diagnostic(void)
{
    const char *enabled = getenv("JM21_BIOME_ENUM_DIAG");
    if (!enabled || strcmp(enabled, "1")) return;
    @autoreleasepool {
        dlopen("/System/Library/PrivateFrameworks/BiomeStorage.framework/BiomeStorage",
               RTLD_LAZY);
        Class cls = objc_getClass("BMFrameStore");
        Method method = class_getInstanceMethod(cls,
            @selector(enumerateWithOptions:fromOffset:usingBlock:));
        char result[8] = {0};
        if (method) method_getReturnType(method, result, sizeof(result));
        if (!method || method_getNumberOfArguments(method) != 5 || strcmp(result, "v") ||
            !class_getInstanceMethod(cls, @selector(segmentPath)) ||
            !class_getInstanceMethod(cls, @selector(bytesUsed))) {
            fprintf(stderr, "JM21 Biome enum diagnostic: incompatible method\n");
            return;
        }
        mach_timebase_info(&timebase);
        enumerate_original = (void *)method_setImplementation(method, (IMP)enumerate_frames);
        fprintf(stderr, "JM21 Biome enum diagnostic installed (first 16, then 8 samples every 32768 calls)\n");
    }
}
