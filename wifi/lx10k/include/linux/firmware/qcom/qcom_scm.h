#ifndef LX10K_QCOM_SCM_H
#define LX10K_QCOM_SCM_H
/* MSA permissions are already in place on the JM21 (hyp assign answers -2; the firmware goes on): the
 * JM21 bus sets qcom,msa-fixed-perm, so these are never needed. */
#define QCOM_SCM_VMID_HLOS 0x3
#define QCOM_SCM_VMID_MSS_MSA 0xF
#define QCOM_SCM_VMID_WLAN 0x18
#define QCOM_SCM_VMID_WLAN_CE 0x19
#define QCOM_SCM_PERM_READ 0x4
#define QCOM_SCM_PERM_WRITE 0x2
#define QCOM_SCM_PERM_EXEC 0x1
#define QCOM_SCM_PERM_RW (QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE)
#define QCOM_SCM_PERM_RWX (QCOM_SCM_PERM_RW | QCOM_SCM_PERM_EXEC)
struct qcom_scm_vmperm { int vmid; int perm; };
static inline int qcom_scm_assign_mem(phys_addr_t a, size_t s, u64 *src, const struct qcom_scm_vmperm *n, unsigned int c) { return 0; }
#endif
