# pte_track（6.6.118-android15 GKI 适配版）

对目标进程的用户页 PTE 置 UXN（bit 54）触发 IABT 异常，再 inline hook `do_mem_abort` 的驱动（游戏辅助）。
本版在"stop_machine 安全版"基础上完成了 **6.6 GKI 的编译/加载适配**。

目标设备内核（设备实测）：

```
6.6.118-android15-8-g93e223c276e7-abogki-4k      # OnePlus Ace 6 / SukiSU Ultra
```

模块内嵌 vermagic 应与之匹配：

```
6.6.118-android15-8-g93e223c276e7-abogki-4k SMP preempt mod_unload modversions aarch64
```

源码同时兼容 5.10（已在 linux-5.10.209 与 linux-6.6.118 两棵树上编译验证通过，原有编译告警清零）。

## 本版改动（6.6 适配）

1. `pte_offset_map()` → `pte_offset_kernel()`：6.6 的 `pte_offset_map()` 是引用未导出符号 `__pte_offset_map` 的 inline 包装，直接编译报 undefined；`pte_offset_kernel()` 纯内联，等价 5.10 的宏。
2. 新增自包含 `flush_tlb_page_direct()` 并替换两处 `flush_tlb_page()`：6.6 的展开链会引用未导出的 `__mmu_notifier_arch_invalidate_secondary_tlbs`。
3. ESR 判定修正：`(esr & ESR_ELx_EC_MASK) != ESR_ELx_EC_IABT_LOW` 恒真（写法错误）→ `ESR_ELx_EC(esr) != ESR_ELx_EC_IABT_LOW`。
4. LDR literal 重定位修正：掩码 `0x3b000000`（与第二条件矛盾、恒假，有编译告警）→ `0xff000000`；立即数提取 `(op & 0x00ffffe0) >> 3` → `(op >> 5) & 0x7ffff`。
5. B/BL 重定位符号位修正：bit25 / `1<<26` → bit27 / `1<<28`（28 位立即数的符号位）。
6. 文件头注释同步更新。

保留的既有加固：patch/unpatch 全程包在 `stop_machine`；写入顺序（地址→ldr→br）；`untagged_addr`、`spin_lock_irqsave`、手动 icache flush；符号解析失败则拒绝加载。

## 编译

### 方式 A：GitHub Actions（推荐）

见 `GITHUB.md`。push 仓库 → Actions 跑 workflow → 下载 artifact 中的 `pte_track.ko`。
workflow 会：clone android15-6.6 GKI 源码 → 固定 SUBLEVEL → 覆盖 LOCALVERSION → 完整编译内核（生成带 CRC 的 `Module.symvers`）→ 编译模块 → 校验 vermagic。

### 方式 B：本地

```sh
# KDIR 指向与设备同源的 android15-6.6 GKI 源码树，且必须先完整 make 过一次（生成 Module.symvers）
make KDIR=/path/to/android15-6.6 LOCALVERSION="-android15-8-g93e223c276e7-abogki-4k"

# 用 GCC 工具链：
make KDIR=/path/to/android15-6.6 LLVM= CROSS_COMPILE=aarch64-linux-gnu- \
     LOCALVERSION="-android15-8-g93e223c276e7-abogki-4k"
```

产物 `pte_track.ko`，可自检：`modinfo -F vermagic pte_track.ko`。
（发布包内不含预编译 ko——GKI 需与目标设备同源编译，请按上面方式生成。）

## 加载（设备上，root / KernelSU）

```sh
sh insmod_pte.sh
```

脚本自动放开 `kptr_restrict`，从 `/proc/kallsyms` 取 `kallsyms_lookup_name` / `stop_machine` 地址（取不到则拒载），然后 `insmod pte_track.ko kln_addr=0x...`。
成功标志：`/dev/pte_track` 就绪，dmesg 出现 `stop_machine patching do_mem_abort`。

## 自测 / 卸载

```sh
aarch64-linux-android-clang -O2 -o test_pte test_pte.c   # 或用 NDK
./test_pte            # 看到 === ALL PASS ===
rmmod pte_track       # 卸载
```

## 常见问题

- **dmesg 报 version magic '...' should be '...'**：vermagic 不匹配。核对设备 `uname -r`，修改 workflow 输入（kernel_sublevel / localversion）或 make 的 LOCALVERSION 后重编。
- **dmesg 报 disagrees about version of symbol**：设备内核与编译用的 GKI 源码不是同源/同配置（MODVERSIONS CRC 不一致）。换用与设备内核同分支同版本的源码树编译。
- **模块链接报 undefined symbol**：内核树没完整编译过（缺 `Module.symvers`）。先跑完整 `make`。
- **签名**：GKI 的 `CONFIG_MODULE_SIG_PROTECT=y` 在源码中定义 `sig_enforce=false`，允许未签名模块（本驱动即未签名）；除非设备内核另行开启强制签名。
- **换内核版本**：改 workflow 的两个输入即可；5.10 老内核用本地方式编译。

## 文件

```
pte_core.c            驱动主源码（6.6 适配 + stop_machine 安全版）
pte_stub.S            do_mem_abort 跳板汇编
pte_track_ioctl.h     用户态接口（HOOK/ARM/UNHOOK）
Kbuild / Makefile     构建（Makefile 支持 KDIR / LLVM / LOCALVERSION）
insmod_pte.sh         设备一键加载
test_pte.c            用户态自测
.github/workflows/build.yml   GitHub Actions 构建
GITHUB.md             Actions 使用说明
```
