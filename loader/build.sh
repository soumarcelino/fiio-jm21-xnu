#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
python3 - <<'PY'
from pathlib import Path
import time
Path('out/build-epoch.h').write_text(f'#define JM21_BUILD_EPOCH {int(time.time())}L\n')
PY
python3 - <<'PY'
from PIL import Image,ImageDraw,ImageFont
from pathlib import Path
f=ImageFont.load_default(size=12)
rows=[]
for c in range(32,127):
 im=Image.new('1',(8,16)); ImageDraw.Draw(im).text((0,0),chr(c),font=f,fill=1)
 rows.append('{'+','.join(str(sum((128>>x) for x in range(8) if im.getpixel((x,y)))) for y in range(16))+'}')
Path('out/font.h').write_text('/* Rasterized Pillow built-in Aileron font (OFL). */\nstatic const unsigned char font[95][16]={'+','.join(rows)+'};\n')
PY
python3 tools/mkeureka.py
userflags=(-Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align -Wall -Wextra)
# Static Darwin programs for the APFS root (md0): PID 1 bridge, shell, multi-call tools.
aarch64-linux-gnu-gcc "${userflags[@]}" -c src/user.S -o out/user.o
for prog in init sh tools eureka; do
 aarch64-linux-gnu-gcc "${userflags[@]}" -Iout -c src/$prog.c -o out/$prog.o
 aarch64-linux-gnu-ld -T src/init.ld --build-id=none out/user.o out/$prog.o -o out/$prog.elf
 aarch64-linux-gnu-objcopy -O binary --only-section=.text out/$prog.elf out/$prog.bin
 python3 tools/mkinit.py out/$prog.bin out/$prog --raw
done
rm -rf out/rootfs && mkdir -p out/rootfs/sbin out/rootfs/bin out/rootfs/dev out/rootfs/tmp out/rootfs/mnt
install -m 755 out/init out/rootfs/sbin/launchd
install -m 755 out/init out/rootfs/sbin/jm21-bridge
# jm21_ios=1: /redwood is bound on the iOS volume's /redwood, where its LaunchDaemons symlink points
mkdir -p out/rootfs/etc out/rootfs/redwood/LaunchDaemons out/rootfs/redwood/xpc out/rootfs/empty out/rootfs/Cryptexes/OS
install -m 644 "${JM21_SYSTEM_TREE:?set JM21_SYSTEM_TREE}/var.skel" out/rootfs/etc/var.skel
install -m 644 LaunchDaemons/*.plist out/rootfs/redwood/LaunchDaemons/
install -m 755 out/sh out/rootfs/bin/sh
install -m 755 out/eureka out/rootfs/bin/eureka
for tool in ls cat echo uname id sysctl ps kill sleep env pwd mkdir xd mount chroot true false poweroff; do install -m 755 out/tools out/rootfs/bin/$tool; done
if [ "${JM21_HOME:-0}" = 1 ]; then bash tools/prepare-home.sh; fi
python3 tools/mkapfs.py -o out/root.apfs root=out/rootfs | tail -1
aarch64-linux-gnu-gcc -c src/shell-cache.S -o out/shell-cache.o
aarch64-linux-gnu-gcc "${userflags[@]}" -c src/shell-vfs.c -o out/shell-vfs.o
python3 tools/prepare.py
flags=(-O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -mgeneral-regs-only -mstrict-align -Iout)
if [ "${JM21_SILENT:-0}" = 1 ]; then flags+=(-DJM21_SILENT=1); fi
for file in start.S main.c loader.c usb.c usb-probe.S; do
 aarch64-linux-gnu-gcc "${flags[@]}" -c "src/$file" -o "out/${file%.*}.o"
done
if aarch64-linux-gnu-readelf -r out/main.o out/loader.o out/usb.o out/usb-probe.o | rg -q 'R_AARCH64_ABS'; then
 echo 'Unexpected absolute address relocation' >&2; exit 1
fi
aarch64-linux-gnu-ld -T src/link.ld --build-id=none out/start.o out/main.o out/loader.o out/usb.o out/usb-probe.o -o out/shim.elf
aarch64-linux-gnu-objcopy -O binary out/shim.elf out/Image
python3 - <<'PY'
import struct,gzip
from pathlib import Path
p=Path('out/Image'); b=p.read_bytes(); size=struct.unpack_from('<Q',b,16)[0]
b+=bytes(size-len(b)); b+=bytes((-len(b))%4096); b+=Path('out/payload.bin').read_bytes(); struct.pack_into('<Q',b:=bytearray(b),16,len(b)); p.write_bytes(b)
name=b'TRAILER!!!\0'; header=b'070701'+b''.join(f'{v:08x}'.encode() for v in [0,0,0,0,1,0,0,0,0,0,0,len(name),0])
c=header+name; c+=bytes((-len(c))%4)
Path('out/ramdisk.gz').write_bytes(gzip.compress(c,mtime=0))
PY
mkbootimg --header_version 4 --kernel out/Image --ramdisk out/ramdisk.gz --cmdline 'console=null' -o out/shim-boot.img
(cd out && sha256sum Image shim-boot.img > SHA256SUMS)
ls -lh out/Image out/shim-boot.img
