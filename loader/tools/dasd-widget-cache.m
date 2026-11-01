/* JM21: discard modulation catch-up across the invalid boot epoch.
 * The stock caller persists its current date after the single modulation. */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static int (*count_original)(id,SEL,id,id);
static int enabled;
static unsigned logged;
static int modulation_count(id self,SEL selector,id from,id to)
{
    int count=count_original(self,selector,from,to);
    double start=[from timeIntervalSince1970],end=[to timeIntervalSince1970];
    /* 2020-01-01 UTC: distinguish the known 1970 boot from real dates. */
    if(from && to && isfinite(start) && isfinite(end) && start<1577836800.0 && end>=1577836800.0 && count>1) {
        if(__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)==0) {
            fprintf(stderr,"JM21 DAS epoch catchup from=%.3f to=%.3f original=%d corrected=%d\n",start,end,count,enabled?1:count);
            fflush(stderr);
        }
        if(enabled) return 1;
    }
    return count;
}
__attribute__((constructor)) static void install(void)
{
    const char *env=getenv("JM21_DAS_WIDGET_CACHE");
    if(!env || (strcmp(env,"1") && strcmp(env,"diag"))) return;
    enabled=!strcmp(env,"1");
    @autoreleasepool {
        Method method=class_getInstanceMethod(objc_getClass("_DASBudgetModulator"),sel_registerName("numberOfModulationFromDate:toDate:"));
        char type[8]={0}; if(method) method_getReturnType(method,type,sizeof(type));
        if(!method || method_getNumberOfArguments(method)!=4 || strcmp(type,"i")) {
            fprintf(stderr,"JM21 DAS epoch catchup incompatible method return=%s\n",type); return;
        }
        count_original=(void*)method_setImplementation(method,(IMP)modulation_count);
        fprintf(stderr,"JM21 DAS epoch catchup installed enabled=%d\n",enabled);
    }
}
