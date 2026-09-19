// pte_track 用户态测试: 验证 UXN fault -> V3/V4/V5 改写
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>

#define PTE_TRACK_MAGIC 'P'
struct pte_track_hook_req { uint64_t orig_page_addr; uint64_t v3, v4, v5; };
struct pte_track_arm_req  { uint64_t orig_page_addr; uint32_t set_v; uint32_t _pad; uint64_t v3, v4, v5; };
struct pte_track_unhook_req { uint64_t orig_page_addr; };
#define PTE_TRACK_IOCTL_HOOK   _IOW(PTE_TRACK_MAGIC, 1, struct pte_track_hook_req)
#define PTE_TRACK_IOCTL_ARM    _IOW(PTE_TRACK_MAGIC, 2, struct pte_track_arm_req)
#define PTE_TRACK_IOCTL_UNHOOK _IOW(PTE_TRACK_MAGIC, 3, struct pte_track_unhook_req)

__asm__(
    ".global test_block_raw\n"
    ".type test_block_raw, %function\n"
    "test_block_raw:\n"
    "  nop\n"
    "  stp q3, q4, [x1, #0]\n"
    "  str q5, [x1, #32]\n"
    "  ret\n"
    ".size test_block_raw, .-test_block_raw\n");
extern void test_block_raw(void);

int main(void)
{
    int fd, ret;
    void *page;
    uint64_t *buf;
    long page_size = sysconf(_SC_PAGESIZE);

    fd = open("/dev/pte_track", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 1; }
    *(volatile char *)page = 0x55;
    memcpy(page, (void *)test_block_raw, 16);
    __builtin___clear_cache(page, page + page_size);

    buf = (uint64_t *)calloc(8, 8);

    struct pte_track_hook_req hreq = {
        .orig_page_addr = (uint64_t)page,
        .v3 = 0x1111222233334444ULL,
        .v4 = 0x5555666677778888ULL,
        .v5 = 0x9999AAAABBBBCCCCULL,
    };
    ret = ioctl(fd, PTE_TRACK_IOCTL_HOOK, &hreq);
    printf("HOOK ret=%d (%s)\n", ret, ret ? strerror(-ret) : "ok");
    if (ret) return 1;

    /*
     * 测试块布局：
     *   stp q3, q4, [x1, #0]  -> q3 @ 0  (buf[0..1]), q4 @ 16 (buf[2..3])
     *   str q5, [x1, #32]     -> q5 @ 32 (buf[4..5])
     * 驱动只写低 64 位，所以校验 buf[0]=V3, buf[2]=V4, buf[4]=V5。
     */
    printf("第一次调用...\n");
    ((void (*)(uint64_t *))page)(buf);
    printf("  结果: V3=%016llx V4=%016llx V5=%016llx\n",
           (unsigned long long)buf[0], (unsigned long long)buf[2],
           (unsigned long long)buf[4]);
    printf("  期望: V3=%016llx V4=%016llx V5=%016llx\n",
           (unsigned long long)hreq.v3, (unsigned long long)hreq.v4,
           (unsigned long long)hreq.v5);
    int pass1 = (buf[0] == hreq.v3 && buf[2] == hreq.v4 && buf[4] == hreq.v5);
    printf("  第一次 %s\n", pass1 ? "PASS" : "FAIL");

    struct pte_track_arm_req areq = {
        .orig_page_addr = (uint64_t)page,
        .set_v = 1,
        .v3 = 0x0102030405060708ULL,
        .v4 = 0x1020304050607080ULL,
        .v5 = 0xDEADBEEFCAFEBABEULL,
    };
    ret = ioctl(fd, PTE_TRACK_IOCTL_ARM, &areq);
    printf("ARM ret=%d (%s)\n", ret, ret ? strerror(-ret) : "ok");

    memset(buf, 0, 48);
    printf("第二次调用...\n");
    ((void (*)(uint64_t *))page)(buf);
    printf("  结果: V3=%016llx V4=%016llx V5=%016llx\n",
           (unsigned long long)buf[0], (unsigned long long)buf[2],
           (unsigned long long)buf[4]);
    int pass2 = (buf[0] == areq.v3 && buf[2] == areq.v4 && buf[4] == areq.v5);
    printf("  第二次 %s\n", pass2 ? "PASS" : "FAIL");

    struct pte_track_unhook_req ureq = { .orig_page_addr = (uint64_t)page };
    ret = ioctl(fd, PTE_TRACK_IOCTL_UNHOOK, &ureq);
    printf("UNHOOK ret=%d (%s)\n", ret, ret ? strerror(-ret) : "ok");

    close(fd);
    munmap(page, page_size);
    free(buf);
    printf(pass1 && pass2 ? "=== ALL PASS ===\n" : "=== FAIL ===\n");
    return (pass1 && pass2) ? 0 : 1;
}
