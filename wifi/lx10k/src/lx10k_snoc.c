/*
 * JM21: Linux's ath10k snoc.c, unmodified, compiled with the platform driver's registration replaced; the
 * JM21 probe (lx10k_bus.c) uses snoc.c's own functions.
 */
#define platform_driver lx10k_platform_driver
struct lx10k_platform_driver {
	int (*probe)(struct platform_device *);
	void (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	struct device_driver driver;
};
#define module_platform_driver(x) void *lx10k_snoc_driver_ref = &x
#include "snoc.c"
#include "lx10k_bus.c"
