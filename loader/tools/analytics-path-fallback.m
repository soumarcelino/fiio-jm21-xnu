/* OSAnalytics can return nil when its system-container directory cannot be
 * prepared on this port. analyticsd passes that path to std::string without
 * checking it. Keep the normal path when available; provide durable storage
 * only for the missing CoreAnalytics tasking directory. */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>

static id (*original_path)(id, SEL);
static NSString *tasking_path(id self, SEL selector)
{
    NSString *path = original_path(self, selector);
    if (path) return path;
    static NSString *fallback;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *directory = @"/private/var/db/analyticsd/Library/CoreAnalytics";
        NSError *error = nil;
        if ([[NSFileManager defaultManager] createDirectoryAtPath:directory
                withIntermediateDirectories:YES
                attributes:@{NSFilePosixPermissions: @0700} error:&error]) {
            fallback = [directory retain];
            fprintf(stderr, "analyticsd: missing system-container tasking path; using %s\n",
                    [directory fileSystemRepresentation]);
        } else
            fprintf(stderr, "analyticsd tasking directory: %s\n", [[error description] UTF8String]);
    });
    return fallback;
}

__attribute__((constructor)) static void install_tasking_path(void)
{
    dlopen("/System/Library/PrivateFrameworks/OSAnalytics.framework/OSAnalytics", RTLD_LAZY);
    Class cls = objc_getClass("OSASystemConfiguration");
    Method method = cls ? class_getInstanceMethod(cls, sel_registerName("pathCATasking")) : NULL;
    if (method) {
        original_path = (void *)method_getImplementation(method);
        method_setImplementation(method, (IMP)tasking_path);
    }
}
