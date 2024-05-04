SHELL := /bin/bash
.DEFAULT_GOAL := all

LLVM_PREFIX ?= /opt/homebrew/opt/llvm
CLANG ?= $(if $(wildcard $(LLVM_PREFIX)/bin/clang),$(LLVM_PREFIX)/bin/clang,clang)
PYTHON ?= python3
QEMU ?= qemu-system-x86_64
OVMF ?=
CPUS ?= 4
MEMORY ?= 256M
LEDGER_PORT ?=
STATE_DISK ?= var/stratum-data.img

export SOURCE_DATE_EPOCH := 1700000000
export CLANG QEMU OVMF

BUILD_DIR := build
BOOT_DISK := $(BUILD_DIR)/images/stratum-boot.img
DATA_DISK := $(BUILD_DIR)/images/stratum-data.img

.PHONY: all bootstrap doctor build image run debug test smoke test-boot test-mm test-smp \
        test-sched test-abi test-drivers test-storage test-storage-crash test-net test-faults fuzz soak bench \
        docs scope-check demo evidence acceptance verify release clean

all: image

bootstrap: doctor

doctor:
	@$(PYTHON) scripts/doctor.py

build:
	@$(MAKE) -f scripts/build_subsystems.mk PYTHON="$(PYTHON)" build-all

image: build
	@$(PYTHON) scripts/build_image.py --boot-img "$(BOOT_DISK)" --data-img "$(DATA_DISK)" \
		--loader "$(BUILD_DIR)/BOOTX64.EFI" --kernel "$(BUILD_DIR)/stratum.elf" \
		--initramfs "$(BUILD_DIR)/initramfs.cpio"

VM_ARGS = --boot-img "$(BOOT_DISK)" --data-img "$(STATE_DISK)" --data-template "$(DATA_DISK)" \
	--cpus "$(CPUS)" --mem "$(MEMORY)" $(if $(LEDGER_PORT),--ledger-port "$(LEDGER_PORT)")

run: image
	@$(PYTHON) scripts/run_vm.py $(VM_ARGS)

debug: image
	@$(PYTHON) scripts/run_vm.py $(VM_ARGS) --gdb 1234

test:
	@$(PYTHON) scripts/run_tests.py

smoke: image
	@$(PYTHON) scripts/test_smoke.py
	@$(PYTHON) scripts/test_recovery.py
	@$(PYTHON) scripts/test_ledger.py
	@$(PYTHON) scripts/test_replication.py

test-boot: image
	@$(PYTHON) scripts/test_boot.py
	@$(PYTHON) scripts/test_smoke.py --matrix

test-mm test-smp test-sched test-abi test-faults: image
	@$(PYTHON) scripts/test_diagnostics.py $(patsubst test-%,%,$@)

test-drivers: image
	@$(PYTHON) scripts/test_block_wrap.py

test-storage: image
	@$(PYTHON) scripts/test_diagnostics.py storage
	@$(PYTHON) scripts/test_recovery.py

test-storage-crash: image
	@$(PYTHON) scripts/test_wal_crash.py --repeats 10

.PHONY: test-boot-campaign test-exec test-threads test-tlb test-ipc test-storage-mixed
test-boot-campaign: image
	@$(PYTHON) scripts/test_boot_campaign.py

test-exec: image
	@$(PYTHON) scripts/test_exec.py

test-threads: image
	@$(PYTHON) scripts/test_threads.py

test-tlb: image
	@$(PYTHON) scripts/test_tlb.py

test-ipc: image
	@$(PYTHON) scripts/test_ipc.py

test-storage-mixed: image
	@$(PYTHON) tests/host/wal_model.py
	@$(PYTHON) scripts/test_storage_mixed.py

test-net demo: image
	@$(PYTHON) scripts/test_ledger.py
	@$(PYTHON) scripts/test_replication.py

fuzz:
	@$(PYTHON) scripts/run_fuzz.py

soak: image
	@$(PYTHON) scripts/test_diagnostics.py stress

bench:
	@$(PYTHON) scripts/run_benchmarks.py

docs:
	@$(PYTHON) scripts/check_docs.py

scope-check:
	@$(PYTHON) scripts/scope_check.py

evidence:
	@$(PYTHON) scripts/run_acceptance.py --smoke

acceptance:
	@$(PYTHON) scripts/run_acceptance.py

verify:
	@$(PYTHON) scripts/verify_evidence.py "$(EVIDENCE)"

release: acceptance

clean:
	@rm -rf "$(BUILD_DIR)"
