JOBS ?= 8
.PHONY: all kernel sources setup wifi mesa boot audit
all: kernel
sources:
	@test -d xnu-src/xnu-8796.121.2 || xnu/apply.sh
setup: sources
	xnu-deps/setup.sh
kernel: sources
	@test -e xnu-deps/host/bin/ld64 || xnu-deps/setup.sh
	tools/make-kernel.sh -j$(JOBS)
wifi: sources
	tools/mk-ath11k.sh
	tools/mk-ath10k.sh
mesa:
	tools/mk-mesa.sh
boot:
	@bash tools/build-boot.sh
audit:
	python3 tools/audit-public.py
