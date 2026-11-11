#!/bin/bash
# Boot a composed system in QEMU, wait until it has printed N
# "TEST PASSED"/"TEST FAILED" lines (or times out), then stop QEMU.
#
#   tools/cosfs/run_qemu_test.sh <build-name> <log> [-n N] [-t SECONDS] [-d DISK_IMAGE]
#
# Exit status: 0 if exactly N "TEST PASSED" lines were seen, 1 otherwise.
# Run from the repository root.

set -u
name=$1
log=$2
shift 2
n=1
tmo=600
disk=""
while [ $# -gt 0 ]; do
	case $1 in
	-n) n=$2; shift 2 ;;
	-t) tmo=$2; shift 2 ;;
	-d) disk=$2; shift 2 ;;
	*) echo "unknown option $1"; exit 2 ;;
	esac
done

iso=system_binaries/cos_build-${name}/cos.iso
[ -f "$iso" ] || { echo "no $iso: compose first"; exit 2; }
drive=()
[ -n "$disk" ] && drive=(-drive "file=${disk},if=virtio,format=raw")

qemu-system-x86_64 -cpu max -smp 8 -m 8192 -cdrom "$iso" -no-reboot -nographic -nic none "${drive[@]}" \
	> "$log" 2>&1 &
qpid=$!

end=$((SECONDS + tmo))
while kill -0 $qpid 2> /dev/null && [ $SECONDS -lt $end ]; do
	done_n=$(grep -a -c -E "TEST (PASSED|FAILED)" "$log")
	[ "$done_n" -ge "$n" ] && break
	sleep 2
done
# give the last writes a moment, then stop QEMU cleanly (SIGTERM flushes the disk image)
sleep 1
kill $qpid 2> /dev/null
wait $qpid 2> /dev/null

passed=$(grep -a -c "TEST PASSED" "$log")
failed=$(grep -a -c "TEST FAILED" "$log")
grep -a -E "SUCCESS|FAILURE|TEST (PASSED|FAILED)" "$log"
if [ "$passed" -eq "$n" ] && [ "$failed" -eq 0 ]; then
	exit 0
fi
echo "run_qemu_test: $name: $passed passed, $failed failed, $n expected (see $log)"
exit 1
