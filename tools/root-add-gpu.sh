#!/bin/bash
# Put the GPU user space (tools/build-gpu-userland.sh output, the CI artifact "gpu-userland")
# into an iOS root image and repack it for fastboot:
#
#   tools/root-add-gpu.sh ROOT_IMG GPU_DIR        (asks for sudo)
#
#   ROOT_IMG  raw HFS+ image from tools/vayu-build-root.sh (e.g. ~/ipsw-x/ios16-root.img)
#   GPU_DIR   unpacked artifact: topaz/lib/*.dylib, usr/libexec/backboardd, SHA256SUMS
#
# On the root: /usr/libexec/backboardd moves to /usr/libexec/real/backboardd (once) and the stand-in
# takes its place; /topaz/lib gets libOSMesa.8.dylib, libtopaz-gl.dylib, libtopaz-inject.dylib.
# The stand-in loads the GPU code only when the files are there; boot-arg gl=0 keeps the software
# renderer (topaz_inject.c). Output: ROOT_IMG with the files, and ROOT_IMG's .simg next to it.
# Flashing that simg rewrites userdata: the iOS data on the phone starts over.
set -euo pipefail
img=$(realpath "${1:?usage: root-add-gpu.sh ROOT_IMG GPU_DIR}")
gpu=$(realpath "${2:?usage: root-add-gpu.sh ROOT_IMG GPU_DIR}")
for f in topaz/lib/libOSMesa.8.dylib topaz/lib/libtopaz-gl.dylib topaz/lib/libtopaz-inject.dylib usr/libexec/backboardd SHA256SUMS; do
	[ -e "$gpu/$f" ] || { echo "missing $gpu/$f" >&2; exit 1; }
done
(cd "$gpu" && shasum -a 256 -c --quiet SHA256SUMS 2>/dev/null || sha256sum -c --quiet SHA256SUMS)
mnt=$(mktemp -d)
sudo mount -t hfsplus -o loop,force "$img" "$mnt"
trap 'sudo umount "$mnt"; rmdir "$mnt"' EXIT
if [ ! -e "$mnt/usr/libexec/real/backboardd" ]; then
	sudo mkdir -p "$mnt/usr/libexec/real"
	sudo mv "$mnt/usr/libexec/backboardd" "$mnt/usr/libexec/real/backboardd"
	echo "backboardd -> usr/libexec/real/backboardd"
fi
sudo install -m 0755 "$gpu/usr/libexec/backboardd" "$mnt/usr/libexec/backboardd"
sudo mkdir -p "$mnt/topaz/lib"
for f in libOSMesa.8.dylib libtopaz-gl.dylib libtopaz-inject.dylib; do
	sudo install -m 0755 "$gpu/topaz/lib/$f" "$mnt/topaz/lib/$f"
done
ls -l "$mnt/usr/libexec/backboardd" "$mnt/usr/libexec/real/backboardd" "$mnt/topaz/lib"
sudo umount "$mnt"
rmdir "$mnt"
trap - EXIT
fsck.hfsplus -f -y "$img" | tail -n 2 || true
img2simg "$img" "${img%.img}.simg"
ls -l "$img" "${img%.img}.simg"
echo "done: flash with  fastboot -S 512M flash userdata ${img%.img}.simg  (the iOS data starts over)"
