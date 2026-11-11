#!/bin/bash
#
# Compose a system and boot it under QEMU (TCG-compatible flags, no
# KVM required), then decide pass/fail from its console output.
#
# Usage: tools/qemu_test.sh <composition_script.toml> [timeout_seconds]
#
# Exit status: 0 iff the output contains a success marker ("TEST
# PASSED" by default) and no failure marker (FAULT, assert errors,
# BUG, kernel assertions, "TEST FAILED", "FAILURE"); 1 on failure or
# timeout; 2 on usage/compose errors. QEMU is stopped as soon as
# either marker shows up, or when the timeout expires.
#
# Tests print different success markers; set QEMU_TEST_PASS for those
# that don't print "TEST PASSED", e.g.:
#   unit_heap.toml        (default)
#   unit_memmgr.toml      QEMU_TEST_PASS="Aligned memory allocation is continguous"
#   sched.toml            QEMU_TEST_PASS="Done testing"
#   ping_pong.toml        QEMU_TEST_PASS="All main functions returned"
# kernel_test.toml has no overall marker (and its timer tests fail on
# timing bounds under TCG), so read its log instead.
#
# Environment overrides:
#   QEMU_TEST_PASS   extended regex for success (default: "TEST PASSED")
#   QEMU_TEST_FAIL   extended regex for failure
#   QEMU_TEST_NAME   build name (default: script basename)
#   QEMU_TEST_OUT    console log path (default: system_binaries/cos_build-<name>/qemu.log)
#   QEMU_TEST_MEM    guest memory in MB (default: 8192; capmgr needs it)
#   QEMU_TEST_EXTRA  extra qemu arguments (e.g. devices)
#   QEMU_TEST_NO_COMPOSE=1  boot an existing build without recomposing

set -u

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	echo "Usage: $0 <composition_script.toml> [timeout_seconds]" >&2
	exit 2
fi

script=$1
timeout_s=${2:-300}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

name=${QEMU_TEST_NAME:-$(basename "$script" .toml)}
pass_re=${QEMU_TEST_PASS:-"TEST PASSED"}
fail_re=${QEMU_TEST_FAIL:-"FAULT|assert error|BUG @|Assertion|TEST FAILED|FAILURE"}
mem=${QEMU_TEST_MEM:-8192}
build=system_binaries/cos_build-$name

# The guest must have exactly the number of CPUs the system is built for.
cfg=src/platform/i386/chal/shared/cos_config.h
ncpu=$(sed -n 's/^#define NUM_CPU_KERNEL[[:space:]]\+\([0-9]\+\).*/\1/p' "$cfg")
if [ -z "$ncpu" ]; then
	echo "qemu_test: cannot find NUM_CPU_KERNEL in $cfg" >&2
	exit 2
fi

# Components are compiled from CODE_DIR; a copied tree must point at itself.
code_dir=$(sed -n 's/^CODE_DIR=//p' src/Makefile.cosconfig 2>/dev/null)
if [ -n "$code_dir" ] && [ "$(cd "$code_dir" 2>/dev/null && pwd -P)" != "$(cd src && pwd -P)" ]; then
	echo "qemu_test: warning: src/Makefile.cosconfig has CODE_DIR=$code_dir, not this tree;" >&2
	echo "           components would be built from there (fix: set CODE_DIR=$root/src)." >&2
fi

if [ -z "${QEMU_TEST_NO_COMPOSE:-}" ]; then
	log=$(mktemp)
	if ! ./cos compose "$script" "$name" > "$log" 2>&1; then
		tail -30 "$log" >&2
		rm -f "$log"
		echo "qemu_test: compose failed" >&2
		exit 2
	fi
	rm -f "$log"
fi
if [ ! -f "$build/cos.iso" ]; then
	echo "qemu_test: $build/cos.iso not found" >&2
	exit 2
fi

out=${QEMU_TEST_OUT:-$build/qemu.log}
: > "$out"

# shellcheck disable=SC2086
timeout "$timeout_s" qemu-system-x86_64 -cpu max -smp "$ncpu" -m "$mem" \
	-cdrom "$build/cos.iso" -no-reboot -nographic -nic none ${QEMU_TEST_EXTRA:-} \
	> "$out" 2>&1 < /dev/null &
qpid=$!

verdict=timeout
while kill -0 "$qpid" 2> /dev/null; do
	if grep -aEq "$fail_re" "$out"; then
		verdict=fail
		sleep 2	# let the fault report finish printing
		break
	fi
	if grep -aEq "$pass_re" "$out"; then
		verdict=pass
		sleep 2	# give late failures on other cores a chance to show
		break
	fi
	sleep 1
done
kill "$qpid" 2> /dev/null
wait "$qpid" 2> /dev/null

# Decide on the whole log (QEMU may also have exited on its own).
if grep -aEq "$fail_re" "$out"; then
	verdict=fail
elif grep -aEq "$pass_re" "$out"; then
	verdict=pass
fi

echo "qemu_test: $script: $verdict (log: $out)"
if [ "$verdict" != pass ]; then
	grep -aE "$fail_re" "$out" | head -5
	exit 1
fi
exit 0
