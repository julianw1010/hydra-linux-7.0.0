# Hydra - NUMA-Aware Page Table Replication for Linux

A Linux 6.4 kernel patch that replicates user-space page tables across NUMA nodes so each node walks a local copy during TLB misses. Based on the Hydra paper by Gao et al. (USENIX ATC '24).

## Prerequisites

This project has been tested on Ubuntu 24.04.04 LTS. Run the prepare script to install the packages required for kernel compilation and generate the kernel configuration:

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

## Installation

After building the kernel, run the install script to install it:

```bash
./install.sh
```

This script removes any previously installed `6.4.0-hydra` kernel images and initrd files from `/boot`, pulls the latest changes from the repository, builds the kernel, and installs the modules and kernel image. You will need to reboot into the new kernel after installation. Make sure your bootloader (e.g. GRUB) is updated to include the new entry, which `make install` typically handles automatically.

If you are building for the first time rather than updating an existing installation, the removal step at the beginning of the script will simply have nothing to remove.

## Page Table Isolation (PTI / Meltdown mitigation)

Hydra supports kernels running with Page Table Isolation (KPTI) enabled. When PTI is active, each PGD allocation is two pages (kernel and user halves). Replica PGDs are allocated at the same order, and writes to user-half PGD entries are propagated alongside their kernel-half counterparts. PTI can remain enabled or disabled; Hydra adapts at runtime.

Note that PCID and INVPCID are disabled at boot to simplify CR3 switching between replicas.

## Transparent Huge Pages

Hydra works with transparent huge pages regardless of the defrag policy (`always`, `madvise`, `never`, or `defer+madvise`). THP split operations correctly free stale PTE replicas before repopulating, and huge PMD entries are broadcast to all replicas on write. The `hydra_get_pmd` function gathers accessed/dirty bits across replicas for correct huge page fault handling.

## Quick Start: Boot-Time Replication

The simplest way to use Hydra is to pass the `hydra` boot parameter. This auto-enables page table replication for all new user processes across all online NUMA nodes. Child processes inherit replication from their parents.

```
# In your bootloader config (e.g. GRUB_CMDLINE_LINUX):
hydra
```

Replication can also be enabled from the command line using [numactl-hydra](https://github.com/julianw1010/numactl-hydra):

```bash
numactl-hydra -r all ./my_application
```

## Syscall Interface

Replication can be enabled or disabled per process via two new syscalls added to the 64-bit syscall table:

```c
#include <unistd.h>
#include <sys/syscall.h>

// Enable replication (syscall 400)
syscall(400, 1, NULL, 0);

// Query replication status (syscall 401)
syscall(401, &policy, nmask, maxnode, addr, flags);

// Set VA segregation mode (syscall 403)
// 0 = off, 1 = per-node, 2 = per-cpu
syscall(403, 1);
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

- `0` - off, no optimization.
- `1` - precise node-mask filtering, single-PMD ranges only.
- `2` - precise node-mask filtering, all ranges.
- `3` - all-or-none filtering.

### `/proc/hydra/repl_order`

Controls how many PTEs are copied per replication fault. The value is a log2 count, so `0` copies a single PTE and `9` (the default) copies up to 512 PTEs from the same page table page. Write a value between `0` and `9`.

### `/proc/hydra/verify`

Enables or disables fault-time consistency checks between replicas.

- Write `1` to enable, `0` to disable.

When enabled, the kernel verifies that every page table page in a replica is located on the correct NUMA node and that every replica PTE has a corresponding master PTE.

## References

This implementation is based on:

Gao, B., Kang, Q., Tee, H.-W., Chu, K. T. N., Sanaee, A., & Jevdjic, D. (2024). *Scalable and Effective Page-table and TLB Management on NUMA Systems.* USENIX ATC '24, pp. 445-461. [https://www.usenix.org/conference/atc24/presentation/gao-bin-scalable](https://www.usenix.org/conference/atc24/presentation/gao-bin-scalable)

## License

GPL-2.0 (same as the Linux kernel).
