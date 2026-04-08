# Hydra - NUMA-Aware Page Table Replication for Linux

A Linux 7.0 kernel patch that replicates user-space page tables across NUMA nodes so each node walks a local copy during TLB misses.

## About This Fork

This repository contains a substantially bugfixed and extended reimplementation of Hydra, rebased onto Linux 7.0 (7.0.0-rc6). It is derived from, and should be distinguished from, the original Hydra prototype published alongside the USENIX ATC '24 paper.

**Original paper:**
Gao, B., Kang, Q., Tee, H.-W., Chu, K. T. N., Sanaee, A., and Jevdjic, D. *Scalable and Effective Page-table and TLB Management on NUMA Systems.* In Proceedings of the 2024 USENIX Annual Technical Conference (USENIX ATC '24), pp. 445-461. https://www.usenix.org/conference/atc24/presentation/gao-bin-scalable

**Original implementation:** https://github.com/julianw1010/Hydra-v6.5

The original implementation is a research prototype demonstrating the ideas from the paper. This fork aims to turn that prototype into something that boots reliably on contemporary multi-socket machines, runs real workloads without data corruption, and provides the debugging and introspection needed to diagnose problems when they occur. Earlier revisions of this fork targeted Linux 6.4; the current tree targets Linux 7.0 and tracks the mm/ and x86/ changes that landed in between (notably the `ptdesc`/`pagetable_*_ctor` split, the `unmap_desc` refactor of `free_pgtables`, the new `pagetable_dtor` path, the `softleaf`/migration entry reshuffle in huge_memory.c, and the updated `switch_mm_irqs_off` ASID accounting).

## Differences From the Original Implementation

The list below enumerates changes relative to the original Hydra-v6.5 tree, carried forward and updated for 7.0. Items are grouped by category.

### Correctness fixes in the replication core

1. The original hardcodes `NUMA_NODE_COUNT` to 2 in `include/linux/mm_types.h`. This fork makes the node count a Kconfig option (`CONFIG_HYDRA_NUMA_NODE_COUNT`) and adds an `early_initcall` that panics at boot if the compile-time value disagrees with `num_online_nodes()`, preventing silent memory corruption on machines with more than two sockets.
2. The original only hooks `set_pte` into `pv_ops.mmu`. Huge-page PMD writes (THP installation, splits, wrprotect) are therefore not propagated to replica PMDs, causing divergence whenever transparent huge pages are involved. Higher directory levels (PUD, P4D, PGD) do not require propagation because each replica tree is independent above the leaf mapping level, but PMD writes need to be broadcast because a PMD can itself be a leaf (huge page) mapping. This fork hooks `set_pmd`, `set_pud`, `set_p4d`, and `set_pgd` via `pgtable_track_set_*` wrappers that propagate huge-PMD entries to all replicas in the chain and reconcile type transitions (huge-to-normal and normal-to-huge) consistently.
3. The original leaves PCID and INVPCID enabled. This fork clears `X86_FEATURE_PCID` and `X86_FEATURE_INVPCID` in `filter_cpuid_features` during early boot. This change was made after observing memory corruption on newer kernels when switching CR3 between per-node replica PGDs, and disabling PCID eliminated the corruption. Whether PCID is fundamentally incompatible with Hydra's CR3-switching scheme or whether the corruption stems from a separate interaction remains unconfirmed; the disable is defensive.
4. The original does not propagate writes to the user-half PGD under Page Table Isolation (KPTI). This fork adds `arch/x86/include/asm/hydra_pti.h` with `hydra_get_user_pgd_entry` and uses it from `pgtable_track_set_pgd` and `pgtable_track_set_p4d` so the user copy is kept consistent with the kernel copy on PTI kernels.
5. The original allocates replica PGDs at `PGD_ALLOCATION_ORDER` but does not adjust for PTI in all allocation paths. This fork threads a `hydra_pgd_alloc_order()` helper through `_pgd_alloc` and `_pgd_free`.
6. Because this fork propagates PMD clears to replicas (see item 2), clearing a replica PMD entry that points at a PTE page requires freeing that PTE page. Remote CPUs may still have TLB entries covering the range, so the page cannot be returned to the allocator until the subsequent TLB flush has completed. This fork adds `hydra_defer_pte_page_free`, a per-mm deferred free list drained by `hydra_drain_deferred_pages` after `flush_tlb_mm_node_range`. The original does not need this mechanism because it does not propagate PMD clears in the first place.
7. The original `mm_free_pgd` unconditionally loops over all `repl_pgd[i]` entries. When replication is disabled, `repl_pgd[i] == mm->pgd` for all `i`, so the primary PGD is freed `NUMA_NODE_COUNT` times. This fork skips replica slots that alias `mm->pgd` and frees the primary PGD exactly once.
8. The original's `check_in_repl_list` swaps `next_replica` pointers between two pages when they are not found in the same chain. This operation can split an existing multi-node chain into two disjoint chains and lose replicas. This fork replaces the ad-hoc logic with `hydra_link_page_to_replica_chain`, which walks the existing chain to check for membership and uses `cmpxchg` to atomically splice the new page in.
9. The original does not clear `next_replica` when a page is returned to the page allocator. Dangling replica pointers can survive across reallocation. This fork adds `hydra_break_chain`, called from `___pte_free_tlb`, `___pmd_free_tlb`, `pte_free`, and `pte_free_kernel`, and clears `next_replica` in `__alloc_pages` and `__free_pages`.
10. The original's `pgtable_repl_get_pte` ORs `pte_val` across all replicas, mixing physical frame numbers and flag bits from different entries when replicas have diverged. This fork masks to flag bits only and uses `pte_set_flags` so the returned PTE carries the master PFN with aggregated accessed and dirty bits.
11. The original writes the full PTE value (including the accessed bit) to every replica in `pgtable_repl_set_pte`. This fork writes `pte_mkold(pteval)` to replicas so that per-CPU accessed-bit tracking remains meaningful for page reclaim.
12. The original does not aggregate accessed and dirty bits across huge-page PMD replicas. This fork adds `hydra_get_pmd`, which ORs flag bits across all replica PMDs for a huge mapping, and uses it from `copy_huge_pmd`, `madvise_free_huge_pmd`, `remove_migration_pmd`, `page_vma_mkclean_one`, `touch_pmd`, `do_huge_pmd_numa_page`, and the MGLRU `walk_pmd_range*` paths.
13. The original disables transparent huge pages entirely by forcing `transhuge_vma_suitable` to return false, because the replication core cannot handle huge PMDs. This fork re-enables THP and implements huge-PMD replication, splitting, coherent wrprotect, establish, and young-bit testing via `hydra_pmdp_establish`, `pmdp_huge_get_and_clear`, `pmdp_set_wrprotect`, and `pmdp_test_and_clear_young`.
14. The original does not reconcile autoNUMA `PROT_NONE` hint faults with replicas. A hint-cleared master combined with a still-`PROT_NONE` replica, or the reverse, can cause the fault handler to loop between the two. This fork detects `pte_protnone` and `pmd_protnone` mismatches in `handle_pte_fault` and `__handle_mm_fault`, forces the fault onto the master, and re-propagates afterward.
15. The original does not track VMA ownership through `vm_area_dup`, `__split_vma`, or `copy_vma`. After fork, split, or mremap, child VMAs can end up with `master_pgd_node == 0` regardless of where the original allocation was made. This fork explicitly preserves `master_pgd_node` in all three paths and initializes it in `vm_area_alloc`.
16. The original's `dup_mmap` does not propagate `master_pgd_node` to child VMAs and does not enter a node scope for `copy_page_range`, so forked processes allocate their copied page tables on whatever node the forking CPU happens to run on. This fork propagates ownership and wraps `copy_page_range` in `hydra_enter_node_scope`.
17. The original's VMA merge functions (`can_vma_merge_before`, `can_vma_merge_after`) do not check `master_pgd_node`, so adjacent VMAs with different owners can be merged, after which the merged VMA has an ambiguous owner. This fork refuses the merge when ownership differs and threads `master_pgd_node` through the 7.0 `vma_merge_struct` and the `VMG_*_INIT` macros so every caller propagates ownership consistently.
18. The original's `mprotect` path sets `tlb.collect_nodemask = 1` after `tlb_finish_mmu` has already been called, so the nodemask is never actually used by the TLB flush. This fork sets `tlb.vma` (the 7.0 mmu_gather field used by `flush_tlb_vma_range`) inside the loop and relies on the per-VMA flush path for precise nodemask collection.
19. The original's `flush_tlb_vma_range` only handles single-PMD ranges and falls back to a full broadcast for anything larger. This fork walks the range PMD by PMD, collects the union of replica nodes from every page table page touched, and issues a filtered flush for multi-PMD ranges as well.
20. The original does not invalidate replica PMDs when a huge page is split. Stale huge PMD entries on replica nodes survive the split and subsequent PTE-level modifications diverge. This fork redirects `__split_huge_pmd` to the master PMD, and `hydra_pmdp_establish` and `pgtable_track_set_pmd` walk replicas and tear down stale entries consistently.
21. The original does not integrate with `page_table_check`. This fork calls `page_table_check_pte_clear` and `page_table_check_pmd_clear` from `ptep_get_and_clear` and `pmdp_huge_get_and_clear` so `CONFIG_PAGE_TABLE_CHECK` kernels remain functional.
22. The original's `ptep_test_and_clear_young` and `pmdp_test_and_clear_young` walk replicas with an ad-hoc counter that treats any iteration count above `NUMA_NODE_COUNT * 2` as a chain corruption and kills the current process via `SIGKILL`. False positives are possible on contended chains. This fork uses the `for_each_replica` iterator with a bounded loop and without fatal signal delivery.
23. The original's `print_bad_pte`, `follow_pte`, `unmap_page_range`, `copy_page_range`, `page_vma_mapped_walk`, and `mm_find_pmd` paths unconditionally call `pgd_offset(mm, addr)`, which uses `mm->pgd`. On a replica-enabled mm where the current CPU is running on a non-owner node, this can diverge from the active CR3. This fork routes all these callers through `pgd_offset_node(mm, addr, vma->master_pgd_node)` when replication is enabled, including the new 7.0 helpers `folio_walk_start`, `follow_pfnmap_start`, `__print_bad_page_map_pgtable`, and the MGLRU `walk_pgd_range` path.
24. The original's `follow_pte` signature takes `(mm, address, ptepp, ptlp)` and cannot be redirected to an owner node because it lacks a VMA. This fork extends `get_locked_pte` and `huge_pte_offset` to take a VMA and updates all callers (including the 7.0 `arch/powerpc/lib/code-patching.c`, `arch/s390/mm/gmap_helpers.c`, `arch/x86/kernel/ldt.c`, `arch/x86/kernel/alternative.c`, `arch/x86/mm/init.c`, and the generic `hugetlb_walk` helper).
25. The original's `hva_to_pfn_remapped` in KVM and `follow_fault_pfn` in vfio therefore walk the wrong PGD for replicated mms. This fork fixes both through the extended helper signatures.
26. The original uses a process-global `static bool pgtable_repl_initialized` set without locking from `init_lazy_pgtabls_repl`. This fork removes the global flag and uses per-mm state exclusively.
27. The original's `handle_pte_fault` recursion via `__handle_mm_fault(..., use_master=1)` can re-enter itself without a bound if the master fault itself triggers a replica fault. This fork gates recursion on `has_recursed` and ensures `try_lazy_repl` is only entered once per fault.
28. The original's `copy_page_range` has a printk that reports the destination node as "from" and the source node as "to", reversing source and destination in kernel logs. This fork removes the misleading prints.
29. The original sets `master_pgd_node` only in `mmap_region`. VMAs created by other paths (brk, stack expansion, special mappings) inherit an uninitialized value. This fork initializes `master_pgd_node` in `vm_area_alloc` and preserves it through all VMA lifecycle operations.
30. The original allocates all replica PGDs unconditionally in `mm_init`, even when `lazy_repl_enabled` is false. This wastes one PGD-order allocation per online node for every process on the system, including kernel threads. This fork initializes `repl_pgd[i] = mm->pgd` for all `i` and only allocates real replicas when `hydra_enable_replication` is called.
31. The 7.0 `switch_mm_irqs_off` rewrite introduced `mm_needs_global_asid`, `broadcast_tlb_flush`, and the `ns` (new state) struct. The original had no awareness of any of this. This fork threads the per-node `next_pgd = next->repl_pgd[numa_node_id()]` selection into the new code path and loads it via `load_new_mm_cr3(next_pgd, ns.asid, new_lam, ...)` while preserving the global ASID and lazy-TLB handling.
32. The 7.0 `___pte_free_tlb` / `___pmd_free_tlb` path calls `pagetable_dtor` separately from `tlb_remove_ptdesc`. The original's free path does not exist in this form. This fork invokes `pagetable_dtor` explicitly before returning a page to the hydra cache or freeing it, so ptdesc bookkeeping stays consistent.
33. The 7.0 `__pte_alloc_one_kernel_noprof` / `__pte_alloc_one_noprof` / `pmd_alloc_one_noprof` / `__pud_alloc_one_noprof` / `__p4d_alloc_one_noprof` allocator generation is new. This fork reimplements each to pull from the hydra per-node cache first, fall back to `alloc_pages_node(..., __GFP_THISNODE)` when replication is active, and correctly run the corresponding `pagetable_*_ctor` on the fetched page.

### Feature additions and infrastructure

34. This fork adds a page table page cache (`struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT]` in `mm/hydra_stats.c`) with per-node free lists protected by a spinlock. `hydra_cache_push` and `hydra_cache_pop` are called from all page table allocation and free paths (PTE, PMD, PUD, P4D, PGD). A `PG_hydra_from_cache` page flag marks pages that came from the cache so they can be returned on free.
35. This fork adds a `/proc/hydra/` directory with nine entries: `status`, `cache`, `tlbflush_opt`, `repl_order`, `verify`, `history`, `walk`, `thp_check`, and `numa_check`. The original exposes no runtime introspection beyond the `hydra_dump_ll` syscall.
36. This fork adds a `hydra` kernel boot parameter that enables replication for all new user processes. Children inherit replication from their parents. The original requires explicit syscalls to enable replication per process.
37. This fork adds runtime enablement of replication on already-running processes via `hydra_enable_replication`. The function migrates existing page tables to the owner node using `migrate_pgtables_to_node`, allocates replica PGDs, and issues an IPI (`hydra_reload_cr3`) to all CPUs in the mm's cpumask to reload CR3. The original can only enable replication at mm creation time.
38. This fork adds per-mm statistics (`hydra_tlb_shootdowns_total`, `hydra_tlb_shootdowns_sent`, `hydra_tlb_shootdowns_saved`, `hydra_repl_pte_faults`, `hydra_repl_ptes_copied`, `hydra_repl_hugepmd_faults`, `hydra_repl_hugepmd_copied`) and an `NUMA_NODE_COUNT x NUMA_NODE_COUNT` migration matrix counting autoNUMA migrations between nodes. All are exposed through `/proc/hydra/status`.
39. This fork adds a history list (`hydra_history_list`) that records per-process statistics on mm teardown via `hydra_record_exit`, allowing inspection of completed workloads through `/proc/hydra/history`.
40. This fork adds a fault-time verification mode (`sysctl_hydra_verify_enabled`) that walks the active CR3's page tables on every fault and compares the node of each page table page against the expected owner, panicking on mismatch. This was the primary tool used to find many of the bugs in the original. Controlled via `/proc/hydra/verify`.
41. This fork adds `/proc/hydra/walk`, which dumps all per-node page table entries for a given PID and virtual address including replica chain contents. `/proc/hydra/thp_check` performs a whole-process sweep for huge-page inconsistencies. `/proc/hydra/numa_check` performs a whole-process sweep for autoNUMA `PROT_NONE` divergences between master and replicas.
42. This fork extends the TLB flush optimization. The original supports modes 0, 1, and 2 (off, precise single-page, all-or-none). This fork adds mode 3 and changes the default from 0 to 1 so the optimization is active out of the box.
43. This fork adds a `repl_order` tunable (0 through 9) exposed via `/proc/hydra/repl_order` that controls how many PTEs are prefetched into a replica on a replication fault. The original has the sysctl but no proc interface to change it.
44. This fork adds `prepare.sh`, which installs build dependencies, copies the running kernel config, detects the NUMA node count with `numactl --hardware`, sets `CONFIG_PARAVIRT_XXL=y` and `CONFIG_HYDRA_NUMA_NODE_COUNT` to the detected value, and runs `make olddefconfig`.
45. This fork adds `install.sh`, which removes previous hydra kernel images from `/boot`, pulls the latest tree, builds, and installs modules and kernel image.
46. This fork adds `CONFIG_PARAVIRT_XXL` as a user-selectable Kconfig option with a help text explaining its role in the replication hooks. The original leaves `PARAVIRT_XXL` as a silent option.
47. This fork makes `mm->lazy_repl_enabled` the single control for virtual address segregation in `arch_get_unmapped_area_topdown`. The original has a separate `va_segregation_mode` with modes 0, 1, and 2 controlled by a dedicated syscall 403, which can desync from the replication state.
48. This fork adds a `pt_owner_mm` pointer on `struct page` to identify which mm a page table page belongs to, used by the deferred free path to decrement the correct `mm_nr_ptes` counter when a PTE page is torn down off the fault path.
49. This fork adds `hydra_find_pte` and `hydra_collect_repl_nodes` helpers in `include/linux/hydra_util.h` used by the range TLB flush path and by verification code.
50. This fork adds a `for_each_replica` iteration macro that bounds replica walks and replaces open-coded counter loops scattered throughout the original.
51. This fork adds `hydra_enter_node_scope` and `hydra_exit_node_scope` helpers that save and restore `current->hydra_fault_target_node`, used to pin allocations to a specific node during fork, remap, and insert-page paths.
52. This fork adds an `hydra_fault_target_node` field on `task_struct` initialized to `-1` in `init_task`, used by `hydra_alloc_node` to select an allocation node during recursive faults.
53. This fork adds `khugepaged` integration: `find_pmd_or_thp_or_none`, `check_pmd_still_valid`, `collapse_huge_page`, `hpage_collapse_scan_pmd`, and `retract_page_tables` are updated to accept a VMA and route PGD lookups through `pgd_offset_node` so collapse operates on the correct per-node page tables.
54. This fork updates `hugetlb` (`huge_pmd_unshare`, `huge_pte_alloc`, `huge_pte_offset`) and `migrate_device` (`migrate_vma_insert_page`) to use `pgd_offset_node` when replication is enabled. The 7.0 `hugetlb_walk` helper is threaded through the new VMA-aware `huge_pte_offset` signature.
55. This fork updates `dev_pagemap_mapping_shift` in `memory-failure` to use `pgd_offset_node`.
56. This fork updates `unuse_vma` in `swapfile` to use `pgd_offset_node`.
57. This fork updates `userfaultfd_must_wait` and `move_pages` to use `pgd_offset_node` via the VMA, fixing userfaultfd on replicated mms under the 7.0 `softleaf`/migration entry refactor.
58. This fork updates `mm_find_pmd` (used by KSM, rmap, khugepaged) to accept a VMA and route through the owner's PGD.
59. This fork updates `ksm.c`'s `replace_page` and `split_huge_pmd_address` to pass the VMA through `mm_find_pmd`.
60. This fork updates `move_page_tables` in `mremap.c` to enter the destination VMA's node scope before walking and allocating, and threads the owner node through `get_old_pud`, `get_old_pmd`, `alloc_new_pud`, and `alloc_new_pmd`.
61. This fork updates `insert_page`, `insert_pages`, `insert_pfn`, and `remap_pfn_range_internal` to use the VMA-aware `get_locked_pte` and `pgd_offset_node` so driver-inserted mappings land on the owner's page tables.
62. This fork adds `hydra_free_pgd_tree` in `exit_mmap` to tear down all replica PGDs cleanly on process exit, walking each replica's tree and freeing PTE, PMD, PUD, and P4D pages. The 7.0 `free_pgtables` takes an `unmap_desc` rather than a `maple_tree` and is handled separately from the replica teardown.
63. This fork adds a final `hydra_unlink_all_replica_chains` sweep in `exit_mmap` before teardown so no chain pointer outlives the pages it references.
64. This fork initializes `hydra_deferred_lock` and `hydra_deferred_pages` in `mm_init` and drains the list in `__mmput`.

### Defaults and usability

65. The default for `sysctl_hydra_tlbflush_opt` is 1, so the TLB flush optimization (the main performance benefit for update-heavy workloads described in Section 3.5 of the paper) is on by default.
66. The default for `sysctl_hydra_repl_order` is 9 (up to 512 PTEs per fault). This matches the authors' recommendation in Section 3.4 of the paper and matches the prefetching behavior used in the paper's evaluation, where the authors observe no measurable downside from prefetching within a page table page.
67. This fork adds a `/proc/hydra/cache` write interface that accepts `-1` to drain all caches, `-2` to reset statistics, and a positive value `N` to pre-populate `N` pages per node.
68. This fork adds `CONFIG_PARAVIRT_XXL` and `CONFIG_HYDRA_NUMA_NODE_COUNT` guidance in the build scripts so users do not need to navigate menuconfig manually.

### Debug output cleanup

69. The original contains a number of commented-out and active debug prints in hot paths (`clone_pgd_range`, `copy_page_range`, `do_fault`, `do_anonymous_page`, `do_swap_page`, `do_wp_page`, `handle_pte_fault`, the paravirt TLB flush path). This fork removes or gates them.
70. The original's `do_hydra_dump_ll` prints kernel virtual addresses of PTEs and page structures without `%pK`, exposing kernel pointers to unprivileged users via the syscall. This fork removes that syscall and replaces it with the `/proc/hydra/walk` interface, which is root-readable.

## Prerequisites

Tested on Ubuntu 24.04 LTS. Run the prepare script to install the packages required for kernel compilation and generate the kernel configuration:

```bash
./prepare.sh
```

This installs build tools (`build-essential`, `bc`, `bison`, `flex`, etc.), development libraries (`libelf-dev`, `libssl-dev`, `libncurses-dev`, `libnuma-dev`, etc.), and other dependencies needed by the kernel build system. See `prepare.sh` for the full list.

After installing packages, the script copies the running kernel's config from `/boot` into `.config`, detects the number of NUMA nodes using `numactl --hardware`, sets `PARAVIRT_XXL=y` and `HYDRA_NUMA_NODE_COUNT` to the detected value, and runs `make olddefconfig` to resolve all remaining new options to their defaults.

## Kernel Configuration

Two kernel config options are required and are set automatically by `prepare.sh`:

**PARAVIRT_XXL** must be enabled. Hydra hooks into the paravirt_ops interface to intercept all page table writes (`set_pte`, `set_pmd`, `set_pud`, `set_p4d`, `set_pgd`) and broadcast them to replicas. Without this, the replication hooks are not compiled in.

**HYDRA_NUMA_NODE_COUNT** must match the number of NUMA nodes on your system. Check with `numactl --hardware`. The kernel will refuse to boot if there is a mismatch, printing an error message with the expected value. This controls the size of per-mm replica pointer arrays and the per-node page table cache.

If you need to adjust options manually after running `prepare.sh`, use `make menuconfig`:

```
make menuconfig
# Processor type and features -> Paravirtualization -> PARAVIRT_XXL: set to Y
# Processor type and features -> NUMA -> HYDRA_NUMA_NODE_COUNT: set to your node count
```

## Building

After running `prepare.sh`, build the kernel:

```bash
make -j$(nproc)
```

The resulting kernel identifies as `7.0.0-rc6-hydra`.

## Installation

After building the kernel, run the install script to install it:

```bash
./install.sh
```

This script removes any previously installed `7.0.0-hydra` kernel images and initrd files from `/boot`, pulls the latest changes from the repository, builds the kernel, and installs the modules and kernel image. You will need to reboot into the new kernel after installation. Make sure your bootloader (e.g. GRUB) is updated to include the new entry, which `make install` typically handles automatically.

If you are building for the first time rather than updating an existing installation, the removal step at the beginning of the script will simply have nothing to remove.

## Page Table Isolation (PTI / Meltdown mitigation)

This fork supports kernels running with Page Table Isolation (KPTI) enabled. When PTI is active, each PGD allocation is two pages (kernel and user halves). Replica PGDs are allocated at the same order, and writes to user-half PGD entries are propagated alongside their kernel-half counterparts. PTI can remain enabled or disabled; the fork adapts at runtime.

Note that PCID and INVPCID are disabled at boot to avoid stale TLB tags when switching between replica CR3s.

## Transparent Huge Pages

This fork supports transparent huge pages regardless of the defrag policy (`always`, `madvise`, `never`, or `defer+madvise`). THP split operations free stale PTE replicas before repopulating, and huge PMD entries are broadcast to all replicas on write. The `hydra_get_pmd` function gathers accessed and dirty bits across replicas for huge page fault handling and MGLRU aging.

## Quick Start: Boot-Time Replication

The simplest way to use this fork is to pass the `hydra` boot parameter. This auto-enables page table replication for all new user processes across all online NUMA nodes. Child processes inherit replication from their parents.

```
# In your bootloader config (e.g. GRUB_CMDLINE_LINUX):
hydra
```

Replication can also be enabled from the command line using [numactl-hydra](https://github.com/julianw1010/numactl-hydra):

```bash
numactl-hydra -r all ./my_application
```

## Syscall Interface

Replication can be enabled per process via the syscalls added to the 64-bit syscall table:

```c
#include <unistd.h>
#include <sys/syscall.h>

// Enable replication (syscall 400)
syscall(400, 1, NULL, 0);

// Query replication status (syscall 401)
syscall(401, &policy, nmask, maxnode, addr, flags);
```

## procfs Interface

All runtime state is exposed under `/proc/hydra/`.

### `/proc/hydra/status`

Shows per-process statistics including TLB shootdown counts (total, sent, saved), PTE replication faults, huge PMD replication faults, and per-node VMA ownership distribution.

- Write `-1` to reset all counters for all processes.

### `/proc/hydra/cache`

Shows per-node page table cache statistics (page count, hits, misses, returns).

- Write `-1` to drain all caches.
- Write `-2` to reset statistics.
- Write a positive number `N` to pre-populate `N` pages per node.

### `/proc/hydra/tlbflush_opt`

Controls the TLB flush optimization mode. Reading shows the current mode.

- `0` off, no optimization.
- `1` precise node-mask filtering, single-PMD ranges only.
- `2` precise node-mask filtering, all ranges.
- `3` all-or-none filtering.

### `/proc/hydra/repl_order`

Controls how many PTEs are copied per replication fault. The value is a log2 count, so `0` copies a single PTE and `9` (the default) copies up to 512 PTEs from the same page table page. Write a value between `0` and `9`.

### `/proc/hydra/verify`

Enables or disables fault-time consistency checks between replicas.

- Write `1` to enable, `0` to disable.

When enabled, the kernel verifies that every page table page in a replica is located on the correct NUMA node and that every replica PTE has a corresponding master PTE.

### `/proc/hydra/walk`, `/proc/hydra/thp_check`, `/proc/hydra/numa_check`

Debugging interfaces for inspecting replica consistency for a given PID. Write a PID (and for `walk`, a hex virtual address) and read back a dump.

## License

GPL-2.0 (same as the Linux kernel).
