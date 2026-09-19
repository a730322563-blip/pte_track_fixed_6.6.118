# pte_track 构建（GKI 内核源码树外部模块）
#
# 目标内核：android15-6.6 GKI
#   设备实测版本串：6.6.118-android15-8-g93e223c276e7-abogki-4k
#
# 用法：
#   make KDIR=/path/to/kernel-src
#   # 若要与设备内核 vermagic 精确匹配（6.6 GKI 有 MODVERSIONS/vermagic 校验）：
#   make KDIR=/path/to/kernel-src LOCALVERSION="-android15-8-g93e223c276e7-abogki-4k"
#
# 说明：
#   - KDIR 指向与设备同源的 android15-6.6 GKI 源码树（建议先完整 make 一次，
#     生成带符号 CRC 的 Module.symvers，否则 6.6 的 modpost 会报未解析符号）；
#   - LOCALVERSION 会原样透传给 kbuild（为空时不传，使用内核树自带版本串）；
#   - 默认用 LLVM 工具链（GKI 官方方式）；用 GCC 时传 LLVM= 并设置 CROSS_COMPILE。

KDIR ?= /workspace/kernel/src
ARCH ?= arm64
CROSS_COMPILE ?=
LLVM ?= 1
LLVM_IAS ?= 1

obj-m := pte_track.o
pte_track-objs := pte_core.o pte_stub.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		LLVM=$(LLVM) LLVM_IAS=$(LLVM_IAS) \
		$(if $(LOCALVERSION),LOCALVERSION="$(LOCALVERSION)",) \
		modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) clean

help:
	@echo "编译: make KDIR=/path/to/kernel-src [LOCALVERSION='-android15-8-g93e223c276e7-abogki-4k']"
	@echo "GCC : make KDIR=/path/to/kernel-src LLVM= CROSS_COMPILE=aarch64-linux-gnu-"