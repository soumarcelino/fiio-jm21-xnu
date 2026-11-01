/* The JM21's eMMC as /dev/rmmc0 and /dev/mmcroot (bsd/dev/hakios.mmc.c). */
#ifndef _SYS_HAKIOS_MMC_H_
#define _SYS_HAKIOS_MMC_H_
#include <sys/types.h>
void hakios_mmc_init(void);
bool hakios_mmc_dev(dev_t dev);
void hakios_jm21_init(void);      /* bsd/dev/hakios.jm21.c: the loader's USB mailbox and log */
#endif
