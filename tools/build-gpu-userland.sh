#!/bin/bash
# GPU user space for the iOS 16.7.16 root (runs on macOS with Xcode; CI: .github/workflows/userland.yml):
#   Mesa 25.0.7 (freedreno + softpipe, OSMesa)        userland/mesa/mk-mesa-mac.sh
#   libtopaz-gl.dylib     OpenGL ES behind EAGLContext  userland/gpu/eagl.c + gl_osmesa.c
#   libtopaz-inject.dylib  loads it into backboardd     userland/gpu/topaz_inject.c
#   backboardd             stand-in that sets DYLD_INSERT_LIBRARIES and execs
#                          /usr/libexec/real/backboardd userland/gpu/bbd_wrap.c
# Output: userland/out/ with the root layout (topaz/lib/..., usr/libexec/backboardd) and SHA256SUMS.
# Everything is ad-hoc signed: the kernel's AMFI stand-in trusts any cdhash, dyld wants a signature.
# Based on HackiOS user space (MIT, OOSEX GANG): mkeagl.py, eagl_slots.txt, the build steps of its
# userland/build.sh (GL=1). See CREDITS.md.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
U=$ROOT/userland
OUT=$U/out
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
DYN=("$CC" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall)

# 1. Mesa (fetches the release tarball, applies userland/mesa/*.patch, adds redwood-drm)
mkdir -p "$U/mesa/build-mesa"
# mk-mesa-mac.sh (from upstream's private tree) wants its patches and redwood-drm in mesa/ beside it
ln -sfn . "$U/mesa/mesa"
bash "$U/mesa/mk-mesa-mac.sh"
M=$U/mesa/build-mesa/out
ls -l "$M/libOSMesa.8.dylib" "$M/gl.xml" "$M/exported-gl.txt"

# 2. libtopaz-gl.dylib: the EAGL table generated from the slots and Mesa's exports
rm -rf "$OUT" "$U/obj"
mkdir -p "$OUT/topaz/lib" "$OUT/usr/libexec" "$U/obj"
python3 "$U/gpu/mkeagl.py" "$U/gpu/eagl_slots.txt" "$M/gl.xml" "$M/exported-gl.txt" "$U/gpu/eagl.c" > "$U/obj/eagl_gen.h"
"${DYN[@]}" -dynamiclib -install_name /topaz/lib/libtopaz-gl.dylib -I"$U/obj" -I"$U/gpu" \
	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc "$M/libOSMesa.8.dylib" \
	-o "$OUT/topaz/lib/libtopaz-gl.dylib" "$U/gpu/eagl.c" "$U/gpu/gl_osmesa.c"
cp "$M/libOSMesa.8.dylib" "$OUT/topaz/lib/"

# 3. the loader inside backboardd, and the backboardd stand-in
"${DYN[@]}" -dynamiclib -install_name /topaz/lib/libtopaz-inject.dylib -I"$U/gpu" \
	-framework CoreFoundation -framework IOKit -lobjc -o "$OUT/topaz/lib/libtopaz-inject.dylib" "$U/gpu/topaz_inject.c"
"${DYN[@]}" -o "$OUT/usr/libexec/backboardd" "$U/gpu/bbd_wrap.c"

# 4. ad-hoc signatures
for f in "$OUT/topaz/lib/"*.dylib "$OUT/usr/libexec/backboardd"; do
	codesign -f -s - "$f"
done
(cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
cat "$OUT/SHA256SUMS"
