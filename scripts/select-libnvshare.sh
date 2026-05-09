#!/usr/bin/env bash
# Detects host glibc version and prints the path to the right artifact.
set -euo pipefail
GLIBC_VER=$(ldd --version | head -1 | awk '{print $NF}')
MAJOR=${GLIBC_VER%%.*}
MINOR=${GLIBC_VER#*.}; MINOR=${MINOR%%.*}
if [ "$MAJOR" -ge 2 ] && [ "$MINOR" -ge 34 ]; then
  echo /opt/nvshare/libnvshare-glibc2.34.so
else
  echo /opt/nvshare/libnvshare-glibc2.31.so
fi
