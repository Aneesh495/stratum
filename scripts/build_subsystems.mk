# Subsystems build rules for Stratum OS

LLVM_PREFIX ?= /opt/homebrew/opt/llvm
CLANG       ?= $(if $(wildcard $(LLVM_PREFIX)/bin/clang),$(LLVM_PREFIX)/bin/clang,clang)
LD_LLD      ?= $(if $(wildcard /opt/homebrew/bin/ld.lld),/opt/homebrew/bin/ld.lld,ld.lld)
LLD         ?= $(if $(wildcard /opt/homebrew/bin/lld),/opt/homebrew/bin/lld,lld)

NASM        ?= $(if $(wildcard /opt/homebrew/bin/nasm),/opt/homebrew/bin/nasm,nasm)

BUILD_DIR ?= build

UEFI_SRCS := boot/uefi/main.c boot/uefi/elf.c
UEFI_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UEFI_SRCS))

KERNEL_CSRCS := kernel/core/string.c \
                kernel/core/kprintf.c \
                kernel/core/boot.c \
                kernel/core/boot_check.c \
                kernel/core/main.c \
                kernel/core/syscall.c \
                kernel/core/user_elf.c \
                kernel/core/file.c \
                kernel/core/process.c \
                kernel/core/exec.c \
                kernel/ipc/pipe.c \
                kernel/ipc/channel.c \
                kernel/arch/x86_64/gdt.c \
                kernel/arch/x86_64/idt.c \
                kernel/arch/x86_64/smp.c \
                kernel/sched/sched.c \
                kernel/sched/timer.c \
                kernel/mm/pmm.c \
                kernel/mm/slab.c \
                kernel/mm/vmm.c \
                kernel/mm/anonymous.c \
                kernel/sync/spinlock.c \
                kernel/sync/mutex.c \
                kernel/sync/futex.c \
                kernel/sync/event.c \
                kernel/drivers/uart.c \
                kernel/drivers/fb.c \
                kernel/drivers/acpi.c \
                kernel/drivers/madt.c \
                kernel/drivers/apic.c \
                kernel/drivers/pci.c \
                kernel/drivers/virtqueue.c \
                kernel/drivers/virtio.c \
                kernel/drivers/virtio_blk.c \
                kernel/drivers/virtio_net.c \
                kernel/fs/vfs.c \
                kernel/fs/initramfs.c \
                kernel/fs/cpio.c \
                kernel/fs/journal.c \
                kernel/fs/stratafs.c \
                kernel/net/net.c \
                kernel/net/ethernet.c \
                kernel/net/arp.c \
                kernel/net/ipv4.c \
                kernel/net/ipv4_decode.c \
                kernel/net/checksum.c \
                kernel/net/icmp.c \
                kernel/net/udp.c \
                kernel/net/tcp.c \
                kernel/net/socket.c \
                kernel/core/sha256.c \
                kernel/ledger/ledger.c \
                kernel/debug/trace.c \
                kernel/debug/panic.c \
                tests/guest/block_wrap.c \
                tests/guest/tlb.c \
                tests/guest/stress.c

KERNEL_ASMSRCS := kernel/arch/x86_64/entry.S \
                  kernel/arch/x86_64/interrupts.S \
                  kernel/arch/x86_64/switch.S \
                  kernel/arch/x86_64/syscall_asm.S \
                  kernel/arch/x86_64/trampoline_blob.S \
                  kernel/core/user_init_blob.S

KERNEL_COBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_CSRCS))
KERNEL_ASMOBJS := $(patsubst %.S,$(BUILD_DIR)/%.o,$(KERNEL_ASMSRCS))
KERNEL_OBJS := $(KERNEL_COBJS) $(KERNEL_ASMOBJS)

UEFI_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fno-stack-protector \
               -fshort-wchar -mno-red-zone -Wall -Wextra -std=c17 -O2 \
               -Iinclude -Iboot/uefi -Iinclude/boot -Iinclude/shared

KERNEL_CFLAGS := -target x86_64-unknown-none-elf -ffreestanding -mno-red-zone \
                 -mcmodel=kernel -mno-mmx -mno-sse -fno-stack-protector \
                 -fno-omit-frame-pointer -Wall -Wextra -std=c17 -O2 \
                 -Iinclude -Iinclude/kernel -Iinclude/shared

KERNEL_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T kernel/arch/x86_64/linker.ld

.PHONY: build-all build-loader build-kernel build-initramfs

build-all: build-loader build-kernel build-initramfs

# Build UEFI Loader
build-loader: $(BUILD_DIR)/BOOTX64.EFI

$(BUILD_DIR)/boot/uefi/%.o: boot/uefi/%.c
	@mkdir -p $(dir $@)
	$(CLANG) $(UEFI_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/BOOTX64.EFI: $(UEFI_OBJS)
	@mkdir -p $(dir $@)
	$(LLD) -flavor link /subsystem:efi_application /entry:EfiMain /dynamicbase:no /nodefaultlib /Brepro /timestamp:1700000000 /out:$@ $^

# Build Kernel ELF
build-kernel: $(BUILD_DIR)/stratum.elf

$(BUILD_DIR)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CLANG) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/tests/guest/%.o: tests/guest/%.c
	@mkdir -p $(dir $@)
	$(CLANG) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

USER_CFLAGS := -target x86_64-unknown-none-elf -ffreestanding -mno-red-zone -mgeneral-regs-only \
               -fno-stack-protector -fno-omit-frame-pointer -Wall -Wextra -std=c17 -O2 \
               -Iinclude -Iinclude/user -Iinclude/shared

USER_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T user/lib/user.ld

USER_LIB_SRCS := user/lib/crt0.S user/lib/thread_start.S user/lib/syscall.c user/lib/string.c user/lib/printf.c
USER_LIB_OBJS := $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(basename $(USER_LIB_SRCS))))

$(BUILD_DIR)/user/%.o: user/%.c scripts/build_subsystems.mk
	@mkdir -p $(dir $@)
	$(CLANG) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/user/%.o: user/%.S
	@mkdir -p $(dir $@)
	$(CLANG) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/user/init_test.o: tests/guest/init.c scripts/build_subsystems.mk
	@mkdir -p $(dir $@)
	$(CLANG) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/user_init.elf: $(USER_LIB_OBJS) $(BUILD_DIR)/user/apps/sh.o $(BUILD_DIR)/user/apps/ledgerd.o $(BUILD_DIR)/user/apps/init.o user/lib/user.ld
	$(LD_LLD) $(USER_LDFLAGS) -o $@ $(filter %.o,$^)

.SECONDARY: $(BUILD_DIR)/user/exec_campaign.o $(BUILD_DIR)/user/thread_campaign.o $(BUILD_DIR)/user/ipc_campaign.o $(BUILD_DIR)/user/fs_campaign.o $(BUILD_DIR)/user/net_campaign.o
$(BUILD_DIR)/user/%_campaign.o: tests/guest/%_campaign.c scripts/build_subsystems.mk
	@mkdir -p $(dir $@)
	$(CLANG) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/user_%_campaign.elf: $(USER_LIB_OBJS) $(BUILD_DIR)/user/%_campaign.o user/lib/user.ld
	$(LD_LLD) $(USER_LDFLAGS) -o $@ $(filter %.o,$^)

$(BUILD_DIR)/user_tools.elf: $(USER_LIB_OBJS) $(BUILD_DIR)/user/apps/tools.o user/lib/user.ld
	$(LD_LLD) $(USER_LDFLAGS) -o $@ $(filter %.o,$^)

$(BUILD_DIR)/user_test.elf: $(USER_LIB_OBJS) $(BUILD_DIR)/user/init_test.o user/lib/user.ld
	$(LD_LLD) $(USER_LDFLAGS) -o $@ $(filter %.o,$^)

$(BUILD_DIR)/kernel/core/user_init_blob.o: kernel/core/user_init_blob.S $(BUILD_DIR)/user_init.elf $(BUILD_DIR)/user_test.elf
	@mkdir -p $(dir $@)
	$(CLANG) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/trampoline.bin: kernel/arch/x86_64/trampoline.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

$(BUILD_DIR)/kernel/arch/x86_64/trampoline_blob.o: kernel/arch/x86_64/trampoline_blob.S $(BUILD_DIR)/trampoline.bin
	@mkdir -p $(dir $@)
	$(CLANG) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(CLANG) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/stratum.elf: $(BUILD_DIR)/user_init.elf $(KERNEL_OBJS) kernel/arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	$(LD_LLD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

# Build Minimal Initramfs
build-initramfs: $(BUILD_DIR)/initramfs.cpio

$(BUILD_DIR)/initramfs.cpio: $(BUILD_DIR)/user_init.elf $(BUILD_DIR)/user_tools.elf $(BUILD_DIR)/user_exec_campaign.elf $(BUILD_DIR)/user_thread_campaign.elf $(BUILD_DIR)/user_ipc_campaign.elf $(BUILD_DIR)/user_fs_campaign.elf $(BUILD_DIR)/user_net_campaign.elf scripts/build_cpio.py
	@mkdir -p $(BUILD_DIR)
	@$(PYTHON) scripts/build_cpio.py $< $@ true=$(BUILD_DIR)/user_tools.elf \
		upper=$(BUILD_DIR)/user_tools.elf exec-campaign=$(BUILD_DIR)/user_exec_campaign.elf \
		thread-campaign=$(BUILD_DIR)/user_thread_campaign.elf ipc-campaign=$(BUILD_DIR)/user_ipc_campaign.elf \
		fs-campaign=$(BUILD_DIR)/user_fs_campaign.elf net-campaign=$(BUILD_DIR)/user_net_campaign.elf


-include $(UEFI_OBJS:.o=.d) $(KERNEL_OBJS:.o=.d) $(USER_LIB_OBJS:.o=.d) \
         $(BUILD_DIR)/user/apps/init.d $(BUILD_DIR)/user/apps/sh.d $(BUILD_DIR)/user/apps/ledgerd.d $(BUILD_DIR)/user/init_test.d
-include $(BUILD_DIR)/user/apps/tools.d $(BUILD_DIR)/user/exec_campaign.d $(BUILD_DIR)/user/thread_campaign.d $(BUILD_DIR)/user/ipc_campaign.d $(BUILD_DIR)/user/fs_campaign.d $(BUILD_DIR)/user/net_campaign.d
