#!/usr/bin/env bash
set -euo pipefail
# Smoke test for cuGetProcAddress / cuGetProcAddress_v2 dispatch.
# Requires CUDA 12+ headers and gcc on the host.
# Safe to skip on macOS or any box without a CUDA installation.

command -v gcc >/dev/null || { echo "gcc not found, skipping"; exit 0; }
[ -e /usr/local/cuda/include/cuda.h ] || { echo "CUDA headers not found, skipping"; exit 0; }

cat > /tmp/test_v2.c <<'EOF'
#include <cuda.h>
#include <stdio.h>
int main(void) {
  cuInit(0);
  void *fn = NULL;
  CUresult r = cuGetProcAddress("cuMemAlloc", &fn, 12000, CU_GET_PROC_ADDRESS_DEFAULT);
  printf("rc=%d fn=%p\n", r, fn);
  return r != CUDA_SUCCESS;
}
EOF

gcc -o /tmp/test_v2 /tmp/test_v2.c -L/usr/local/cuda/lib64 -lcuda

if [ -f "$(pwd)/src/libnvshare.so" ]; then
  LD_PRELOAD="$(pwd)/src/libnvshare.so" NVSHARE_DEBUG=1 /tmp/test_v2 2>&1 | grep -q 'fn=' && echo OK
else
  echo "libnvshare.so not built; skipping LD_PRELOAD verification"
  /tmp/test_v2
fi
