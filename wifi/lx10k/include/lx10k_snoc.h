/* JM21: the platform glue snoc.c names. The JM21 bus (wifi/lx10k/src/lx10k_bus.c) does its own probe, so
 * these are reached only through snoc.c's power/resource helpers, which it does not call; each says so. */
#ifndef LX10K_SNOC_H
#define LX10K_SNOC_H
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/iommu.h>
#define IOMMU_READ 1
#define IOMMU_WRITE 2
void lx10k_unexpected(const char *what);
static inline int lx10k_dev_u32(void *d, const char *p, u32 *v) { return -EINVAL; }
#define device_property_read_u32(d, p, v) lx10k_dev_u32((void *)(d), p, v)
static inline void *devm_memremap(void *d, resource_size_t o, size_t s, unsigned long f) { lx10k_unexpected("devm_memremap"); return ERR_PTR(-ENODEV); }
static inline struct iommu_domain *iommu_paging_domain_alloc(void *d) { return ERR_PTR(-ENODEV); }
static inline int iommu_attach_device(struct iommu_domain *a, void *d) { return -ENODEV; }
static inline void iommu_detach_device(struct iommu_domain *a, void *d) {}
static inline void iommu_domain_free(struct iommu_domain *a) {}
static inline int iommu_map(struct iommu_domain *a, unsigned long i, phys_addr_t p, size_t s, int pr, gfp_t g) { return -ENODEV; }
static inline size_t iommu_unmap(struct iommu_domain *a, unsigned long i, size_t s) { return 0; }
static inline void netif_threaded_enable(void *n) {}
static inline int of_dma_configure(void *d, void *n, bool f) { return 0; }
#define MEMREMAP_WT 2
struct platform_device_info { void *parent; void *fwnode; const char *name; int id; void *res; unsigned num_res; const void *data; size_t size_data; u64 dma_mask; void *properties; };
static inline int platform_get_irq(struct platform_device *p, unsigned i) { lx10k_unexpected("platform_get_irq"); return -ENXIO; }
static inline struct resource *platform_get_resource_byname(struct platform_device *p, unsigned t, const char *n) { return NULL; }
static inline struct platform_device *platform_device_register_full(const struct platform_device_info *i) { return ERR_PTR(-ENODEV); }
struct regulator_bulk_data { const char *supply; void *consumer; int ret; };
static inline int regulator_bulk_enable(int n, struct regulator_bulk_data *r) { return 0; }
static inline int regulator_bulk_disable(int n, struct regulator_bulk_data *r) { return 0; }
static inline int devm_regulator_bulk_get(void *d, int n, struct regulator_bulk_data *r) { return 0; }
#ifndef IRQF_NO_AUTOEN
#define IRQF_NO_AUTOEN 0
#endif
#define device_get_match_data(d) ((const void *)NULL)
#define platform_set_drvdata(p, v) dev_set_drvdata(&(p)->dev, (v))
#define platform_get_drvdata(p) dev_get_drvdata(&(p)->dev)
#endif
