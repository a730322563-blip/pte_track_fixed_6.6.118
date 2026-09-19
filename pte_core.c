// SPDX-License-Identifier: GPL-2.0
/*
 * pte_core.c - PTE UXN-based code page hook (do_mem_abort inline hook)
 *
 * 6.6 适配（目标：6.6.118-android15 GKI，设备实测内核
 * 6.6.118-android15-8-g93e223c276e7-abogki-4k；同时兼容 5.10 系列）：
 *   1) pte_offset_map() 在 6.6 是引用未导出符号 __pte_offset_map 的 inline 包装，
 *      改用 pte_offset_kernel()（等价 5.10 的 pte_offset_map 宏，纯内联，无外部符号）；
 *   2) flush_tlb_page() 在 6.6 的展开链会引用未导出的
 *      __mmu_notifier_arch_invalidate_secondary_tlbs，改用本文件自包含的
 *      flush_tlb_page_direct()（不进入 mmu_notifier 链）；
 *   3) ESR IABT 判定修正为 ESR_ELx_EC(esr)（原掩码比较写法恒真）；
 *   4) LDR literal 重定位掩码修正（原第二条件与掩码矛盾、恒假，编译告警）；
 *      并修正 B/BL 分支与 LDR literal 分支的符号位/位偏移错误（潜在错误）；

 *
 * 修复版（基于 0902 重构版，解决卡死重启）：
 *   1) patch / unpatch do_mem_abort 全部包在 stop_machine 里执行，
 *      杜绝"别的 CPU 正在跑 do_mem_abort，读到半写跳板"导致的跑飞；
 *   2) 写入顺序修正：先写跳板目标地址(p[2..3])，再写 ldr x17(p[0])，
 *      最后写 br x17(p[1])，消除"br 到垃圾地址"的窗口；
 *   3) 保留 untagged_addr、spin_lock_irqsave、手动 icache flush 等加固；
 *   4) stop_machine 符号解析失败则拒绝加载（宁可拒绝，不要带病重启设备）。
 *
 * 原理：
 *   对目标用户虚拟页 PTE 置 UXN(bit54) -> 用户态执行该页触发 IABT@EL0
 *   -> do_mem_abort 入口被 16B inline hook 接管。
 *   命中：清 UXN 恢复可执行 + 写 V3/V4/V5 到 thread.uw.fpsimd_state，直接 ret。
 *   未命中：恢复现场，跳 trampoline 执行被搬走的 16 字节原指令后回到 do_mem_abort+16。
 *
 * 加载：
 *   insmod pte_track.ko kln_addr=0x$(grep -w kallsyms_lookup_name /proc/kallsyms | awk '{print $1}')
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/version.h>
#include <asm/pgtable.h>
#include <asm/ptrace.h>
#include <asm/processor.h>
#include <asm/esr.h>
#include <asm/tlbflush.h>
#include <asm/barrier.h>
#include <asm/thread_info.h>
#include <asm/memory.h>
#include <asm/cacheflush.h>
#include "pte_track_ioctl.h"

#define MAX_HOOKS     64
#define PTE_UXN_BIT   (1UL << 54)

/* 跳板指令（固定编码） */
#define INSN_LDR_X17_8  0x58000051UL  /* ldr x17, [pc, #8] */
#define INSN_BR_X17     0xd61f0220UL  /* br  x17           */

static unsigned long kln_addr_param;
module_param_named(kln_addr, kln_addr_param, ulong, 0444);
MODULE_PARM_DESC(kln_addr, "runtime address of kallsyms_lookup_name");

typedef unsigned long (*kln_t)(const char *);
typedef int  (*set_mem_t)(unsigned long addr, int numpages);
typedef void (*fpsimd_save_t)(struct user_fpsimd_state *);
typedef void (*fpsimd_restore_t)(void);
/* stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpu) */
typedef int  (*stop_machine_t)(int (*fn)(void *), void *, const struct cpumask *);

static kln_t kln;
static unsigned long do_mem_abort_addr;
static set_mem_t set_memory_rw_fn, set_memory_ro_fn;
static fpsimd_save_t fpsimd_save_state_fn;
static fpsimd_restore_t fpsimd_restore_current_state_fn;
static stop_machine_t stop_machine_fn;

struct hook_entry {
    struct mm_struct *mm;
    unsigned long page;
    u64 v3, v4, v5;
};
static struct hook_entry hooks[MAX_HOOKS];
static DEFINE_SPINLOCK(hooks_lock);
static u32 orig_insns[4];

extern void pte_stub_entry(void);
extern unsigned long pte_trampoline_area;
extern unsigned long pte_tramp_slot;

/* 64 行粒度的手动 icache 刷新（dc cvau / ic ivau），比通用 flush_icache_range 更可控 */
static void flush_icache_asm(unsigned long start, unsigned long end)
{
    unsigned long addr;
    start &= ~63UL;
    for (addr = start; addr < end; addr += 64)
        asm volatile("dc cvau, %0" :: "r"(addr));
    asm volatile("dsb ish" ::: "memory");
    for (addr = start; addr < end; addr += 64)
        asm volatile("ic ivau, %0" :: "r"(addr));
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb" ::: "memory");
}

/* ===================== TLB flush（6.6 兼容自包含实现） ===================== */
/*
 * 6.6 的 flush_tlb_page() → flush_tlb_page_nosync() → __flush_tlb_page_nosync()
 * 内嵌了 mmu_notifier_arch_invalidate_secondary_tlbs()，展开后引用未导出符号
 * __mmu_notifier_arch_invalidate_secondary_tlbs（modpost 报 undefined、无法 insmod）。
 * 这里按 arm64 内核同等语义手工执行 TLBI 序列（本驱动不涉及 SMMU 共享页表，
 * 不需要通知 secondary TLB）。
 */
static void flush_tlb_page_direct(struct mm_struct *mm, unsigned long uaddr)
{
    unsigned long addr = __TLBI_VADDR(uaddr, ASID(mm));

    dsb(ishst);
    __tlbi(vale1is, addr);
    __tlbi_user(vale1is, addr);
    dsb(ish);
    isb();
}

/* ===================== 页表操作 ===================== */

static pte_t *find_pte(struct mm_struct *mm, unsigned long addr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;

    pgd = pgd_offset(mm, addr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return NULL;
    p4d = p4d_offset(pgd, addr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return NULL;
    pud = pud_offset(p4d, addr);
    if (pud_none(*pud) || pud_bad(*pud))
        return NULL;
    pmd = pmd_offset(pud, addr);
    if (pmd_none(*pmd) || pmd_bad(*pmd))
        return NULL;
    if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
        return NULL;  /* 大页暂不支持 */
    /* 6.6: pte_offset_map() 引用未导出的 __pte_offset_map；用内联展开的 pte_offset_kernel() */
    return pte_offset_kernel(pmd, addr);
}

/* uxn=true 置 UXN（武装），false 清 UXN（恢复可执行） */
static void set_pte_uxn(struct mm_struct *mm, unsigned long addr, bool uxn)
{
    pte_t *ptep;
    struct vm_area_struct *vma;
    pte_t pte;
    unsigned long page = addr & PAGE_MASK;

    mmap_read_lock(mm);
    vma = find_vma(mm, page);
    ptep = find_pte(mm, page);
    if (!vma || vma->vm_start > page || !ptep) {
        mmap_read_unlock(mm);
        return;
    }
    pte = *ptep;
    if (uxn)
        pte_val(pte) |= PTE_UXN_BIT;
    else
        pte_val(pte) &= ~PTE_UXN_BIT;
    WRITE_ONCE(*ptep, pte);
    flush_tlb_page_direct(mm, page);   /* 6.6: 替代 flush_tlb_page()，绕开 mmu_notifier 未导出符号 */
    mmap_read_unlock(mm);
}

/* ===================== V3/V4/V5 写入 ===================== */

static void write_vregs(struct hook_entry *h)
{
    struct user_fpsimd_state *fps = &current->thread.uw.fpsimd_state;

    if (test_thread_flag(TIF_SVE))
        return;  /* SVE 布局不同，游戏一般走 NEON */
    if (!test_thread_flag(TIF_FOREIGN_FPSTATE) && fpsimd_save_state_fn)
        fpsimd_save_state_fn(fps);
    fps->vregs[3] = (__uint128_t)h->v3;
    fps->vregs[4] = (__uint128_t)h->v4;
    fps->vregs[5] = (__uint128_t)h->v5;
    set_thread_flag(TIF_FOREIGN_FPSTATE);
    if (fpsimd_restore_current_state_fn)
        fpsimd_restore_current_state_fn();
}

/* ===================== do_mem_abort 命中处理 ===================== */

int pte_handle_abort(unsigned long far, unsigned int esr, struct pt_regs *regs)
{
    struct hook_entry *h = NULL;
    unsigned long page;
    unsigned long flags;
    int i;

    /* 修正：ESR_ELx_EC_IABT_LOW 是移位后的 EC 值，必须用 ESR_ELx_EC() 取 EC 字段比较
     *（原写法 (esr & ESR_ELx_EC_MASK) != ESR_ELx_EC_IABT_LOW 恒真） */
    if (ESR_ELx_EC(esr) != ESR_ELx_EC_IABT_LOW)
        return 0;
    if (!user_mode(regs))
        return 0;

    page = untagged_addr(far) & PAGE_MASK;

    spin_lock_irqsave(&hooks_lock, flags);
    for (i = 0; i < MAX_HOOKS; i++) {
        if (hooks[i].mm && hooks[i].mm == current->mm && hooks[i].page == page) {
            h = &hooks[i];
            break;
        }
    }
    spin_unlock_irqrestore(&hooks_lock, flags);
    if (!h)
        return 0;

    /* 命中：恢复该页可执行，再写 V3/V4/V5，直接返回（不产信号） */
    set_pte_uxn(current->mm, page, false);
    write_vregs(h);
    return 1;
}

/* ===================== arm64 PC-relative 重定位 ===================== */

static int reloc_pc_relative(u32 *insn, unsigned long orig_pc, unsigned long new_pc)
{
    u32 op = *insn;
    long imm;
    unsigned long target;

    if ((op & 0x9f000000) == 0x90000000) {          /* ADRP */
        imm = (op >> 29) & 0x3;
        imm |= (op >> 3) & 0x1ffffc;
        if (imm & (1 << 20))
            imm -= (1 << 21);
        target = (orig_pc & ~0xfffUL) + (imm << 12);
        imm = (long)((target >> 12) - (new_pc >> 12));
        if (imm < -(1 << 20) || imm >= (1 << 20))
            return -1;
        *insn = (op & 0x9f00001f) | ((imm & 0x3) << 29) | ((imm & 0x1ffffc) << 3);
        return 0;
    }
    if ((op & 0x9f000000) == 0x10000000) {          /* ADR */
        imm = (op >> 29) & 0x3;
        imm |= (op >> 3) & 0x1ffffc;
        if (imm & (1 << 20))
            imm -= (1 << 21);
        target = orig_pc + imm;
        imm = (long)target - (long)new_pc;
        if (imm < -(1 << 20) || imm >= (1 << 20))
            return -1;
        *insn = (op & 0x9f00001f) | ((imm & 0x3) << 29) | ((imm & 0x1ffffc) << 3);
        return 0;
    }
    if ((op & 0x7c000000) == 0x14000000) {          /* B / BL */
        imm = (op & 0x03ffffff) << 2;
        if (imm & (1 << 27))                    /* 28-bit value: sign bit is bit27 */
            imm -= (1 << 28);
        target = orig_pc + imm;
        imm = (long)target - (long)new_pc;
        if (imm & 3)
            return -1;
        imm >>= 2;
        if (imm < -(1 << 25) || imm >= (1 << 25))
            return -1;
        *insn = (op & 0xfc000000) | (imm & 0x03ffffff);
        return 0;
    }
    if ((op & 0xff000000) == 0x18000000 || /* LDR literal W */
        (op & 0xff000000) == 0x58000000) { /* LDR literal X */
        imm = (op >> 5) & 0x7ffff;         /* imm19 */
        if (imm & (1 << 18))
            imm -= (1 << 19);              /* 19-bit sign extend */
        target = orig_pc + (imm << 2);
        imm = (long)(target - new_pc) >> 2;
        if (imm < -(1 << 18) || imm >= (1 << 18))
            return -1;
        *insn = (op & 0xff00001f) | ((imm << 5) & 0x00ffffe0);
        return 0;
    }
    return 0;  /* 非 PC-relative，原样拷贝 */
}

static int tramp_ok;

static void build_trampoline(unsigned long orig)
{
    u32 *tramp = (u32 *)&pte_trampoline_area;
    int i;
    int bad = 0;

    tramp_ok = 0;
    for (i = 0; i < 4; i++)
        tramp[i] = orig_insns[i];

    for (i = 0; i < 4; i++) {
        unsigned long orig_pc = orig + i * 4;
        unsigned long new_pc = (unsigned long)&pte_trampoline_area + i * 4;
        if (reloc_pc_relative(&tramp[i], orig_pc, new_pc)) {
            pr_err("pte_track: unrelocatable insn #%d @+%d (0x%08x)\n",
                   i, i * 4, orig_insns[i]);
            bad = 1;
        }
    }
    if (bad)
        return;

    tramp[4] = INSN_LDR_X17_8;
    tramp[5] = INSN_BR_X17;
    *(unsigned long *)&tramp[6] = orig + 16;      /* .quad orig+16 */

    *(unsigned long *)&pte_tramp_slot = (unsigned long)&pte_trampoline_area;
    tramp_ok = 1;
    pr_info("pte_track: trampoline @%px -> orig+16 @%px\n",
            (void *)&pte_trampoline_area, (void *)(orig + 16));
}

/*
 * stop_machine 回调：此时所有其他 CPU 都已停在 cpu_stopper 线程，
 * 不可能再有 CPU 正在执行 do_mem_abort，安全改写 16 字节跳板。
 */
static int patch_stm_fn(void *data)
{
    unsigned long orig = do_mem_abort_addr;
    unsigned long orig_page = orig & PAGE_MASK;
    u32 *p = (u32 *)orig;
    int *pres = (int *)data;
    int ret;

    local_irq_disable();  /* 再关本地中断，确保本核不被异常打断 */

    ret = set_memory_rw_fn(orig_page, 1);
    if (ret) {
        pr_err("pte_track: set_memory_rw(orig) failed %d\n", ret);
        *pres = ret;
        goto out;
    }

    /*
     * 关键：写入顺序
     *   1) 先写跳板目标地址 p[2..3]：此时 p[0]/p[1] 还是原指令，CPU 顺序执行
     *      不会跳到这里，写地址绝对安全；
     *   2) 再写 p[0] = ldr x17,#8：此时 p[1] 还是原第 2 条指令，即使此刻有 CPU
     *      执行到开头，它只会把正确地址读进 x17，然后继续执行原第 2 条指令；
     *   3) 最后写 p[1] = br x17：从此刻起才真正接管。
     * 这个顺序在没有 stop_machine 时也把"br 到垃圾地址"的窗口压到最小，
     * 配合 stop_machine 则完全消除。
     */
    *(unsigned long *)&p[2] = (unsigned long)pte_stub_entry;
    p[0] = INSN_LDR_X17_8;
    p[1] = INSN_BR_X17;

    flush_icache_asm(orig, orig + 16);
    set_memory_ro_fn(orig_page, 1);
    *pres = 0;

out:
    local_irq_enable();
    return 0;
}

static int patch_do_mem_abort(void)
{
    unsigned long orig = do_mem_abort_addr;
    unsigned long mod_page = (unsigned long)pte_stub_entry & PAGE_MASK;
    int mod_pages = 2;
    int pres = -1;
    int ret;

    memcpy(orig_insns, (void *)orig, 16);

    /* 先把模块内 stub/trampoline 页改可写，建好 trampoline */
    ret = set_memory_rw_fn(mod_page, mod_pages);
    if (ret) {
        pr_err("pte_track: set_memory_rw(mod) failed %d\n", ret);
        return ret;
    }
    build_trampoline(orig);
    set_memory_ro_fn(mod_page, mod_pages);
    if (!tramp_ok) {
        pr_err("pte_track: trampoline build failed\n");
        return -EINVAL;
    }

    /* 用 stop_machine 原子地 patch 内核 text */
    pr_info("pte_track: stop_machine patching do_mem_abort @%px ...\n", (void *)orig);
    stop_machine_fn(patch_stm_fn, &pres, NULL);
    if (pres)
        return pres;

    pr_info("pte_track: do_mem_abort @%px patched, stub @%px\n",
            (void *)orig, (void *)pte_stub_entry);
    return 0;
}

/* stop_machine 回调：恢复原 16 字节 */
static int unpatch_stm_fn(void *data)
{
    unsigned long orig = do_mem_abort_addr;
    unsigned long orig_page = orig & PAGE_MASK;
    u32 *p = (u32 *)orig;
    int *pres = (int *)data;

    local_irq_disable();
    set_memory_rw_fn(orig_page, 1);
    p[0] = orig_insns[0];
    p[1] = orig_insns[1];
    p[2] = orig_insns[2];
    p[3] = orig_insns[3];
    flush_icache_asm(orig, orig + 16);
    set_memory_ro_fn(orig_page, 1);
    local_irq_enable();
    *pres = 0;
    return 0;
}

static void unpatch_do_mem_abort(void)
{
    unsigned long orig = do_mem_abort_addr;
    int pres = -1;

    if (!orig || !orig_insns[0])
        return;
    stop_machine_fn(unpatch_stm_fn, &pres, NULL);
    pr_info("pte_track: do_mem_abort restored\n");
}

/* ===================== ioctl 实现 ===================== */

static int hook_page(struct mm_struct *mm, unsigned long addr,
                     u64 v3, u64 v4, u64 v5)
{
    unsigned long page = addr & PAGE_MASK;
    struct vm_area_struct *vma;
    pte_t *ptep;
    pte_t pte;
    unsigned long flags;
    int idx = -1;
    int i;

    mmap_read_lock(mm);
    vma = find_vma(mm, page);
    ptep = find_pte(mm, page);
    if (!vma || vma->vm_start > page || !ptep || !pte_present(*ptep)) {
        mmap_read_unlock(mm);
        return -EINVAL;
    }
    pte = *ptep;
    if (!(pte_val(pte) & PTE_USER)) {
        mmap_read_unlock(mm);
        return -EINVAL;
    }
    pte_val(pte) |= PTE_UXN_BIT;
    WRITE_ONCE(*ptep, pte);
    flush_tlb_page_direct(mm, page);   /* 6.6: 替代 flush_tlb_page() */
    mmap_read_unlock(mm);

    spin_lock_irqsave(&hooks_lock, flags);
    for (i = 0; i < MAX_HOOKS; i++) {
        if (hooks[i].mm == mm && hooks[i].page == page) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        for (i = 0; i < MAX_HOOKS; i++) {
            if (!hooks[i].mm) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) {
            mmget(mm);
            hooks[idx].mm = mm;
        }
    }
    if (idx >= 0) {
        hooks[idx].page = page;
        hooks[idx].v3 = v3;
        hooks[idx].v4 = v4;
        hooks[idx].v5 = v5;
    }
    spin_unlock_irqrestore(&hooks_lock, flags);

    if (idx < 0) {
        /* 回滚 UXN */
        set_pte_uxn(mm, page, false);
        return -ENOSPC;
    }
    pr_info("pte_track: HOOK %px (v=%llx,%llx,%llx)\n",
            (void *)page, v3, v4, v5);
    return 0;
}

static int arm_page(struct mm_struct *mm, unsigned long addr,
                    bool set_v, u64 v3, u64 v4, u64 v5)
{
    unsigned long page = addr & PAGE_MASK;
    struct hook_entry *h = NULL;
    unsigned long flags;
    int i;

    spin_lock_irqsave(&hooks_lock, flags);
    for (i = 0; i < MAX_HOOKS; i++) {
        if (hooks[i].mm == mm && hooks[i].page == page) {
            h = &hooks[i];
            break;
        }
    }
    if (h && set_v) {
        h->v3 = v3;
        h->v4 = v4;
        h->v5 = v5;
    }
    spin_unlock_irqrestore(&hooks_lock, flags);
    if (!h)
        return -EINVAL;

    set_pte_uxn(mm, page, true);
    return 0;
}

static int unhook_page(struct mm_struct *mm, unsigned long addr)
{
    unsigned long page = addr & PAGE_MASK;
    struct hook_entry *h = NULL;
    unsigned long flags;
    int i;

    spin_lock_irqsave(&hooks_lock, flags);
    for (i = 0; i < MAX_HOOKS; i++) {
        if (hooks[i].mm == mm && hooks[i].page == page) {
            h = &hooks[i];
            break;
        }
    }
    if (h)
        h->mm = NULL;
    spin_unlock_irqrestore(&hooks_lock, flags);
    if (!h)
        return -EINVAL;

    set_pte_uxn(mm, page, false);
    mmput(mm);
    pr_info("pte_track: UNHOOK %px\n", (void *)page);
    return 0;
}

static long pte_track_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case PTE_TRACK_IOCTL_HOOK: {
        struct pte_track_hook_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        return hook_page(current->mm, (unsigned long)req.orig_page_addr,
                         req.v3, req.v4, req.v5);
    }
    case PTE_TRACK_IOCTL_ARM: {
        struct pte_track_arm_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        return arm_page(current->mm, (unsigned long)req.orig_page_addr,
                        !!req.set_v, req.v3, req.v4, req.v5);
    }
    case PTE_TRACK_IOCTL_UNHOOK: {
        struct pte_track_unhook_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        return unhook_page(current->mm, (unsigned long)req.orig_page_addr);
    }
    default:
        return -ENOTTY;
    }
}

static const struct file_operations pte_track_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = pte_track_ioctl,
};

static struct miscdevice pte_track_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "pte_track",
    .fops = &pte_track_fops,
};

/* ===================== init / exit ===================== */

static int __init pte_track_init(void)
{
    if (!kln_addr_param) {
        pr_err("pte_track: kln_addr param missing (from /proc/kallsyms)\n");
        return -EINVAL;
    }
    kln = (kln_t)kln_addr_param;

    do_mem_abort_addr = kln("do_mem_abort");
    set_memory_rw_fn = (set_mem_t)kln("set_memory_rw");
    set_memory_ro_fn = (set_mem_t)kln("set_memory_ro");
    fpsimd_save_state_fn = (fpsimd_save_t)kln("fpsimd_save_state");
    fpsimd_restore_current_state_fn = (fpsimd_restore_t)kln("fpsimd_restore_current_state");
    stop_machine_fn = (stop_machine_t)kln("stop_machine");

    pr_info("pte_track: do_mem_abort=%px rw=%px ro=%px stop_machine=%px\n",
            (void *)do_mem_abort_addr, (void *)set_memory_rw_fn,
            (void *)set_memory_ro_fn, (void *)stop_machine_fn);

    if (!do_mem_abort_addr || !set_memory_rw_fn || !set_memory_ro_fn) {
        pr_err("pte_track: core symbol resolution failed\n");
        return -ENXIO;
    }
    /* stop_machine 是稳定性关键，解析不到就拒绝加载，不带病重启设备 */
    if (!stop_machine_fn) {
        pr_err("pte_track: stop_machine not found, refuse to load (safety)\n");
        return -ENXIO;
    }

    if (patch_do_mem_abort())
        return -EPERM;

    if (misc_register(&pte_track_dev)) {
        unpatch_do_mem_abort();
        return -ENODEV;
    }
    pr_info("pte_track: /dev/pte_track ready\n");
    return 0;
}

static void __exit pte_track_exit(void)
{
    unsigned long flags;
    int i;

    misc_deregister(&pte_track_dev);

    /* 先把所有 hook 页恢复可执行，再退 patch */
    spin_lock_irqsave(&hooks_lock, flags);
    for (i = 0; i < MAX_HOOKS; i++) {
        if (hooks[i].mm) {
            unsigned long page = hooks[i].page;
            struct mm_struct *mm = hooks[i].mm;
            hooks[i].mm = NULL;
            spin_unlock_irqrestore(&hooks_lock, flags);
            set_pte_uxn(mm, page, false);
            mmput(mm);
            spin_lock_irqsave(&hooks_lock, flags);
        }
    }
    spin_unlock_irqrestore(&hooks_lock, flags);

    unpatch_do_mem_abort();
    pr_info("pte_track: unloaded\n");
}

module_init(pte_track_init);
module_exit(pte_track_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PTE UXN code-page hook via do_mem_abort inline hook (stop_machine safe)");
