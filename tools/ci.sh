#!/bin/sh
# Build and test script.
#
#   tools/ci.sh            clean build, boot smoke test, full self-test run in QEMU (needs QEMU + Python 3)
#   tools/ci.sh --quick    incremental build + boot smoke test only
#   tools/ci.sh --watch    repeat --quick whenever a source file changes (a poor man's continuous build)
#
# Exit status is non-zero if the build has warnings or errors, the kernel does not boot, or any
# self-test fails. A full log of the last run is kept in build/ci.log.
cd "$(dirname "$0")/.." || exit 1
LOG=build/ci.log
mkdir -p build

step() { printf '== %s\n' "$*"; }
fail() { printf 'CI FAILED: %s\n' "$*"; exit 1; }

build() {
	[ "$1" = clean ] && make clean >/dev/null 2>&1
	make >"$LOG" 2>&1 || { tail -n 25 "$LOG"; fail "build error"; }
	if grep -qi 'warning' "$LOG"; then
		grep -i 'warning' "$LOG" | head -n 10
		fail "the build produced warnings"
	fi
	printf 'kernel image: %s bytes in %s sectors\n' "$(stat -c%s build/kernel.bin)" \
		"$(( ($(stat -c%s build/kernel.bin) + 511) / 512 ))"
}

smoke() {
	python tools/qemu_drive.py --fresh-disk --menu 1 "version" "ls /proc" >>"$LOG" 2>&1
	grep -q 'Tiny OS .* - type' "$LOG" || fail "the kernel did not reach the shell"
	grep -q 'PANIC\|EXCEPTION' "$LOG" && fail "the kernel panicked while booting"
	printf 'boot smoke test: ok\n'
}

selftests() {
	python tools/qemu_drive.py --fresh-disk --menu 1 "format" "selftest" "wait:20" >"$LOG.selftest" 2>&1
	cat "$LOG.selftest" >>"$LOG"
	grep -E '^\s+\[(PASS|FAIL|SKIP)\]' "$LOG.selftest" | grep -v PASS
	summary=$(grep -E '[0-9]+ passed, [0-9]+ failed' "$LOG.selftest" | tail -n 1)
	[ -n "$summary" ] || fail "the self-test run did not finish"
	printf 'self-tests: %s\n' "$summary"
	case "$summary" in
	*" 0 failed"*) ;;
	*) fail "self-tests failed (see $LOG)";;
	esac
}

sources() { find kernel include user boot Makefile -type f 2>/dev/null | sort | xargs cksum | cksum; }

case "$1" in
--watch)
	last=""
	while :; do
		now=$(sources)
		if [ "$now" != "$last" ]; then
			last=$now
			step "change detected, building"
			( build && smoke ) && printf 'OK\n\n' || printf 'FAILED (waiting for the next change)\n\n'
		fi
		sleep 2
	done
	;;
--quick)
	step "build";       build
	step "boot";        smoke
	;;
*)
	step "clean build"; build clean
	step "boot";        smoke
	step "self-tests";  selftests
	;;
esac
printf 'CI PASSED\n'
