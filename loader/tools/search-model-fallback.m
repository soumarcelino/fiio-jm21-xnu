/* The reduced system image omits Spotlight's canned CoreML models.  Stock
 * SSCoreMLInterface constructs NSURL(nil) before it can report a load error.
 * Preserve the initializer's ordinary NSError/nil failure contract instead. */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static id (*original_init_model)(id, SEL, NSString *, NSError **);
static NSUncaughtExceptionHandler *previous_exception_handler;

static void spotlight_exception(NSException *exception)
{
    fprintf(stderr, "Spotlight exception %s: %s\n%s\n",
            [[exception name] UTF8String], [[exception reason] UTF8String],
            [[[exception callStackSymbols] description] UTF8String]);
    fsync(STDERR_FILENO);
    if (previous_exception_handler && previous_exception_handler != spotlight_exception)
        previous_exception_handler(exception);
}

static id init_model(id self, SEL selector, NSString *name, NSError **error)
{
    NSBundle *bundle = [NSBundle bundleForClass:[self class]];
    if (name && [bundle pathForResource:name ofType:@"mlmodelc"])
        return original_init_model(self, selector, name, error);

    if (error) {
        *error = [NSError errorWithDomain:NSCocoaErrorDomain
                                    code:NSFileNoSuchFileError
                                userInfo:@{NSLocalizedDescriptionKey:
                                    @"Spotlight ranking model is absent from this system image",
                                    NSFilePathErrorKey: name ?: @"(missing model name)"}];
    }
    /* Match an init-family failure: consume the allocated receiver. */
    [self release];
    return nil;
}

__attribute__((constructor)) static void install_search_model_fallback(void)
{
    @autoreleasepool {
        /* App launches may replace the launchd job's stderr. Keep only this
         * failing app's exception diagnostic reachable through the USB shell. */
        if (getprogname() && strcmp(getprogname(), "Spotlight") == 0) {
            int fd = open("/private/var/tmp/jm21-Spotlight.log",
                          O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                if (fd != STDERR_FILENO) {
                    dup2(fd, STDERR_FILENO);
                    close(fd);
                }
                previous_exception_handler = NSGetUncaughtExceptionHandler();
                NSSetUncaughtExceptionHandler(spotlight_exception);
            }
        }
        dlopen("/System/Library/PrivateFrameworks/SpotlightServices.framework/SpotlightServices",
               RTLD_LAZY);
        Class cls = objc_getClass("SSCoreMLInterface");
        Method method = class_getInstanceMethod(cls,
                          sel_registerName("initSpecificModel:error:"));
        if (method)
            original_init_model = (void *)method_setImplementation(method, (IMP)init_model);
        fprintf(stderr, "JM21 Spotlight missing-model fallback installed=%d\n", method != NULL);
    }
}
