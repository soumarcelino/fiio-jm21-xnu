/* JM21: what ath10k (snoc) needs beyond what the ath11k port's LinuxKPI layer offers. Stubs where the
 * JM21 bus (wifi/lx10k/src) does the job itself. */
#ifndef LX10K_PRELUDE_H
#define LX10K_PRELUDE_H
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/of.h>
DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t, spin_lock_bh(_T->lock), spin_unlock_bh(_T->lock))
struct nvmem_cell;
static inline struct nvmem_cell *devm_nvmem_cell_get(void *d, const char *n) { return (struct nvmem_cell *)ERR_PTR(-ENOENT); }
static inline void *nvmem_cell_read(struct nvmem_cell *c, size_t *l) { return ERR_PTR(-ENOENT); }
#ifndef of_property_read_u8_array
static inline int of_property_read_u8_array(const struct device_node *n, const char *p, u8 *v, size_t c) { return -EINVAL; }
static inline int of_property_read_u8(const struct device_node *n, const char *p, u8 *v) { return -EINVAL; }
#endif
#endif
#ifndef of_property_read_string_index
static inline int lx10k_of_string_index(const struct device_node *n, const char *p, int i, const char **o) { return -EINVAL; }
#define of_property_read_string_index lx10k_of_string_index
#endif
