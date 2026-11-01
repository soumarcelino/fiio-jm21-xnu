#ifndef LX10K_SMEM_H
#define LX10K_SMEM_H
#define QCOM_SMEM_HOST_ANY -1
static inline void *qcom_smem_get(unsigned host, unsigned item, size_t *size) { return ERR_PTR(-ENOENT); }
#endif
