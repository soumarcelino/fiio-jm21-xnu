/* Capture the exception behind provider aborts; preserve stock termination. */
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static NSUncaughtExceptionHandler *previous_handler;

static void wallpaper_exception(NSException *exception)
{
    /* ExtensionKit launches can discard stderr. Use one bounded local log,
     * opened only on failure, without redirecting normal provider output. */
    const char *path = "/private/var/tmp/jm21-wallpaper-exception.log";
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        struct stat st;
        if (!fstat(fd, &st) && st.st_size > 131072) ftruncate(fd, 0);
        dprintf(fd, "provider=%s pid=%d exception=%s reason=%s\n%s\n",
                getprogname(), getpid(), exception.name.UTF8String,
                exception.reason.UTF8String ?: "(nil)",
                exception.callStackSymbols.description.UTF8String);
        fsync(fd);
        close(fd);
    }
    if (previous_handler && previous_handler != wallpaper_exception)
        previous_handler(exception);
}

__attribute__((constructor)) static void install_wallpaper_diagnostic(void)
{
    previous_handler = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(wallpaper_exception);
}
