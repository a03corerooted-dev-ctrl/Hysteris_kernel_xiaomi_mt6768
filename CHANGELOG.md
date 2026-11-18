# Hysteris — Stable Integration 4.19

## Base

- Linux kernel 4.19.325 for the Xiaomi merlin / MediaTek MT6768 target.
- ARM64 build configuration uses `vendor/mt6768_defconfig` with the `vendor/` device fragment.
- This branch is based on the previously tested scheduler/reclaim/ZRAM integration line and adds the reclaim changes listed below.
- The tested build was reported to boot successfully on-device, with noticeably smoother operation than the user's previous 17.0 branch. This is a user-reported result, not a benchmark claim.

## Memory management and reclaim

### LRU rotation on inactive-page putback

- Restores tail insertion for eligible inactive pages when reclaim puts isolated pages back onto the LRU.
- The special path is limited to pages that are inactive, unmapped, clean, not under writeback, unreferenced, and satisfy `is_page_cache_freeable()`.
- All other pages retain the normal LRU insertion path.
- Intended to preserve the effect of a missed reclaimable-page rotation when writeback completes while a page is isolated.

### Timed application-launch memory boost

- Adds `/sys/kernel/mm/vmscan/memory_boost` (read/write, mode 0644).
- Writing `1` activates a short-lived reclaim hint for up to five seconds; writing `0` disables it.
- The hint is honored only by global kswapd reclaim. It biases scanning toward file pages to temporarily preserve anonymous working sets during application launches.
- A dynamic file-cache reserve prevents the boost from consuming cache below the larger of 5% of node-present pages or the aggregate high watermark.
- Direct reclaim and memcg reclaim policy are not changed by this feature.

### Memory-boost expiry race fix

- Makes reserve-triggered cancellation conditional with `cmpxchg()`.
- Prevents a reclaim-side cancellation from clearing a newer userspace boost request that races with the reserve check.

### Swap-thrashing detection and reclaim bias

- Adds a lightweight detector based on the combined `PSWPIN` and `PSWPOUT` VM event counters.
- Samples approximately every 125 ms (subject to kernel HZ), using a serialized snapshot of online CPU counters.
- Detects sustained swap churn above 12,000 events per second and keeps the detector active for a one-second hold period.
- Counter snapshots that move backwards are treated as a new baseline rather than as a counter wrap.
- During detected churn, global kswapd reclaim caps swappiness at 40, preserving proportional reclaim rather than forcing file-only scanning.
- The bias is applied only while the dynamic file-cache reserve is available; it does not block reclaim.
- Adds read-only telemetry at `/sys/kernel/mm/vmscan/swap_thrash` (mode 0444), returning `1` while the detector's hold window is active and `0` otherwise.
- The implementation is guarded by `CONFIG_VM_EVENT_COUNTERS`; without it, the detector is inactive.

### Sysfs status formatting

- Corrects the newline formatting of the `swap_thrash` status value.

## Builder integration

- The paired builder branch is `stable/integration-4.19` in `NonGKI_Kernel_Build-K419`.
- The `xxksu` build configuration now fetches this kernel branch.
- The builder keeps the existing ARM64, MT6768 defconfig/fragment, Clang, KernelSU, APatch, and AnyKernel3 selections from the tested configuration.

## Validation status and notes

- The user reports that the latest build completed, the device booted successfully, and interactive smoothness improved versus the previous 17.0 branch.
- No independent benchmark, stress test, or long-duration stability test is claimed here.
- The swap-thrash threshold and reclaim behavior remain candidates for measurement on-device under sustained memory pressure.
