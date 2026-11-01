#!/usr/bin/env bash
# Build only the adapters needed by Apple's Home; Apple jobs stay in ignored out/.
set -euo pipefail
cd "$(dirname "$0")/.."
kernel_tree=${JM21_KERNEL_TREE:-$(cd .. && pwd)}
system_tree=${JM21_SYSTEM_TREE:?set JM21_SYSTEM_TREE}
source "$kernel_tree/xnu-env.sh"
mkdir -p out/home/lib out/rootfs/redwood/lib
sdk=${THEOS}/sdks/iPhoneOS16.5.sdk
# init binds this directory over /System/Library/xpc. Preserve the extension
# definitions used by LaunchServices/ExtensionKit; omit only launchd.plist.
mkdir -p out/rootfs/redwood/xpc
for name in extensions.plist extensions.plist.sig; do
 install -m 644 "$system_tree/rootfs/System/Library/xpc/$name" "out/rootfs/redwood/xpc/$name"
done
for adapter in inject inject_sb inject_tweaks; do
 case "$adapter" in inject) name=libhakios;; inject_sb) name=libhakios.sb;; inject_tweaks) name=libhakios.tweaks;; esac
 extra_sources=()
 if [ "$adapter" = inject_sb ]; then extra_sources=(src/native-app-trust.m -framework Foundation); fi
 xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib \
  -install_name "/redwood/lib/$name.dylib" -framework CoreFoundation -framework IOKit -lobjc \
  -o "out/home/lib/$name.dylib" "$kernel_tree/userland/src/$adapter.c" "${extra_sources[@]}"
done
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -o out/home/rwtouch \
 "$kernel_tree/userland/src/rwtouch.c"
install -m 755 out/home/lib/*.dylib out/rootfs/redwood/lib/
install -m 755 out/home/rwtouch out/rootfs/bin/rwtouch
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib \
 -install_name /redwood/lib/libhakios.search.dylib -framework Foundation -lobjc \
 -o out/home/lib/libhakios.search.dylib tools/search-model-fallback.m
install -m 755 out/home/lib/libhakios.search.dylib out/rootfs/redwood/lib/
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -fblocks -dynamiclib \
 -install_name /redwood/lib/libhakios.analytics-path.dylib -framework Foundation -lobjc \
 -o out/home/lib/libhakios.analytics-path.dylib tools/analytics-path-fallback.m
install -m 755 out/home/lib/libhakios.analytics-path.dylib out/rootfs/redwood/lib/
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -o out/home/vol_ls \
 "$kernel_tree/userland/src/vol_ls.c"
install -m 755 out/home/vol_ls out/rootfs/bin/vol_ls
# Existing read-only MobileGestalt diagnostic for capacity verification.
sed 's/char line\[256\]/char line[4096]/' "$kernel_tree/userland/src/mg_test.c" | \
 xnu-clang -x c -arch arm64 -isysroot "$sdk" -O1 -framework CoreFoundation -lMobileGestalt \
 -o out/home/mg-query -
install -m 755 out/home/mg-query out/rootfs/bin/mg-query
if [ "${JM21_DAS_WIDGET_CACHE:-0}" != 0 ]; then
 xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib \
  -install_name /redwood/lib/libhakios.dasd-widget-cache.dylib -framework Foundation -lobjc \
  -o out/home/lib/libhakios.dasd-widget-cache.dylib tools/dasd-widget-cache.m
 install -m 755 out/home/lib/libhakios.dasd-widget-cache.dylib out/rootfs/redwood/lib/
fi
if [ "${JM21_BIOME_DIAG:-0}" = 1 ]; then
 xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib \
  -install_name /redwood/lib/libhakios.coreduet-diag.dylib -framework Foundation -lobjc \
  -o out/home/lib/libhakios.coreduet-diag.dylib tools/coreduet-frame-diagnostic.m
 install -m 755 out/home/lib/libhakios.coreduet-diag.dylib out/rootfs/redwood/lib/
fi
# JM21_MEDIA=1 restores VideoToolbox's HEIC decoder service for the stock
# wallpaper. The existing media adapter skips unsupported portrait prewarming;
# enabling the daemon without that adapter used to cause a crash loop.
if [ "${JM21_MEDIA:-0}" = 1 ]; then
 xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib \
  -install_name /redwood/lib/libhakios.media.dylib -framework CoreFoundation \
  -framework CoreMedia -framework CoreVideo -framework IOSurface -lobjc \
  -o out/home/lib/libhakios.media.dylib "$kernel_tree/userland/src/inject_media.c" \
  "$kernel_tree/userland/src/camera_device.c" "$kernel_tree/userland/src/camera_isp.c"
 install -m 755 out/home/lib/libhakios.media.dylib out/rootfs/redwood/lib/
fi
# Provider crash loops need the actual exception, not only corpse notifications.
# This adapter logs only uncaught exceptions and preserves stock termination.
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib \
 -install_name /redwood/lib/libhakios.wallpaper-exception.dylib -framework Foundation \
 -o out/home/lib/libhakios.wallpaper-exception.dylib tools/wallpaper-exception-diagnostic.m
install -m 755 out/home/lib/libhakios.wallpaper-exception.dylib out/rootfs/redwood/lib/
# The preview adapter handles this device's lack of a Metal command queue.
# Extra logging and the read-only discovery tool remain explicitly opt-in.
poster_diag_flags=()
if [ "${JM21_POSTER_DIAG:-0}" = 1 ]; then poster_diag_flags=(-DJM21_POSTER_DIAG=1); fi
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib "${poster_diag_flags[@]}" \
 -install_name /redwood/lib/libhakios.poster.dylib -framework Foundation \
 -framework UIKit -framework CoreGraphics -lobjc \
 -o out/home/lib/libhakios.poster.dylib tools/poster-diagnostic.c
install -m 755 out/home/lib/libhakios.poster.dylib out/rootfs/redwood/lib/
if [ "${JM21_POSTER_DIAG:-0}" = 1 ]; then
 xnu-clang -arch arm64 -isysroot "$sdk" -O1 -framework Foundation -lobjc \
  -o out/home/extension-diagnostic tools/extension-diagnostic.c
 install -m 755 out/home/extension-diagnostic out/rootfs/bin/extension-diagnostic
fi
# Opt-in A610: use the existing CoreAnimation/Mesa adapter and local vendor firmware.
if [ "${JM21_A610:-0}" = 1 ]; then
 mesa="$kernel_tree/xnu-deps/build/mesa/out"
 firmware=${JM21_GPU_FIRMWARE:-${JM21_FIRMWARE_DIR:?set JM21_FIRMWARE_DIR}/filesystems/vendor/firmware}
 mkdir -p out/home/gl out/rootfs/fw
 python3 "$kernel_tree/userland/mkeagl.py" "$kernel_tree/userland/src/eagl_slots.txt" \
  "$mesa/gl.xml" "$mesa/exported-gl.txt" "$kernel_tree/userland/src/eagl.c" > out/home/gl/eagl_gen.h
 xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib \
  -install_name /redwood/lib/libhakios.gl.dylib -Iout/home/gl -framework CoreFoundation \
  -framework IOKit -framework IOSurface -lobjc "$mesa/libOSMesa.8.dylib" \
  -o out/home/lib/libhakios.gl.dylib "$kernel_tree/userland/src/eagl.c" "$kernel_tree/userland/src/gl_osmesa.c"
 install -m 755 out/home/lib/libhakios.gl.dylib "$mesa/libOSMesa.8.dylib" out/rootfs/redwood/lib/
 for name in a630_sqe.fw a610_zap.mdt a610_zap.b02; do
  install -m 644 "$firmware/$name" out/rootfs/fw/
 done
fi
# Opt-in Wi-Fi (JM21_WIFI=1): MSS image, PD JSON, wlanmdsp.mbn (served by TFTP) and the BDF, local only.
if [ "${JM21_WIFI:-0}" != 0 ]; then
 modem=${JM21_MODEM_FIRMWARE:-${JM21_FIRMWARE_DIR:?set JM21_FIRMWARE_DIR}/filesystems/modem/image}
 mkdir -p out/rootfs/fw
 for f in "$modem"/modem.mdt "$modem"/modem.b[0-9][0-9] "$modem"/*.jsn "$modem"/wlanmdsp.mbn "$modem"/bdwlan.bin; do
  install -m 644 "$f" out/rootfs/fw/
 done
 install -m 644 "${JM21_FIRMWARE_DIR:?set JM21_FIRMWARE_DIR}/filesystems/vendor/firmware/fiio/bdwlans.bin" out/rootfs/fw/bdwlans.bin
 # ath10k's firmware-5.bin for a QMI (snoc) device: only feature IEs (written here; no Qualcomm code):
 # FW_VERSION, FEATURES {WOWLAN 6, MGMT_TX_BY_REF 18, NON_BMI 19, SINGLE_CHAN_INFO_PER_CHANNEL 20}, WMI op TLV(4), HTT op TLV(3)
 python3 - out/rootfs/fw/ath10k-firmware-5.bin <<'PYFW'
import struct,sys
def ie(i,d): return struct.pack('<II',i,len(d))+d+bytes((-len(d))%4)
feat=0
for b in (6,18,19,20): feat|=1<<b
out=b'QCA-ATH10K\0'; out+=bytes((-len(out))%4)
out+=ie(0,b'JM21-WCN3990-QMI')+ie(2,struct.pack('<Q',feat))+ie(5,struct.pack('<I',4))+ie(6,struct.pack('<I',3))
open(sys.argv[1],'wb').write(out)
PYFW
 # net_test (kernel tree userland): "dhcp en0", "waitaddr en0 S", "show" through configd, for the USB shell
 xnu-clang -arch arm64 -isysroot "$sdk" -O1 -fobjc-arc -framework Foundation -framework SystemConfiguration \
  -o out/home/net_test "$kernel_tree/userland/src/net_test.m" && install -m 755 out/home/net_test out/rootfs/bin/net_test
fi
# JM21_PERSIST=1: /private/var kept between boots by hakios' var_store, raw on userdata
# (/dev/rmmcroot) at 8 GiB, two 2 GiB slots, past the 2.3 GiB iOS container (docs/jm21-persistence-plan.md).
if [ "${JM21_PERSIST-}" = 1 ] || [ "${JM21_DATA_HFS-}" = 1 ]; then
 mini=$kernel_tree/userland/libmini
 cc=(xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin)
 "${cc[@]}" -I"$mini" -c "$mini/mini.c" -o out/home/mini.o
 "${cc[@]}" -c "$mini/start.S" -o out/home/start.o
 "${cc[@]}" -I"$mini" -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
  -DDEVICE='"/dev/rmmcroot"' -DSTORE_AT=8589934592LL \
  -o out/home/var_store "$kernel_tree/userland/src/var_store.c" out/home/mini.o out/home/start.o
 install -m 755 out/home/var_store out/rootfs/bin/var_store
 "${cc[@]}" -I"$mini" -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
  -o out/home/data-volume src/data-volume.c out/home/mini.o out/home/start.o
 install -m 755 out/home/data-volume out/rootfs/bin/data-volume
fi
# The JM21 exposes its radio through HakiosWLAN, not Apple's device-tree nodes
# wlan/marconi-wifi. --rest preserves wifid's hardware load restriction, so
# ALLOW=wifi alone leaves the daemon unavailable to Settings/MobileWiFi.
# Explicit jobs have that restriction removed by mkjobs.py and retain Apple's
# normal RunAtLoad/KeepAlive policy so connection state reaches the interface.
wifi_jobs=()
if [ "${JM21_WIFI:-0}" = 2 ]; then
 wifi_jobs+=(com.apple.wifid)
fi
ALLOW="${ALLOW:+$ALLOW }duetexpertd mobileassetd" OSLOG="${JM21_OSLOG-com.apple.backboardd}" python3 "$kernel_tree/userland/mkjobs.py" --rest \
 "$system_tree/rootfs" out/rootfs/redwood/LaunchDaemons \
 com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system \
 com.apple.logd com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd \
 com.apple.fseventsd com.apple.containermanagerd com.apple.containermanagerd.system \
 com.apple.runningboardd com.apple.backboardd com.apple.mobile.usermanagerd \
 com.apple.mobilegestalt.xpc com.apple.lsd com.apple.powerd com.apple.analyticsd \
 com.apple.logd_helper com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd \
 com.apple.SpringBoard "${wifi_jobs[@]}"
# JM21 has no cellular/voicemail backend. vmd otherwise starts on demand from
# Phone and consumes approximately one CPU continuously (runtime audit 2026-10-31).
# Omit only this unsupported service, independently of the general boot trim.
# JM21_VOICEMAIL=1 restores the stock job for backend bring-up/diagnostics.
if [ "${JM21_VOICEMAIL:-0}" != 1 ]; then
 rm -f out/rootfs/redwood/LaunchDaemons/com.apple.voicemail.vmd.plist
fi
# JM21_TRIM=1: drop jobs that only abort or crash-loop during boot (docs/jm21-boot-speed.md).
# JM21_TRIM_JOBS overrides the list, to try one at a time.
if [ "${JM21_TRIM-}" = 1 ]; then
 for label in ${JM21_TRIM_JOBS-com.apple.activityawardsd com.apple.mediaserverd}; do
  if [ "$label" = com.apple.mediaserverd ] && [ "${JM21_MEDIA:-0}" = 1 ]; then
   continue
  fi
  rm -fv "out/rootfs/redwood/LaunchDaemons/$label.plist"
 done
fi
# PID 1 installs the kernel's supported insertion table before Apple launchd starts.
cat > out/rootfs/etc/inject.conf <<'EOF'
/redwood/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libhakios.sb.dylib SpringBoard
EOF
if [ "${JM21_MEDIA:-0}" = 1 ]; then
 printf '%s\n' '/redwood/lib/libhakios.media.dylib mediaserverd' >> out/rootfs/etc/inject.conf
fi
printf '%s\n' '/redwood/lib/libhakios.poster.dylib PosterBoard' >> out/rootfs/etc/inject.conf
printf '%s\n' '/redwood/lib/libhakios.wallpaper-exception.dylib AegirPoster WeatherPoster' >> out/rootfs/etc/inject.conf
printf '%s\n' '/redwood/lib/libhakios.search.dylib searchd Spotlight' >> out/rootfs/etc/inject.conf
printf '%s\n' '/redwood/lib/libhakios.analytics-path.dylib analyticsd' >> out/rootfs/etc/inject.conf
if [ "${JM21_DAS_WIDGET_CACHE:-0}" != 0 ]; then
 printf '%s\n' '/redwood/lib/libhakios.dasd-widget-cache.dylib dasd' >> out/rootfs/etc/inject.conf
fi
if [ "${JM21_BIOME_DIAG:-0}" = 1 ]; then
 printf '%s\n' '/redwood/lib/libhakios.coreduet-diag.dylib coreduetd' >> out/rootfs/etc/inject.conf
fi
# Keep the environment explicit for unrestricted diagnostic launches too.
python3 - <<'PY'
from pathlib import Path
import os
import plistlib
jobs=Path('out/rootfs/redwood/LaunchDaemons')
for path in jobs.glob('*.plist'):
 job=plistlib.loads(path.read_bytes())
 # Keep daemon diagnostics readable over USB without the scrolling framebuffer.
 job['StandardErrorPath']=f'/private/var/tmp/{path.stem}.log'
 program=Path(job.get('Program') or job.get('ProgramArguments',[''])[0]).name
 lib={'backboardd':'libhakios','containermanagerd':'libhakios',
      'containermanagerd_system':'libhakios','SpringBoard':'libhakios.sb',
      'PosterBoard':'libhakios.poster',
      'searchd':'libhakios.search','Spotlight':'libhakios.search',
      'analyticsd':'libhakios.analytics-path',
      'mediaserverd':'libhakios.media' if os.environ.get('JM21_MEDIA') == '1' else None}.get(program)
 if lib:
  job.setdefault('EnvironmentVariables',{})['DYLD_INSERT_LIBRARIES']=f'/redwood/lib/{lib}.dylib'
  # Audio routing emits extensive diagnostics; keep them opt-in on the USB port.
  if program == 'mediaserverd' and os.environ.get('JM21_MEDIA_LOG') == '1':
   job['EnvironmentVariables']['OS_ACTIVITY_DT_MODE']='YES'
 if program == 'dasd' and os.environ.get('JM21_DAS_WIDGET_CACHE','0') != '0':
  job.setdefault('EnvironmentVariables',{}).update({
   'DYLD_INSERT_LIBRARIES':'/redwood/lib/libhakios.dasd-widget-cache.dylib',
   'JM21_DAS_WIDGET_CACHE':os.environ['JM21_DAS_WIDGET_CACHE']})
 if program == 'coreduetd' and os.environ.get('JM21_BIOME_DIAG') == '1':
  job.setdefault('EnvironmentVariables',{}).update({
   'DYLD_INSERT_LIBRARIES':'/redwood/lib/libhakios.coreduet-diag.dylib',
   'JM21_BIOME_ENUM_DIAG':'1'})
 # Discovery runs in pkd/lsd as well as the client; these daemons already have
 # per-job stderr files, but their os_log output is normally disabled.
 if program in ('PosterBoard', 'pkd', 'lsd', 'duetexpertd') and os.environ.get('JM21_POSTER_DIAG') == '1':
  job.setdefault('EnvironmentVariables',{})['OS_ACTIVITY_DT_MODE']='YES'
 path.write_bytes(plistlib.dumps(job,fmt=plistlib.FMT_BINARY))
PY
if [ "${JM21_PERSIST-}" = 1 ] && [ "${JM21_DATA_HFS-}" != 1 ]; then
python3 - <<'PY'
import os
import plistlib
job={'Label':'com.fiio.jm21.varstore','RunAtLoad':True,
     'ProgramArguments':['/private/preboot/bin/var_store','watch','300'],
     'StandardOutPath':'/private/var/tmp/varstore.log','StandardErrorPath':'/private/var/tmp/varstore.log'}
# A concurrent save previously doubled composition/copy cost. Let the watcher
# yield CPU to interaction without changing save cadence or persistence format.
# LowPriorityIO is advisory: ramfs and raw MMC do not implement disk throttling.
if os.environ.get('JM21_BACKGROUND_SAVE') == '1':
 job.update({'ProcessType':'Background','Nice':20,'LowPriorityIO':True})
open('out/rootfs/redwood/LaunchDaemons/com.fiio.jm21.varstore.plist','wb').write(plistlib.dumps(job,fmt=plistlib.FMT_BINARY))
PY
fi
# Eureka must not race the iOS compositor at boot.
rm out/rootfs/redwood/LaunchDaemons/com.fiio.jm21.eureka.plist
if [ ! -f out/iphone-d22.adt ]; then
 python3 "$system_tree/fetch-from-ipsw.py" Firmware/all_flash/DeviceTree.d22ap.im4p out/iphone-d22.im4p
 python3 tools/adt.py unwrap out/iphone-d22.im4p out/iphone-d22.adt
fi

if [ "${JM21_METAL:-0}" = 1 ]; then bash tools/prepare-metal.sh; fi
