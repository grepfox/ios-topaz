#!/bin/bash
# Build the iOS 16.7.16 (iPhone X, 20H392) root for vayu's userdata in one go:
# tools/mkiosroot.sh -> tools/sweet-root-tweaks.py -> sparse image for fastboot.
#
#   tools/vayu-build-root.sh IPSW_DIR [--reuse]    (asks for sudo several times)
#
# --reuse: keep an existing IPSW_DIR/ios16-root.img from an earlier run whose
# copy finished (mkiosroot.sh printed its df line) and continue from the tweaks.
#
# IPSW_DIR holds the three disk images of the D22AP Erase identity
# (BuildManifest.plist): OS 087-86698-044.dmg, SystemOS 087-86593-047.dmg,
# AppOS 087-86715-047.dmg. Output: IPSW_DIR/ios16-root.img (16 GiB raw) and
# IPSW_DIR/ios16-root.simg (for `fastboot flash userdata`).
# apfs-fuse may live in ~/src/apfs-fuse/build (not packaged by Ubuntu).
set -euo pipefail
dir=$(realpath "${1:?usage: vayu-build-root.sh IPSW_DIR}")
repo=$(cd "$(dirname "$0")/.." && pwd)
export PATH="$HOME/src/apfs-fuse/build:$PATH"
for t in apfs-fuse mkfs.hfsplus fsck.hfsplus rsync img2simg; do
	command -v "$t" >/dev/null || { echo "missing: $t" >&2; exit 1; }
done
img=$dir/ios16-root.img
fsck_ok() {
	fsck.hfsplus -f -n "$1" && return 0
	fsck.hfsplus -f -y "$1" | grep -q 'appears to be OK'	# see mkiosroot.sh
}
if [[ ${2:-} == --reuse && -e $img ]]; then
	echo "reusing $img"
	fsck_ok "$img" || { echo "$img is not clean; rebuild without --reuse" >&2; exit 1; }
else
	[[ ! -e $img ]] || { echo "$img exists; remove it to rebuild, or pass --reuse" >&2; exit 1; }
	bash "$repo/tools/mkiosroot.sh" "$dir/087-86698-044.dmg" "$dir/087-86593-047.dmg" \
		"$dir/087-86715-047.dmg" "$img" 16G
fi

mnt=$(mktemp -d)
sudo mount -t hfsplus -o loop,force "$img" "$mnt"
sudo python3 "$repo/tools/sweet-root-tweaks.py" "$mnt" || { sudo umount "$mnt"; exit 1; }
sudo umount "$mnt"
rmdir "$mnt"
fsck.hfsplus -f -y "$img" | grep -v 'Illegal name' || true
fsck_ok "$img" || { echo "fsck: $img is not clean after the tweaks" >&2; exit 1; }

img2simg "$img" "$dir/ios16-root.simg"
sudo chown "$(id -u):$(id -g)" "$img" "$dir/ios16-root.simg" 2>/dev/null || true
ls -l "$img" "$dir/ios16-root.simg"
echo "done: flash with  fastboot -S 512M flash userdata $dir/ios16-root.simg  (ERASES Android user data)"
