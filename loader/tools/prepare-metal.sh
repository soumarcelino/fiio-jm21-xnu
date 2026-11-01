#!/usr/bin/env bash
# Experimental Maps-only Metal; Home continues to use EAGL.
set -euo pipefail
kernel_tree=${JM21_KERNEL_TREE:-$(cd .. && pwd)}
source "$kernel_tree/xnu-env.sh"
sdk=$THEOS/sdks/iPhoneOS16.5.sdk
vectorkit=${JM21_SYSTEM_TREE:?set JM21_SYSTEM_TREE}/rootfs/System/Library/PrivateFrameworks/VectorKit.framework
test -s "$vectorkit/default.metallib"
mkdir -p out/rootfs/redwood/Metal/VectorKit.framework
mkdir -p out/rootfs/redwood/Metal/VectorKit.framework/metal_libraries
install -m644 "$vectorkit/default.metallib" out/rootfs/redwood/Metal/VectorKit.framework/
install -m644 "$vectorkit/metal_libraries/AlloyCommonLibrary.metallib" out/rootfs/redwood/Metal/VectorKit.framework/metal_libraries/
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -fblocks -framework Foundation -o out/rootfs/bin/appctl src/appctl.m
python3 tools/sign-development.py out/rootfs/bin/appctl src/appctl.entitlements
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -dynamiclib -framework Foundation -o out/rootfs/redwood/lib/libhakios.maps-diagnostic.dylib src/maps-diagnostic.m
printf '%s\n' '/redwood/lib/libhakios.maps-diagnostic.dylib Maps' >> out/rootfs/etc/inject.conf
mesa=$kernel_tree/xnu-deps/build/mesa
build=$mesa/build-softpipe-freedreno
src=$mesa/mesa-25.0.7
cp "$kernel_tree"/tools/mesa/air/*.[ch] "$src/src/compiler/air/"
if ! rg -q 'hakios_gallium = shared_library' "$src/src/gallium/drivers/freedreno/meson.build"; then
 cat "$kernel_tree/tools/mesa/air/gallium-bridge.meson" >> "$src/src/gallium/drivers/freedreno/meson.build"
fi
PATH="$mesa/venv/bin:$PATH" ninja -C "$build" src/gallium/drivers/freedreno/libHakiosGallium.dylib src/gallium/drivers/freedreno/air_run
install -m755 "$build/src/gallium/drivers/freedreno/libHakiosGallium.dylib" out/home/lib/
install_name_tool -id /redwood/lib/libHakiosGallium.dylib out/home/lib/libHakiosGallium.dylib
install_name_tool -change /System/Library/Frameworks/IOKit.framework/Versions/A/IOKit /System/Library/Frameworks/IOKit.framework/IOKit out/home/lib/libHakiosGallium.dylib
install -m755 out/home/lib/libHakiosGallium.dylib out/rootfs/redwood/lib/
mkdir -p out/rootfs/redwood/Metal/HakiosMetal.bundle
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -fblocks -DHAKIOS_METAL_GALLIUM -bundle \
 -framework Foundation -framework Metal -framework QuartzCore -framework IOSurface -lobjc \
 out/home/lib/libHakiosGallium.dylib -o out/rootfs/redwood/Metal/HakiosMetal.bundle/HakiosMetal \
 "$kernel_tree/userland/src/HakiosMetal.m" "$kernel_tree/userland/src/hakios_metallib.c"
python3 - <<'PY'
import plistlib
from pathlib import Path
p=Path('out/rootfs/redwood/Metal/HakiosMetal.bundle/Info.plist')
p.write_bytes(plistlib.dumps(dict(CFBundleExecutable='HakiosMetal',CFBundleIdentifier='org.hakios.Metal',CFBundleName='HakiosMetal',CFBundlePackageType='BNDL',CFBundleVersion='1',NSPrincipalClass='HakiosMetalDevice')))
Path('out/rootfs/redwood/Metal/HakiosMetal.clients').write_text('Maps\nmetal_test\n')
Path('out/rootfs/redwood/Metal/HakiosMetal.bundle/HakiosMetal.clients').write_text('Maps\nmetal_test\n')
PY
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -fblocks -framework Foundation -framework Metal \
 -framework CoreImage -framework CoreGraphics -framework OpenGLES -framework IOKit -lMobileGestalt \
 -o out/rootfs/bin/metal_test "$kernel_tree/userland/src/metal_test.m"
install -m755 "$build/src/gallium/drivers/freedreno/air_run" out/rootfs/bin/
install_name_tool -change /System/Library/Frameworks/IOKit.framework/Versions/A/IOKit /System/Library/Frameworks/IOKit.framework/IOKit out/rootfs/bin/air_run
