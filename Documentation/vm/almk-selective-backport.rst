Selective ALMK reclaim audit
=============================

Scope
-----

This note records the comparison of Qualcomm's Adaptive Low Memory Killer
(ALMK) lineage with the Hysteris 4.19-based source on the
`debug/memory-419` branch.

The goal is to identify reclaim/allocator improvements that can be reused
without restoring the legacy kernel LMK process-selection policy, especially
its `minfree` and `oom_score_adj` threshold tables. No ALMK driver, process
killer, minfree table, adj table, or userspace interface is added by this
audit.

Reference lineage examined
--------------------------

* Qualcomm `kernel/msm` ALMK source:
  https://android.googlesource.com/kernel/msm/+/ca651d72a3a13a8c08cad4a5db2771b0f23bd7fe/drivers/staging/android/lowmemorykiller.c
* Qualcomm ALMK snapshot containing the reclaim-feedback interface:
  https://android.googlesource.com/kernel/msm/+/fbab955f0afa8e9d7929c18153559383e5603224/drivers/staging/android/lowmemorykiller.c
* Follow-up change introducing `lmk_feed` and `lmk_kill_possible()`:
  https://android.googlesource.com/kernel/msm/%2B/8e09a2c319fead3d04af26c7daa127684e9b86d1
* A downstream OnePlus 7 kernel snapshot (commit dated 2022-06-15/16)
  retains the same interface and shows its consumers in page allocator and
  reclaim code:
  https://github.com/engstk/op7/tree/4edad4df5b942028030f813cab8b7bd354213c3d

The 2022 snapshot is a downstream carry-forward, not evidence that Qualcomm
continued developing ALMK until that date. The references are used to inspect
the mature implementation and its integration points, not to claim a single
universally "latest" ALMK release.

Findings
--------

1. `lmk_feed` is not an independent reclaim optimization
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In the Qualcomm implementation, `lmk_feed` is an atomic deadline owned by
the lowmemorykiller shrinker. When a scan fails to select a victim under the
driver's low-memory conditions, it can set the deadline to approximately
one second in the future. A successful victim selection or a scan outside
those conditions clears it.

`lmk_kill_possible()` exposes that state. Qualcomm consumers use it in:

* `mm/vmscan.c::zone_reclaimable_pages()`, to decide whether anonymous LRU
  pages may be included in the reclaimable-page estimate when no free swap
  slots are available.
* `mm/page_alloc.c::should_reclaim_retry()` and its compaction retry path,
  to influence whether allocation/reclaim attempts continue.

This feedback is meaningful only because the in-kernel LMK shrinker owns the
state and updates it after scanning the task list. Copying the helper and its
call sites without that producer would not reproduce the behavior. Providing
a replacement producer would require a deliberate LMKD-to-kernel contract
and lifecycle semantics; none is present in this branch. Inferring LMKD
victim selection from generic `vmpressure` or PSI events would be incorrect
and could prolong direct-reclaim stalls.

Decision: do not port `lmk_feed`, `lmk_kill_possible()`, or their allocator
and vmscan call sites as a standalone feature.

2. OOM reaper is already present
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The current `mm/oom_kill.c` already contains:

* the OOM reaper worker and its wait queue;
* task queueing through `wake_oom_reaper()`;
* address-space reaping through `__oom_reap_task_mm()`;
* victim-state handling and retry behavior.

The ALMK `oom_reaper` option merely lets its own driver use the kernel's
existing OOM reaper after selecting and signaling a victim. It is not a
separate reaper implementation to backport.

Decision: no OOM-reaper changes.

3. Adaptive min-adj and vmpressure handling remain LMK policy
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The adaptive part of ALMK uses `vmpressure` to change the minimum
`oom_score_adj` admitted by the kernel LMK shrinker. Its decisions also
depend on the driver's minfree arrays and file-cache thresholds.

Removing those policy inputs leaves no independent reclaim algorithm. It
would also create a second process-killing policy alongside Android's
userspace `lmkd`.

Decision: exclude adaptive min-adj, minfree scaling, batch-kill behavior,
victim selection, and ALMK-only kill notifications.

4. CMA/highmem accounting is scoped to LMK victim selection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The Qualcomm `tune_lmk_param()` / `tune_lmk_zone_param()` helpers adjust
free-page and file-cache estimates for the zones accessible to a particular
GFP allocation, including CMA constraints. In ALMK these estimates feed the
decision whether the driver should kill a process. They do not replace the
allocator's zone watermarks or constitute a general reclaim improvement.

Decision: do not transplant these calculations into Hysteris reclaim without
a separate demonstrated allocator-accounting defect on MT6768.

5. ALMK tracepoints are driver-specific
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ALMK trace events describe the driver's vmpressure-to-victim-selection
path. This branch already has standard VM reclaim tracepoints in
`trace/events/vmscan.h` and PSI support. Importing ALMK events without the
driver would either add no useful events or require inventing a new event
contract.

Decision: use existing vmscan/PSI instrumentation first. Add new tracepoints
only if a measured blind spot remains.

Hysteris 4.19 compatibility check
---------------------------------

The current target branch was inspected at:

* `mm/vmscan.c`: `zone_reclaimable_pages()` counts anonymous LRU pages when
  `get_nr_swap_pages() > 0`. It has no ALMK feedback dependency.
* `mm/page_alloc.c`: `should_reclaim_retry()` uses actual reclaim progress
  and the existing no-progress retry accounting. It has no
  `lmk_kill_possible()` dependency.
* `mm/internal.h`: no `lmk_kill_possible()` interface is declared.
* `mm/Kconfig`: no `CONFIG_ANDROID_LOW_MEMORY_KILLER` option is defined.
* `mm/oom_kill.c`: the kernel OOM reaper is already implemented.

This confirms that a partial copy of Qualcomm's allocator hooks would not
be valid in the current configuration.

Result and next step
--------------------

No runtime code has been changed by this audit. The ALMK mechanisms that
could affect reclaim are either inseparable from the legacy LMK shrinker,
already implemented in the 4.19 base (OOM reaper), or specific to LMK's
victim-selection accounting.

The next useful work should therefore remain measurement-driven and use the
existing Android `lmkd`, PSI, vmscan tracepoints, and VM counters to identify
whether the device is experiencing excessive direct-reclaim stalls,
inefficient scan/steal ratios, late kswapd wakeups, or allocation failures.
Any new allocator behavior should be designed as a separate, LMKD-compatible
kernel change rather than presented as a standalone ALMK backport.
