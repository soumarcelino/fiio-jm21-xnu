#!/bin/bash
# JM21 (khaje, WCN3990): Linux's ath10k snoc, unmodified, on the ath11k port's layer (wifi/lx) plus the JM21
# bus and compatibility headers (wifi/lx10k). Sources come from the same pinned Linux tarball and FreeBSD tree
# as tools/mk-ath11k.sh (run that once first: it fetches and patches them). Out: xnu-deps/build/ath10k/out/ath10k.o,
# which xnu-build.sh links in preference to ath11k.o.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
A=$ROOT/xnu-deps/build/ath11k
B=$ROOT/xnu-deps/build/ath10k
S=$A/freebsd/sys
W=$ROOT/wifi/lx
X=$ROOT/wifi/lx10k
[ -f $A/linux-7.0.14.tar.xz ] && [ -d $S ] || { echo "mk-ath10k.sh: run tools/mk-ath11k.sh first" >&2; exit 1; }
mkdir -p $B/src $B/obj $B/out
cd $B
if [ ! -f src/ath10k/snoc.c ]; then
	tar xJf $A/linux-7.0.14.tar.xz -C src --strip-components=5 linux-7.0.14/drivers/net/wireless/ath/ath10k \
		--wildcards 'linux-7.0.14/drivers/net/wireless/ath/*.[ch]'
	for f in src/ath10k/*.[ch] src/*.[ch]; do
		case "$(head -8 "$f")" in *ISC*|*"Permission to use, copy, modify"*|*BSD-3-Clause-Clear*|*"Dual BSD/GPL"*) ;;
		*) echo "mk-ath10k.sh: $f: licence not the one expected" >&2; exit 1 ;; esac
	done
fi
source "$ROOT/xnu-env.sh"
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing -funsigned-char -nostdinc
	-std=gnu17 -O2 -g -w -D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$X/include -I$W/include -I$A/gen -I$S -I$S/contrib/ck/include -I$S/compat/linuxkpi/common/include
	-I$S/compat/linuxkpi/dummy/include -include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000
	-Isrc/ath10k -Isrc -include $W/include/lx_prelude.h -include $X/include/lx10k_prelude.h -include $X/include/lx10k_snoc.h
	-DCONFIG_ATH10K_SNOC -DCONFIG_ATH10K_DEBUG -DLX_POOL_SIZE=0x2000000u -DLX_POOL_RINGS=0x400000u -DKBUILD_MODNAME=\"ath10k\"
	-DLX_HAKIOS_LX_H=\"$XNU_SRC/bsd/sys/hakios.lx.h\" -DLX_HAKIOS_QMI_H=\"$XNU_SRC/bsd/sys/hakios.qmi.h\""
DRIVER="core ce htc htt htt_rx htt_tx txrx wmi wmi-tlv mac hw bmi qmi qmi_wlfw_v01 p2p debug swap"
rm -f obj/* out/ath10k.o
pids=
for f in $DRIVER; do $CC $CFLAGS -c src/ath10k/$f.c -o obj/ath10k_$f.o 2> obj/ath10k_$f.err & done
$CC $CFLAGS -c src/regd.c -o obj/ath_regd.o 2> obj/ath_regd.err &
$CC $CFLAGS -I$X/src -I$W/src -c $X/src/lx10k_snoc.c -o obj/lx10k_snoc.o 2> obj/lx10k_snoc.err &
for f in lx_base lx_80211 lx_wpa; do $CC $CFLAGS -I$W/src -c $W/src/$f.c -o obj/lx_$f.o 2> obj/lx_$f.err & done
wait
bad=
for f in obj/*.err; do
	[ -f "${f%.err}.o" ] || { echo "--- $(basename "$f" .err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero snprintf vsnprintf
	__stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 obj/*.o -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
llvm-nm -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt; : > out/stubs.s
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in lx_os_*|qmi_*|hakios_qmi_*|section\$*) continue ;; esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
$CC -c out/stubs.s -o out/stubs.o
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/ath10k.o 2> out/ld.err || { cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-ath10k.sh: out/ath10k.o: $(llvm-size out/ath10k.o | awk 'NR == 2 { print $1 + $2 }') bytes; stubs: $(wc -l < out/stubs.txt): $(tr '\n' ' ' < out/stubs.txt)"
