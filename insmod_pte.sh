#!/system/bin/sh
# insmod_pte.sh —— 在设备上加载 pte_track.ko
# 用法: sh insmod_pte.sh        （需 root / KernelSU）
set -e

# 解除 kptr_restrict，让 /proc/kallsyms 显示真实符号地址（6.6 GKI 默认隐藏）
echo 0 > /proc/sys/kernel/kptr_restrict 2>/dev/null || true

KLN=$(grep -w kallsyms_lookup_name /proc/kallsyms | awk '{print $1}')
DO_MEM=$(grep -w do_mem_abort /proc/kallsyms | awk '{print $1}')
STOP=$(grep -w stop_machine /proc/kallsyms | awk '{print $1}')

echo "kallsyms_lookup_name = 0x$KLN"
echo "do_mem_abort         = 0x$DO_MEM"
echo "stop_machine         = 0x$STOP"

# 地址必须是 16 进制且不是全 0
if [ -z "$KLN" ] || [ "$KLN" = "0000000000000000" ] || [ "$KLN" = "0" ]; then
    echo "[!] kallsyms_lookup_name 地址为空或全 0，kptr_restrict 没解除，中止"
    exit 1
fi
if [ -z "$STOP" ] || [ "$STOP" = "0000000000000000" ] || [ "$STOP" = "0" ]; then
    echo "[!] stop_machine 地址为空或全 0，中止"
    exit 1
fi

# 若已加载先卸载
rmmod pte_track 2>/dev/null || true

insmod ./pte_track.ko kln_addr=0x$KLN
echo "[+] loaded, dmesg 末尾："
dmesg | tail -n 20
ls -l /dev/pte_track 2>/dev/null && echo "[+] /dev/pte_track 就绪"
