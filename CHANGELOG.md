# Hysteris — Changelog

## Stable Integration 4.19

**Target:** Xiaomi merlin / MediaTek MT6768  
**Kernel:** Linux 4.19.325  
**Architecture:** ARM64

### Memory management

#### LRU rotation on inactive-page putback
- Added a conditional tail-insertion path in `putback_inactive_pages()` for eligible inactive pages.
- The path applies to pages that are unmapped, clean, not under writeback, unreferenced, and satisfy `is_page_cache_freeable()`.
- Other pages continue through the normal LRU insertion path.
- Preserves reclaimable-page rotation when a page is isolated during writeback completion.

#### Timed memory boost
- Added the sysfs interface `/sys/kernel/mm/vmscan/memory_boost` with read/write permissions (0644).
- Writing `1` activates a temporary boost with a five-second expiry.
- Writing `0` disables the boost.
- The policy applies to global kswapd reclaim and biases scanning toward file pages during the boost window.
- Added a dynamic file-cache reserve based on the greater of 5% of node-present pages or the aggregate high watermark.
- The reserve condition prevents the boost from remaining active when available file cache falls below the configured reserve.

#### Memory boost race-safety
- Made reserve-triggered boost cancellation conditional with `cmpxchg()`.
- Prevents a cancellation from clearing a newer boost request issued concurrently.

#### Swap-thrashing detection
- Added swap activity monitoring using the `PSWPIN` and `PSWPOUT` VM event counters.
- Samples aggregate per-CPU counters at an interval of approximately 125 ms, depending on kernel HZ.
- Activates the detector when combined swap activity exceeds 12,000 events per second.
- Keeps the detector active for a one-second hold period after the threshold is reached.
- Resets the sampling baseline when observed counters decrease.
- The implementation is conditional on `CONFIG_VM_EVENT_COUNTERS`.

#### Swap-aware reclaim policy
- During detected swap thrashing, caps global kswapd reclaim swappiness at 40.
- Retains proportional reclaim behavior rather than forcing file-only scanning.
- Applies the adjustment only while the dynamic file-cache reserve is available.
- Does not disable reclaim or alter direct-reclaim and memcg reclaim policy.

#### Swap-thrashing telemetry
- Added the read-only sysfs interface `/sys/kernel/mm/vmscan/swap_thrash` (0444).
- Reports `1` while the detector's hold window is active and `0` otherwise.
- Corrected the sysfs output newline formatting.

### Build configuration
- Uses `vendor/mt6768_defconfig` with the `vendor/` configuration fragment directory.
- Configured for ARM64 and the `Image.gz-dtb` output.
- The paired XXKSU builder configuration targets the `stable/integration-4.19` kernel branch.
