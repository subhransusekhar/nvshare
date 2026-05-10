#!/usr/bin/env bash
# Copyright (c) 2023 Georgios Alexopoulos
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# soak-restart.sh — exercises scheduler-restart resilience (#9/#10/#11).
#
# Runs two PyTorch workloads continuously while kill -9'ing the scheduler
# every KILL_INTERVAL_SEC seconds for DURATION_SEC seconds total.
# Pass condition: zero Python Tracebacks across the entire run.
#
# Usage:
#   sudo bash tests/scripts/soak-restart.sh
#
# Environment variables:
#   DURATION_SEC        Total soak duration in seconds.  Default: 3600 (1 h).
#   KILL_INTERVAL_SEC   How often to kill the scheduler.  Default: 300 (5 min).
#   LOG_DIR             Directory to write logs.  Default: /tmp/soak-<epoch>.

set -euo pipefail

DURATION_SEC=${DURATION_SEC:-3600}
KILL_INTERVAL_SEC=${KILL_INTERVAL_SEC:-300}
LOG_DIR=${LOG_DIR:-/tmp/soak-$(date +%s)}

mkdir -p "$LOG_DIR"
echo "logs: $LOG_DIR"

# ── Start fresh ──────────────────────────────────────────────────────────────
sudo pkill -9 nvshare-scheduler || true
sudo rm -rf /var/run/nvshare && sudo mkdir -p /var/run/nvshare
sudo nvshare-scheduler > "$LOG_DIR/scheduler-1.log" 2>&1 &

# ── Two clients running pytorch-add-small in a tight loop ────────────────────
(
    while true; do
        LD_PRELOAD=/usr/local/lib/libnvshare.so \
            python tests/pytorch-add-small.py >> "$LOG_DIR/client-1.log" 2>&1
    done
) &
CLIENT_1=$!

(
    while true; do
        LD_PRELOAD=/usr/local/lib/libnvshare.so \
            python tests/pytorch-add-small.py >> "$LOG_DIR/client-2.log" 2>&1
    done
) &
CLIENT_2=$!

# ── Soak loop — kill -9 the scheduler every KILL_INTERVAL_SEC ────────────────
start=$(date +%s)
restart_count=0

while [ $(( $(date +%s) - start )) -lt "$DURATION_SEC" ]; do
    sleep "$KILL_INTERVAL_SEC"
    sudo pkill -9 nvshare-scheduler
    sleep 2
    restart_count=$(( restart_count + 1 ))
    sudo nvshare-scheduler > "$LOG_DIR/scheduler-$(( restart_count + 1 )).log" 2>&1 &
    echo "restart #$restart_count at $(date)"
done

# ── Tear down ─────────────────────────────────────────────────────────────────
kill "$CLIENT_1" "$CLIENT_2" 2>/dev/null || true

TRACEBACK_COUNT=$(grep -c 'Traceback' "$LOG_DIR"/client-*.log 2>/dev/null || true)

echo "soak complete: $restart_count scheduler restarts"
echo "expected client crashes: 0"
echo "actual client crashes: $TRACEBACK_COUNT"
