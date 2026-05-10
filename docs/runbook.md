# nvshare Operator Runbook

This runbook covers installation verification, environment variables, and
mitigation steps for common failure modes.

---

## Install

**Local.** Follow the [Installation (Local)](../README.md#installation_local)
and [Usage (Local)](../README.md#usage_local) sections of the README.
If the binary reports a `GLIBC_2.34 not found` error, run
`scripts/select-libnvshare.sh` to choose the glibc-2.31-compatible artifact
(see [GLIBC_2.34 not found](#glibc_234-not-found) below).

**Kubernetes.** Follow the [Installation (Kubernetes)](../README.md#installation_k8s)
section of the README. The resource name is `netgroup.ai/gpu` (renamed from
`nvshare.com/gpu` in the first `production-roadmap` commit).

---

## Verify it's working

After starting `nvshare-scheduler` and injecting `libnvshare.so`:

**1. Confirm `LD_PRELOAD` took effect.**

```
grep libnvshare /proc/$(pgrep -n python)/maps
```

You should see a line containing `libnvshare.so`. If empty, the library was
not preloaded into the target process.

**2. Confirm dispatch coverage.**

```
nm -D /usr/local/lib/libnvshare.so | grep -E ' T cu' | wc -l
```

Expect ≥ 25. A lower count means the build is missing hook coverage for
modern CUDA entry points (VMM, async allocator, UM hint families).

**3. Confirm the scheduler is alive and serving metrics.**

```
curl -s localhost:9601/metrics | head -5
```

You should see Prometheus-format lines starting with `# HELP nvshare_`. If
the connection is refused, the scheduler is not running or has crashed.

---

## Tunable env vars

These are the environment variables read by nvshare. Set them before
launching the workload (or the scheduler, as noted).

| Variable | Component | Default | Description |
|---|---|---|---|
| `NVSHARE_DEBUG` | hook + scheduler | unset | Set to `1` to enable verbose debug logging. |
| `NVSHARE_LOG_FORMAT` | hook + scheduler | unset | Set to `json` to emit structured JSON log lines to stderr. |
| `NVSHARE_ENABLE_SINGLE_OVERSUB` | hook | unset | Set to `1` to allow a single process to allocate more GPU memory than the physical GPU can hold. Use only when you know the working set fits; may cause internal thrashing. |
| `NVSHARE_SCHEDULER_SOCKET` | hook + scheduler | `/var/run/nvshare/scheduler.sock` | Override the path to the scheduler UNIX socket. |
| `KUBERNETES_SERVICE_HOST` | hook | unset | Detected automatically inside a Pod; enables pod-name/namespace tagging in log events. |

**Workload-side (not nvshare-owned).** `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True`
causes PyTorch to use the VMM allocator family (`cuMemCreate` /
`cuMemAddressReserve`). nvshare hooks these as of #1/#2; do not disable
that env var — it is expected and handled.

---

## Common failures

### Scheduler unreachable

**Symptom.** Pod logs show:

```
[NVSHARE][WARN]: client: scheduler unreachable (<socket-path>); running without anti-thrash
```

**What is happening.** `libnvshare` did not find `/var/run/nvshare/scheduler.sock`,
or the `connect()` call failed. The library is still loaded — `cudaMalloc` is still
being routed to managed memory, so memory oversubscription continues to work. However,
the FCFS lock and time-quantum anti-thrash are off, so co-located workloads may thrash.

This is an intentional fail-soft: the library never aborts the process when the
scheduler is missing. Anti-thrash is silently disabled, and the workload continues.

**Causes (most common first).**

1. Device plugin did not mount `/var/run/nvshare/` into the workload pod.
2. `nvshare-scheduler` DaemonSet pod is not running
   (`kubectl get pods -n nvshare-system`).
3. `nvshare-scheduler` crashed and the restart policy has not brought it back yet
   (check container logs).

**Mitigation.**

- Verify the socket mount:
  ```
  kubectl exec <pod> -- ls -la /var/run/nvshare/
  ```
- Check scheduler pod status:
  ```
  kubectl get pods -n nvshare-system
  ```
- Restart the scheduler DaemonSet:
  ```
  kubectl rollout restart -n nvshare-system ds/nvshare-scheduler
  ```
- The workload itself is fine; restart it only if you need anti-thrash to kick in
  immediately for a new run. After `#10`'s reconnect-with-retry work, an already-
  running workload will automatically reconnect within ~30 s of the scheduler
  coming back.

---

### GLIBC_2.34 not found

**Symptom.**

```
nvshare-scheduler: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.34' not found
```

**Cause.** The prebuilt binary was linked against glibc 2.34 (Ubuntu 22.04+)
but the host has an older glibc (Ubuntu 20.04 / Debian 11 ship 2.31).

**Fix.** Run the runtime selector:

```
bash scripts/select-libnvshare.sh
```

This script inspects the host's glibc version and symlinks the correct artifact
(glibc-2.31 or glibc-2.34 build) into place. Both variants are shipped as of
#7. After running the script, restart the scheduler.

---

### Hooks not firing on CUDA 12+

**Symptom.** `cudaMalloc` calls are not being redirected to `cudaMallocManaged`.
The scheduler grants locks but GPU memory usage is unexpectedly low or the
workload crashes with `CUDA_ERROR_OUT_OF_MEMORY` as if UM is off.

**Cause.** On CUDA 12+, the CUDA Runtime resolves Driver API symbols via
`cuGetProcAddress` rather than via the ELF symbol table. If `cuGetProcAddress`
itself is not hooked, all subsequent symbol lookups bypass nvshare.

**Verify.**

```
nm -D /usr/local/lib/libnvshare.so | grep cuGetProcAddress
```

Expected output includes a line with `T cuGetProcAddress` and the symbol
version tag `@@NVSHARE_HOOKS`. If the symbol is absent or shows `U`
(undefined), the build is broken.

**Fix.** Rebuild from the `production-roadmap` branch (includes #4/#5).
The `hook_proc_addr.c` file provides the single-source dispatch table;
`cuGetProcAddress` and `cuGetProcAddress_v2` must both appear as `T` in
`nm -D` output.

---

### Lock never granted

**Symptom.** A workload logs `req_lock_received` but never proceeds; the
scheduler never emits `lock_granted` for that `req_id`.

**Diagnosis.**

1. Grep scheduler logs for the `req_id` seen in the client:
   ```
   grep <req_id> /var/log/nvshare-scheduler.log
   ```
   If `req_lock_received` appears but `lock_granted` does not, the request
   is queued but the lock is not being cycled.

2. Check whether the current lock holder is alive:
   ```
   grep lock_granted /var/log/nvshare-scheduler.log | tail -1
   ```
   Note the `req_id` and `pod` of the current holder. If that pod is gone,
   the heartbeat sweep should have reaped it within 30 s. Check for a
   `heartbeat_timeout` event for that pod.

3. If the heartbeat sweep is not firing, verify the scheduler has not
   deadlocked — `curl localhost:9601/metrics` should still respond. If not,
   the scheduler needs a restart (`kubectl rollout restart`); the reconnect
   logic (#10) will bring clients back.

4. If `req_id` tracking is needed across both sides (#15), grep for the
   same 16-hex string in both the scheduler and client logs simultaneously:
   ```
   grep <req_id> /var/log/nvshare-scheduler.log /var/log/nvshare-client.log
   ```

---

## Metrics quick-tour

The scheduler exposes five Prometheus metrics on `http://localhost:9601/metrics`.

| Metric | Type | What it tells you | Alert threshold |
|---|---|---|---|
| `nvshare_clients_connected` | Gauge | Number of clients currently registered with the scheduler. | Alert if 0 when workloads are expected to be running. |
| `nvshare_lock_acquisitions_total` | Counter | Cumulative lock grants since scheduler start. | Rate drop to 0 for > 2 × TQ seconds while clients are connected. |
| `nvshare_lock_wait_seconds` | Histogram | Time from `req_lock_received` to `lock_granted` per acquisition. | p95 > 2 × TQ suggests queue backup or a stuck holder. |
| `nvshare_lock_hold_seconds` | Histogram | Time the lock was held per acquisition (including early-release). | p99 > TQ + 10 s may indicate a hanging workload. |
| `nvshare_early_releases_total` | Counter | Cumulative count of locks dropped before TQ elapsed. | Informational; high rate means workloads are idle-releasing correctly. |

To scrape with Prometheus, add `localhost:9601` as a static target. All
metric names are prefixed `nvshare_`.

---

## Log markers

Grep for these structured event names in scheduler logs.
Enable JSON format with `NVSHARE_LOG_FORMAT=json` for machine-readable output;
each line is then a single JSON object with `level`, `event`, and `msg` fields.

| Marker | Source | Meaning |
|---|---|---|
| `req_lock_received` | scheduler | Client sent a `REQ_LOCK` message; request enqueued. |
| `lock_granted` | scheduler | Lock handed to the next client in FCFS order. |
| `lock_released` | scheduler | Client returned the lock via `DROP_LOCK`. |
| `drop_lock` | scheduler | Scheduler sent `DROP_LOCK` to the current holder (TQ expired). |
| `heartbeat_timeout` | scheduler | No heartbeat from a client within the deadline; client reaped. |
| `scheduler_resumed` | scheduler | Scheduler restarted and loaded state from `state.json`. |

**JSON logging.** Set `NVSHARE_LOG_FORMAT=json` on the scheduler process to
get one JSON object per line:

```
NVSHARE_LOG_FORMAT=json sudo nvshare-scheduler 2>scheduler.log
```

Then query with `jq`:

```
jq 'select(.event == "heartbeat_timeout")' scheduler.log
```

---

## Running soak tests

The scheduler-restart soak test (`tests/scripts/soak-restart.sh`) exercises the
restart-resilience work from issues #9 (state.json), #10 (reconnect-with-cached-id),
and #11 (HEARTBEAT). It runs two PyTorch workloads continuously while killing the
scheduler with `SIGKILL` at a fixed interval for the full duration, then reports the
number of Python tracebacks observed.

**Requirements.** A host with an NVIDIA GPU, `nvshare-scheduler` on `PATH`, and
`/usr/local/lib/libnvshare.so` installed. The script uses `sudo` for scheduler
management; run it as a user with passwordless sudo or as root.

**Quick start (1 h, kill every 5 min):**

```
sudo bash tests/scripts/soak-restart.sh
```

Or via Make (from the repo root):

```
make -C tests soak-restart
```

**Environment variables:**

| Variable | Default | Description |
|---|---|---|
| `DURATION_SEC` | `3600` | Total soak duration in seconds. |
| `KILL_INTERVAL_SEC` | `300` | Seconds between each `kill -9` of the scheduler. |
| `LOG_DIR` | `/tmp/soak-<epoch>` | Directory where per-scheduler and per-client logs are written. |

**Example — short 10-minute smoke run:**

```
DURATION_SEC=600 KILL_INTERVAL_SEC=60 sudo bash tests/scripts/soak-restart.sh
```

**Pass condition.** The final output line must read:

```
actual client crashes: 0
```

Any non-zero count means a Python traceback appeared in a client log, which indicates
a regression in the restart-resilience logic.

**GitHub Actions.** `.github/workflows/soak.yml` provides a `workflow_dispatch`
trigger targeting a `[self-hosted, gpu]` runner. It never runs automatically and
never blocks PRs. Trigger it manually from the Actions tab when a GPU runner is
available.
