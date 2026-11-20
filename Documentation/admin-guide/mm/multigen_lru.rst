.. SPDX-License-Identifier: GPL-2.0

=============
Multi-Gen LRU
=============

This document describes the page-based Multi-Gen LRU (MGLRU) backport for
the Linux 4.19 tree. It is adapted from the upstream MGLRU admin guide
authored by Yu Zhao (upstream commit 07017acb06012d250fb68930e809257e6694d324).
The implementation in this tree uses ``struct page``; it does not depend on
the newer folio APIs or include the separate MGLRU-FG proposal.

Overview
========

MGLRU is an alternative LRU implementation intended to improve page reclaim
under memory pressure. It divides evictable pages in each ``lruvec`` into
generations. Pages in newer generations have been accessed more recently;
older generations are reclaim candidates.

The implementation tracks anonymous and file-backed pages separately while
aging both types on a common generation sequence. It also tracks up to four
tiers within each generation. Tiers represent different access frequencies
and feed the refault-based protection logic.

Aging uses reverse-mapping information and page-table walks to find accessed
pages. Eviction consumes old generations, uses refault feedback to balance
anonymous and file-backed reclaim, and protects tiers that show evidence of
being reused. The page-table walker uses per-memcg ``mm_struct`` lists and
Bloom filters to avoid repeatedly scanning address spaces that are unlikely
to contain useful young PTEs.

Build configuration
===================

Enable the implementation with::

    CONFIG_LRU_GEN=y

For this experimental 4.19 branch, ``CONFIG_LRU_GEN_ENABLED`` is deliberately
left unset. This compiles MGLRU and exposes its controls, but keeps the
classic active/inactive LRU as the boot-time default. Enable MGLRU explicitly
at runtime after validating the kernel on the target device.

Optional historical statistics are enabled with::

    CONFIG_LRU_GEN_STATS=y

This option adds per-memcg and per-node accounting overhead and is intended
for debugging and validation. The debugfs interface also requires::

    CONFIG_DEBUG_FS=y

Runtime controls
================

The sysfs directory ``/sys/kernel/mm/lru_gen/`` contains the runtime
controls.

Kill switch
-----------

The ``enabled`` file reports and controls the supported MGLRU components.
Read it first; the returned mask depends on the architecture and CPU
capabilities.

The mask bits are:

====== ================================================================
Value  Component
====== ================================================================
0x0001 Main MGLRU switch. This moves evictable pages between the classic
       LRU lists and the generation lists.
0x0002 Batched clearing of accessed bits in leaf page-table entries.
       This requires hardware support for accessed-bit updates.
0x0004 Clearing accessed bits in non-leaf page-table entries. This is
       available only when the architecture advertises the capability.
[yYnN] Enable or disable all supported components.
====== ================================================================

For example, to enable the main MGLRU switch and all supported components::

    cat /sys/kernel/mm/lru_gen/enabled
    echo y > /sys/kernel/mm/lru_gen/enabled
    cat /sys/kernel/mm/lru_gen/enabled

Do not assume that the result must be ``0x0007``. Unsupported architecture
capabilities are omitted from the reported mask. In particular, bit 0x0004
is not a requirement for correct operation.

To return to the classic LRU::

    echo n > /sys/kernel/mm/lru_gen/enabled

Changing the main switch migrates evictable pages between the two LRU
representations while holding the required reclaim and memory-hotplug locks.
It is not a substitute for testing under memory pressure.

Thrashing prevention
--------------------

The ``min_ttl_ms`` file controls the minimum age of the oldest generation
that reclaim may evict. A non-zero value protects the recent working set for
approximately that interval::

    echo 1000 > /sys/kernel/mm/lru_gen/min_ttl_ms

The default value, ``0``, disables this protection. A value that is too large
can prevent reclaim from making progress and may lead to OOM; use conservative
values and monitor memory pressure.

Debugfs interface
=================

When ``CONFIG_DEBUG_FS=y``, the following files are available:

* ``/sys/kernel/debug/lru_gen`` provides generation information and accepts
  aging and proactive-eviction commands.
* ``/sys/kernel/debug/lru_gen_full`` provides additional historical
  statistics when ``CONFIG_LRU_GEN_STATS=y``.

The debugfs interface is intended for diagnostics and controlled experiments.
It is not necessary for normal MGLRU operation.

Working-set estimation
----------------------

Reading ``lru_gen`` reports generation numbers, age in milliseconds, and
anonymous/file page counts for each memcg and node. The oldest generation
contains the coldest pages; the newest contains the most recently accessed
pages.

A new generation can be requested with::

    echo "+ memcg_id node_id max_gen_nr [can_swap [force_scan]]" \
        > /sys/kernel/debug/lru_gen

``can_swap`` defaults to the current swap policy. Setting it to ``1``
allows anonymous-page aging even when the normal swap policy would prevent
it. ``force_scan`` defaults to ``1``; setting it to ``0`` allows the
walker to use heuristics that reduce overhead but may reduce coverage.

Proactive reclaim
-----------------

The following command requests eviction of generations up to
``min_gen_nr``::

    echo "- memcg_id node_id min_gen_nr [swappiness [nr_to_reclaim]]" \
        > /sys/kernel/debug/lru_gen

``min_gen_nr`` must be older than the two youngest generations.
``swappiness`` overrides the normal swappiness setting and accepts values
from 0 through 200. ``nr_to_reclaim`` limits the number of pages reclaimed.

These debugfs commands can initiate reclaim without ordinary memory pressure.
Use them only for controlled tests, and monitor reclaim latency, refaults,
swap activity, and OOM events.

Compatibility notes
===================

This is a backport to the 4.19 memory-management interfaces. It intentionally
retains ``struct page``, the target tree's ``lruvec`` and memcg structures,
and its existing swap and reclaim APIs. Do not transplant newer folio-based
MGLRU or MGLRU-FG changes directly into this branch: each such change needs
a separate compatibility and lifetime audit.

The ARM64 accessed-bit optimizations are capability-gated. Their presence in
the build does not imply that every CPU supports every mask bit. Always use
the mask reported by ``/sys/kernel/mm/lru_gen/enabled`` as the authoritative
runtime capability set.
