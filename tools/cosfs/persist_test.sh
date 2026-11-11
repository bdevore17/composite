#!/bin/bash
# Persistence test for cosfs on virtio-blk (unit_vfs_persist).
#
#   ./cos compose composition_scripts/unit_vfs_persist.toml vfs_persist
#   tools/cosfs/persist_test.sh [build-name (default vfs_persist)] [workdir]
#
# 1. host: mkfs a 64 MiB image and put /from_host.txt into it
# 2. boot #1 (phase 1): the guest checks the host's file and writes a tree
# 3. host: fsck the image and check every file the guest wrote
# 4. boot #2 (phase 2): the guest verifies its files survived the reboot, modifies them
# 5. host: fsck and check the modified state
#
# The expected contents mirror unit_vfs_persist.c. Run from the repository root.

set -u
name=${1:-vfs_persist}
work=${2:-$(mktemp -d)}
img=$work/persist.img
tool=tools/cosfs/cosfs
fail=0

step() { echo "== $*"; }
check() {
	# check <description> <command...>
	local what=$1
	shift
	if "$@" > "$work/check.out" 2>&1; then
		echo "SUCCESS: host: $what"
	else
		echo "FAILURE: host: $what"
		cat "$work/check.out"
		fail=1
	fi
}
expect_eq() {
	# expect_eq <description> <expected> <actual>
	if [ "$2" == "$3" ]; then
		echo "SUCCESS: host: $1"
	else
		echo "FAILURE: host: $1: expected [$2], got [$3]"
		fail=1
	fi
}
hello() {
	local i
	for i in $(seq 0 49); do echo "line $i of the persistence test"; done
	[ "${1:-}" == 2 ] && echo "phase 2"
}

make -s -C tools/cosfs cosfs || exit 2

step "host: create the image"
rm -f "$img"
$tool mkfs "$img" 64 || exit 2
printf 'written by the host\n' > "$work/from_host.txt"
$tool put "$img" "$work/from_host.txt" /from_host.txt || exit 2

step "boot 1: phase 1"
check "boot 1 passed" tools/cosfs/run_qemu_test.sh "$name" "$work/boot1.log" -d "$img"

step "host: check phase 1's files"
check "fsck after boot 1" $tool fsck "$img"
check "big.bin content" $tool verify "$img" /persist/big.bin $((5 * 1024 * 1024 + 1234)) 7
expect_eq "hello.txt content" "$(hello)" "$($tool cat "$img" /persist/dir1/dir2/hello.txt)"
expect_eq "20 files" 20 "$($tool ls "$img" /persist/files | wc -l)"
expect_eq "files/f13" "file 13" "$($tool cat "$img" /persist/files/f13)"
check "renamed exists" $tool cat "$img" /persist/renamed
check "deleted is gone" bash -c "! $tool cat '$img' /persist/deleted"

step "boot 2: phase 2"
check "boot 2 passed" tools/cosfs/run_qemu_test.sh "$name" "$work/boot2.log" -d "$img"

step "host: check phase 2's changes"
check "fsck after boot 2" $tool fsck "$img"
check "big.bin truncated" $tool verify "$img" /persist/big.bin $((1024 * 1024)) 7
expect_eq "hello.txt appended" "$(hello 2)" "$($tool cat "$img" /persist/dir1/dir2/hello.txt)"
expect_eq "10 files left" 10 "$($tool ls "$img" /persist/files | wc -l)"

if [ $fail -eq 0 ]; then
	echo "PERSISTENCE TEST PASSED (logs and image in $work)"
else
	echo "PERSISTENCE TEST FAILED (logs and image in $work)"
fi
exit $fail
