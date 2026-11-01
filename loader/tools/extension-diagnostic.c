/* Read-only inspection of the definitions consumed by LaunchServices. */
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

static id send(id object, const char *name)
{
    SEL selector = sel_registerName(name);
    if (!object || !class_respondsToSelector(object_getClass(object), selector))
        return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, selector);
}

static const char *describe(id object)
{
    id description = send(object, "description");
    const char *text = (const char *)send(description, "UTF8String");
    return text ? text : "(nil)";
}

int main(void)
{
    dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
    id pool = send(send((id)objc_getClass("NSAutoreleasePool"), "alloc"), "init");
    void *framework = dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW);
    if (!framework) {
        fprintf(stderr, "ExtensionFoundation: %s\n", dlerror());
        return 1;
    }
    dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
    id enumerator = send((id)objc_getClass("EXEnumerator"), "extensionPointDefinitionEnumerator");
    printf("enumerator=%s\n", describe(enumerator));
    unsigned count = 0, null_urls = 0;
    id point;
    while (count < 4096 && (point = send(enumerator, "nextObject"))) {
        id url = send(point, "url");
        SEL platform = sel_registerName("platform");
        unsigned long value = class_respondsToSelector(object_getClass(point), platform)
            ? ((unsigned long (*)(id, SEL))objc_msgSend)(point, platform) : 0;
        const char *identifier = describe(send(point, "identifier"));
        if (!url || strcmp(identifier, "com.apple.posterkit.provider") == 0)
            printf("class=%s id=%s platform=%lu url=%s SDK=%s\n",
                   object_getClassName(point), identifier, value,
                   describe(url), describe(send(point, "SDKDictionary")));
        count++;
        if (!url) null_urls++;
    }
    printf("definitions=%u null_urls=%u\n", count, null_urls);
    send(pool, "drain");
    return enumerator ? 0 : 1;
}
