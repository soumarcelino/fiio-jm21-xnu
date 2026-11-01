#ifndef LX10K_QCOM_RPROC_H
#define LX10K_QCOM_RPROC_H
struct notifier_block;
struct qcom_ssr_notify_data { const char *name; bool crashed; };
enum { QCOM_SSR_BEFORE_POWERUP, QCOM_SSR_AFTER_POWERUP, QCOM_SSR_BEFORE_SHUTDOWN, QCOM_SSR_AFTER_SHUTDOWN };
static inline void *qcom_register_ssr_notifier(const char *n, struct notifier_block *nb) { return (void *)1; }
static inline int qcom_unregister_ssr_notifier(void *h, struct notifier_block *nb) { return 0; }
#endif
