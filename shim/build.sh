#!/bin/bash
# Build the shim and wrap it in a boot image.
#   shim/build.sh [PAYLOAD]                  topaz (default)
#   TARGET=vayu VAYU_DTB=.../dtb.img shim/build.sh [PAYLOAD]
#
# topaz: boot image v4. The topaz (Xiaomi) ABL ignores v1/v2 images and drops
# the v4 boot ramdisk, so the payload is appended to the shim Image.
# vayu: boot image v2 with the stock DTB blob (boot.img dtb section of the
# phone's MIUI build: ABL picks the SM8150P DTB and applies the VAYU dtbo) and
# the payload as the ramdisk, as sweet did on its Xiaomi SM7150 ABL.
# VAYU_EMBED=1 appends the payload to the Image instead (topaz style).
#
# Toolchain: aarch64-linux-gnu-* if installed, else clang + lld (LLVM_BIN).
set -euo pipefail
cd "$(dirname "$0")"
TARGET=${TARGET:-topaz}
case "$TARGET" in
topaz) TDEF= ;;
vayu) TDEF=-DTARGET_VAYU ;;
*) echo "unknown TARGET=$TARGET (topaz, vayu)" >&2; exit 1 ;;
esac
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra $TDEF"
if command -v aarch64-linux-gnu-gcc >/dev/null; then
	CC=aarch64-linux-gnu-gcc
	OBJCOPY=aarch64-linux-gnu-objcopy
	READELF=aarch64-linux-gnu-readelf
	NM=aarch64-linux-gnu-nm
	LDFLAGS="-Wl,--no-warn-rwx-segments"
else
	LLVM_BIN=${LLVM_BIN:-$(ls -d /usr/lib/llvm-*/bin 2>/dev/null | sort -V | tail -1)}
	# no switch lookup tables: their pointers would be absolute relocations
	CC="clang --target=aarch64-none-elf -fno-jump-tables"
	OBJCOPY=$LLVM_BIN/llvm-objcopy
	READELF=$LLVM_BIN/llvm-readelf
	NM=$LLVM_BIN/llvm-nm
	LDFLAGS="-fuse-ld=lld -B$LLVM_BIN"
fi
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none $LDFLAGS -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$($NM out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
empty_ramdisk() {
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
}
embed_payload() {
	# append the payload at the first 4K boundary after _end (loader.c) and grow image_size
	truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
	cat "$1" >> out/Image
	python3 -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' out/Image
}
MKBOOTIMG=${MKBOOTIMG:-python3 /home/makr/work/topaz/mkbootimg_src/mkbootimg.py}
if [ "$TARGET" = vayu ]; then
	# Ramdisk: the xnu payload (tools/mkpayload.py) if given as $1, else an
	# empty newc cpio (ABL dislikes a zero-size ramdisk) -> shim diagnostics.
	: "${VAYU_DTB:?set VAYU_DTB to the dtb of the stock boot.img (e.g. unpacked boot/dtb.img)}"
	if [ -n "${1:-}" ] && [ "${VAYU_EMBED:-0}" = 1 ]; then
		embed_payload "$1"
		empty_ramdisk
	elif [ -n "${1:-}" ]; then
		cp "$1" out/ramdisk.bin
	else
		empty_ramdisk
	fi
	# Header fields as in the stock MIUI V14.0.3.0 boot.img (os 13.0.0, 2023-05)
	$MKBOOTIMG --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
		--dtb "$VAYU_DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
		--os_version 13.0.0 --os_patch_level 2023-05 \
		--cmdline "console=null" -o out/shim-boot.img
else
	if [ -n "${1:-}" ]; then
		embed_payload "$1"
	fi
	empty_ramdisk
	$MKBOOTIMG --header_version 4 --kernel out/Image --ramdisk out/ramdisk.bin \
		--os_version 13.0.0 --os_patch_level 2025-01 \
		--cmdline "console=null" -o out/shim-boot.img
fi
# Host render of the console layout.
gcc -O1 -Wall $TDEF -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
