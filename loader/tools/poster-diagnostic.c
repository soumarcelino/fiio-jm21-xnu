/* Opt-in diagnostics for PosterBoard, whose app launch can discard job stderr. */
#include <fcntl.h>
#include <dlfcn.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

/* Home previews use CARenderer with a Metal command queue on stock iOS.
 * JM21 exposes OpenGL ES, so render this UIKit preview into a bitmap instead. */
extern void UIGraphicsBeginImageContextWithOptions(CGSize, bool, CGFloat);
extern CGContextRef UIGraphicsGetCurrentContext(void);
extern id UIGraphicsGetImageFromCurrentImageContext(void);
extern void UIGraphicsEndImageContext(void);
static id (*original_image_for_view)(id, SEL, id);

static id image_for_view(id self, SEL command, id view)
{
    id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    static id (*create_device)(void);
    static int metal_loaded;
    if (!metal_loaded) {
        void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
        if (metal) create_device = dlsym(metal, "MTLCreateSystemDefaultDevice");
        metal_loaded = 1;
    }
    id device = create_device ? create_device() : nil;
    id queue = device ? send(device, sel_registerName("newCommandQueue")) : nil;
    if (queue) {
        send(queue, sel_registerName("release"));
        send(device, sel_registerName("release"));
        return original_image_for_view(self, command, view);
    }
    if (device) send(device, sel_registerName("release"));
    if (!view) return nil;
    CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(view, sel_registerName("bounds"));
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return nil;
    id screen = send(send(view, sel_registerName("window")), sel_registerName("screen"));
    if (!screen) screen = send((id)objc_getClass("UIScreen"), sel_registerName("mainScreen"));
    CGFloat scale = ((CGFloat (*)(id, SEL))objc_msgSend)(screen, sel_registerName("scale"));
    if (scale <= 0) scale = 1;
    UIGraphicsBeginImageContextWithOptions(bounds.size, false, scale);
    CGContextRef context = UIGraphicsGetCurrentContext();
    if (context) {
        CGContextTranslateCTM(context, -bounds.origin.x, -bounds.origin.y);
        id layer = send(view, sel_registerName("layer"));
        ((void (*)(id, SEL, CGContextRef))objc_msgSend)(layer, sel_registerName("renderInContext:"), context);
    }
    id image = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return image;
}

static void install_preview_fallback(void)
{
    dlopen("/System/Library/PrivateFrameworks/PosterBoard.framework/PosterBoard", RTLD_LAZY);
    Class cls = objc_getClass("PBFHomeScreenPreviewImageBuilder");
    Method method = class_getInstanceMethod(cls, sel_registerName("_imageForView:"));
    if (method) original_image_for_view = (void *)method_setImplementation(method, (IMP)image_for_view);
#ifdef JM21_POSTER_DIAG
    dprintf(STDERR_FILENO, "PosterBoard Home preview fallback installed=%d\n", method != NULL);
#endif
}

#ifdef JM21_POSTER_DIAG
typedef void (*ExceptionHandler)(id);
extern ExceptionHandler NSGetUncaughtExceptionHandler(void);
extern void NSSetUncaughtExceptionHandler(ExceptionHandler);
static ExceptionHandler previous_handler;

static void print_property(id exception, const char *property)
{
    id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
    id value = get(exception, sel_registerName(property));
    id description = get(value, sel_registerName("description"));
    const char *text = utf8(description, sel_registerName("UTF8String"));
    dprintf(STDERR_FILENO, "PosterBoard exception %s: %s\n", property,
            text ? text : "(nil)");
}

static void uncaught_exception(id exception)
{
    print_property(exception, "name");
    print_property(exception, "reason");
    print_property(exception, "callStackSymbols");
    fsync(STDERR_FILENO);
    if (previous_handler && previous_handler != uncaught_exception)
        previous_handler(exception);
}

#endif

__attribute__((constructor)) static void poster_diagnostic_init(void)
{
#ifdef JM21_POSTER_DIAG
    int fd = open("/private/var/tmp/jm21-PosterBoard-diagnostic.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        if (fd != STDERR_FILENO) {
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    }
    previous_handler = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(uncaught_exception);
    dprintf(STDERR_FILENO, "PosterBoard diagnostics installed pid=%d previous=%p\n",
            getpid(), (void *)previous_handler);
#endif
    install_preview_fallback();
}
