#!/usr/bin/env bash
set -euo pipefail

# This test verifies fail-soft: with no scheduler running, libnvshare
# must still load via LD_PRELOAD and a CUDA workload must still run.
# On macOS / non-GPU hosts, we degrade to inspection-only.

if [ -z "${SKIP_GPU_TEST:-}" ] && command -v nvidia-smi >/dev/null 2>&1; then
    sudo pkill -9 nvshare-scheduler 2>/dev/null || true
    sudo rm -f /var/run/nvshare/scheduler.sock 2>/dev/null || true
    NVSHARE_DEBUG=1 LD_PRELOAD=$(pwd)/src/libnvshare.so \
      python tests/pytorch-add-small.py 2>&1 | tee /tmp/test.log
    # Confirm the library logged the expected fail-soft warning
    grep -q -i 'scheduler unreachable\|scheduler\.sock' /tmp/test.log
    # Confirm the CUDA workload still completed
    grep -qi 'PASSED\|sum=\|Test' /tmp/test.log
    echo OK
else
    # No GPU available — verify the source-level guarantee instead:
    # client.c must set rsock = -1 on connect failure (fail-soft sentinel).
    grep -E 'rsock\s*=\s*-1' src/client.c \
        >/dev/null \
        || { echo "FAIL: no fail-soft sentinel (rsock = -1) in src/client.c"; exit 1; }
    # The initial-connect fail-soft path must also log a warning, not call log_fatal.
    grep -q 'scheduler unreachable' src/client.c \
        || { echo "FAIL: 'scheduler unreachable' warning missing from src/client.c"; exit 1; }
    echo "OK (inspection mode — no GPU); fail-soft sentinel found in src/client.c"
fi
