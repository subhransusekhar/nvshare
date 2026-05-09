# nvshare Operator Runbook

This runbook covers installation verification, environment variables, and
mitigation steps for common failure modes.

---

## Scheduler unreachable

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

## Installation verification

After installing nvshare on a local host, verify the setup:

1. Start the scheduler:
   ```
   sudo nvshare-scheduler &
   ```
2. Confirm the socket was created:
   ```
   ls -la /var/run/nvshare/scheduler.sock
   ```
3. Run the smoke test with `LD_PRELOAD`:
   ```
   LD_PRELOAD=/usr/local/lib/libnvshare.so python tests/pytorch-add-small.py
   ```
   The output should include `Successfully initialized nvshare GPU`.

---

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `NVSHARE_DEBUG` | unset | Set to `1` to enable verbose debug logging from `libnvshare`. |
| `NVSHARE_LOG_FORMAT` | unset | Set to `json` to emit structured JSON log lines to stderr. |
| `NVSHARE_SCHEDULER_SOCKET` | `/var/run/nvshare/scheduler.sock` | Override the scheduler socket path. |
| `KUBERNETES_SERVICE_HOST` | unset | Detected automatically; enables Kubernetes pod-name/namespace tagging. |
