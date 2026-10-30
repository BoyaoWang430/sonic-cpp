#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/rvv_vlen_test.sh RVV_GTEST_BINARY

Run the same compiled RVV test binary under QEMU with VLEN 128, 256, 512,
and 1024 bits. QEMU_RISCV64 selects the emulator (default: qemu-riscv64).
Only Rvv*.* tests are selected; a binary without matching tests is rejected.
The max CPU model also enables extensions that linked dependencies may use;
this is a VLEN check, not a minimum-ISA qualification.
QEMU timings are not native performance measurements.
EOF
}

if [[ $# -eq 1 && ( "$1" == -h || "$1" == --help ) ]]; then
  usage
  exit 0
fi
if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi
if [[ ! -f "$1" || ! -x "$1" ]]; then
  echo "Expected an executable RVV GoogleTest binary: $1" >&2
  exit 2
fi

binary=$(realpath -- "$1")
qemu=${QEMU_RISCV64:-qemu-riscv64}
if ! command -v "$qemu" >/dev/null 2>&1; then
  echo "QEMU executable not found: $qemu" >&2
  exit 2
fi

test_list=$("$qemu" -cpu max,v=true,vlen=128,elen=64,vext_spec=v1.0 \
  "$binary" --gtest_list_tests --gtest_filter='Rvv*.*')
if ! awk '
  /^Rvv[^[:space:]]*\./ { in_suite = 1; next }
  /^[^[:space:]]/ { in_suite = 0 }
  in_suite && /^  [^[:space:]]/ { found = 1 }
  END { exit !found }
' <<< "$test_list"; then
  echo "No Rvv*.* tests found; compile the binary with RVV enabled." >&2
  exit 1
fi

for vlen in 128 256 512 1024; do
  printf 'Running RVV tests with CPU=max, VLEN=%s bits\n' "$vlen"
  "$qemu" -cpu "max,v=true,vlen=$vlen,elen=64,vext_spec=v1.0" \
    "$binary" --gtest_filter='Rvv*.*' --gtest_color=no
done
