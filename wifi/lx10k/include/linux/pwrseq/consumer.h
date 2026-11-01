#ifndef LX10K_PWRSEQ_H
#define LX10K_PWRSEQ_H
struct pwrseq_desc;
static inline struct pwrseq_desc *devm_pwrseq_get(void *d, const char *t) { return ERR_PTR(-ENODEV); }
static inline int pwrseq_power_on(struct pwrseq_desc *p) { return 0; }
static inline int pwrseq_power_off(struct pwrseq_desc *p) { return 0; }
#endif
