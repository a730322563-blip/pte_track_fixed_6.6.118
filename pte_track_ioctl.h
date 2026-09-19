// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _PTE_TRACK_IOCTL_H_
#define _PTE_TRACK_IOCTL_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define PTE_TRACK_MAGIC 'P'

/* 挂钩: 对目标页置 UXN, 命中时写 V3/V4/V5 */
struct pte_track_hook_req {
    uint64_t orig_page_addr;   /* 页基址(4K 对齐) */
    uint64_t v3, v4, v5;       /* 命中时写入 V3/V4/V5 的值 */
};
#define PTE_TRACK_IOCTL_HOOK _IOW(PTE_TRACK_MAGIC, 1, struct pte_track_hook_req)

/* 重新武装 UXN(命中后 PTE 已被回调恢复), 可同时更新 V3/V4/V5 */
struct pte_track_arm_req {
    uint64_t orig_page_addr;
    uint32_t set_v;            /* 1 = 同时更新 V3/V4/V5 */
    uint32_t _pad;
    uint64_t v3, v4, v5;
};
#define PTE_TRACK_IOCTL_ARM _IOW(PTE_TRACK_MAGIC, 2, struct pte_track_arm_req)

/* 解除挂钩并恢复 PTE */
struct pte_track_unhook_req {
    uint64_t orig_page_addr;
};
#define PTE_TRACK_IOCTL_UNHOOK _IOW(PTE_TRACK_MAGIC, 3, struct pte_track_unhook_req)

#endif /* _PTE_TRACK_IOCTL_H_ */
