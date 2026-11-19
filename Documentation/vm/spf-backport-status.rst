SPF backport status for the 4.19 Android kernel
=================================================

Scope
-----

This document records the initial audit of the existing tree before enabling
Speculative Page Faults (SPF).  The target is the ARM64 Android 4.19 kernel.
The implementation reference is Android Common 5.10, not a blind copy of the
upstream v5.17 development series.

SPF is intended to reduce contention on the address-space mmap lock for
eligible user page faults.  It is not a reclaim or free-memory optimization.
Every speculative attempt must be allowed to abort and use the existing
mmap-lock-protected fault path.

Existing SPF-related scaffolding
--------------------------------

The branch already contains dormant, CONFIG_SPECULATIVE_PAGE_FAULT-guarded
pieces:

* include/linux/mm_types.h has vm_area_struct.vm_sequence and
  vm_area_struct.vm_ref_count.
* include/linux/mm_types.h has mm_struct.mm_rb_lock.
* include/linux/mm.h initializes the VMA sequence/refcount in INIT_VMA()
  and provides vm_write_begin()/vm_write_end() wrappers.
* include/linux/mm.h defines FAULT_FLAG_SPECULATIVE and includes it in
  FAULT_FLAG_TRACE.
* kernel/fork.c initializes mm_rb_lock in mm_init().

These pieces are not an operational SPF implementation.  In the audited
branch, CONFIG_SPECULATIVE_PAGE_FAULT is not defined in mm/Kconfig or
arch/arm64/Kconfig, and no SPF object is added to mm/Makefile.

Missing implementation
----------------------

The following required pieces were not found in the audited source:

* No get_vma()/put_vma() reference protocol exists.  vm_ref_count is only
  initialized; no reader pins a VMA and no removal path defers the release
  of VMA-owned resources until the last reference is dropped.
* No writer-side protocol uses vm_sequence.  mmap.c does not bracket VMA
  metadata changes with vm_write_begin()/vm_write_end().
* mm_rb_lock is initialized but the VMA rbtree insertion, removal, and
  lookup paths do not use it for SPF-safe concurrent lookup.
* mm/memory.c has no speculative page-table walk or fault resolver.
* arch/arm64/mm/fault.c does not attempt SPF before taking mmap_lock.
* There is no SPF-specific Kconfig dependency/architecture capability gate,
  trace event set, or complete fallback/reuse path.

The current vm_area_free() releases the slab object immediately.  This is
not sufficient for SPF: a speculative reader may still hold a VMA pointer,
and file-backed fault callbacks may access vm_file after the VMA is unlinked.
The lifetime work must therefore cover both the VMA allocation and the
resources reachable through it, not just add a reference counter.

Implementation order
--------------------

1. Complete VMA lifetime management: pinning, final put, deferred VMA
   destruction, and correct retention of vm_file and VMA policy resources.
2. Serialize SPF lookup against rbtree updates and bracket every relevant
   VMA metadata writer with the sequence protocol.  Include split, merge,
   unmap, mprotect, mremap, stack expansion, brk, fork/dup, and teardown.
3. Add the speculative page-table walk and fault resolver, initially
   restricting unsupported cases to VM_FAULT_RETRY.  Do not create page-table
   levels speculatively where teardown could race and leak them.
4. Add the ARM64 user-fault entry hook.  Kernel faults and ineligible VMAs
   must continue through the existing path.
5. Expand support for file-backed faults, swap, COW, THP, KSM, and userfaultfd
   only where their locking, callbacks, and page-table semantics are proven
   safe.  Unsupported cases must fall back rather than partially execute.
6. Add Kconfig gating, tracepoints/counters, and stress coverage for concurrent
   mmap/munmap/mprotect/mremap/fork/exit and page-fault workloads.

Compatibility notes
-------------------

The tree already has seqcount, RCU, rwlock, and ARM64 page-table primitives;
these should be reused where their 4.19 semantics match the Android 5.10
implementation.  This does not imply that the 5.10 SPF code can be copied
without adaptation.  The VMA lifetime and writer-serialization contracts
must be ported as a unit.

No runtime behavior is enabled by this audit.  No kernel build or device
stress test has been performed as part of this status record.
