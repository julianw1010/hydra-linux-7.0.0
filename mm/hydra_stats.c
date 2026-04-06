#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/hydra_util.h>
#include <linux/uaccess.h>
#include <linux/rwlock.h>
#include <linux/timekeeping.h>

extern int sysctl_hydra_tlbflush_opt;
extern int sysctl_hydra_repl_order;

int sysctl_hydra_verify_enabled __read_mostly = 0;
EXPORT_SYMBOL(sysctl_hydra_verify_enabled);

struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT] = {
	[0 ... NUMA_NODE_COUNT - 1] = {
		.lock		= __SPIN_LOCK_UNLOCKED(hydra_cache.lock),
		.head		= NULL,
		.count		= ATOMIC_INIT(0),
		.hits		= ATOMIC64_INIT(0),
		.misses		= ATOMIC64_INIT(0),
		.returns	= ATOMIC64_INIT(0),
		.evictions	= ATOMIC64_INIT(0),
	}
};
EXPORT_SYMBOL(hydra_cache);

static struct proc_dir_entry *hydra_proc_dir;

struct hydra_history_entry {
	struct list_head list;
	char comm[TASK_COMM_LEN];
	pid_t pid;
	u64 exit_time_ns;
	long tlb_total;
	long tlb_sent;
	long tlb_saved;
	long pte_faults;
	long ptes_copied;
	long hugepmd_faults;
	long hugepmd_copied;
	long migration_matrix[NUMA_NODE_COUNT][NUMA_NODE_COUNT];
};

static LIST_HEAD(hydra_history_list);
static int hydra_history_count;
static DEFINE_SPINLOCK(hydra_history_lock);

void hydra_record_exit(struct mm_struct *mm, const char *comm, pid_t pid)
{
	unsigned long flags;
	struct hydra_history_entry *e;
	int src, dst;

	if (!mm->lazy_repl_enabled)
		return;

	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return;

	strncpy(e->comm, comm, TASK_COMM_LEN - 1);
	e->comm[TASK_COMM_LEN - 1] = '\0';
	e->pid = pid;
	e->exit_time_ns = ktime_get_real_ns();
	e->tlb_total = atomic_long_read(&mm->hydra_tlb_shootdowns_total);
	e->tlb_sent = atomic_long_read(&mm->hydra_tlb_shootdowns_sent);
	e->tlb_saved = atomic_long_read(&mm->hydra_tlb_shootdowns_saved);
	e->pte_faults = atomic_long_read(&mm->hydra_repl_pte_faults);
	e->ptes_copied = atomic_long_read(&mm->hydra_repl_ptes_copied);
	e->hugepmd_faults = atomic_long_read(&mm->hydra_repl_hugepmd_faults);
	e->hugepmd_copied = atomic_long_read(&mm->hydra_repl_hugepmd_copied);

	for (src = 0; src < NUMA_NODE_COUNT; src++)
		for (dst = 0; dst < NUMA_NODE_COUNT; dst++)
			e->migration_matrix[src][dst] = atomic_long_read(&mm->hydra_migration_matrix[src][dst]);

	spin_lock_irqsave(&hydra_history_lock, flags);
	list_add_tail(&e->list, &hydra_history_list);
	hydra_history_count++;
	spin_unlock_irqrestore(&hydra_history_lock, flags);
}
EXPORT_SYMBOL(hydra_record_exit);

static int hydra_tlbflush_opt_show(struct seq_file *m, void *v)
{
	switch (sysctl_hydra_tlbflush_opt) {
	case 0:
		seq_printf(m, "0 (off)\n");
		break;
	case 1:
		seq_printf(m, "1 (precise, single-page only)\n");
		break;
	case 2:
		seq_printf(m, "2 (precise, always)\n");
		break;
	case 3:
		seq_printf(m, "3 (all-or-none)\n");
		break;
	default:
		seq_printf(m, "%d (unknown)\n", sysctl_hydra_tlbflush_opt);
		break;
	}
	return 0;
}

static ssize_t hydra_tlbflush_opt_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	char kbuf[16];
	long val;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtol(kbuf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 3)
		return -EINVAL;

	sysctl_hydra_tlbflush_opt = val;
	return count;
}

static int hydra_tlbflush_opt_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_tlbflush_opt_show, NULL);
}

static const struct proc_ops hydra_tlbflush_opt_ops = {
	.proc_open = hydra_tlbflush_opt_open,
	.proc_read = seq_read,
	.proc_write = hydra_tlbflush_opt_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int hydra_repl_order_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d (%lu PTEs or 1 huge PMD per fault)\n",
		   sysctl_hydra_repl_order,
		   sysctl_hydra_repl_order > 0 ? 1UL << sysctl_hydra_repl_order : 1UL);
	return 0;
}

static ssize_t hydra_repl_order_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char kbuf[16];
	long val;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtol(kbuf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 9)
		return -EINVAL;

	sysctl_hydra_repl_order = val;
	return count;
}

static int hydra_repl_order_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_repl_order_show, NULL);
}

static const struct proc_ops hydra_repl_order_ops = {
	.proc_open = hydra_repl_order_open,
	.proc_read = seq_read,
	.proc_write = hydra_repl_order_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int hydra_cache_show(struct seq_file *m, void *v)
{
	int node;

	for_each_online_node(node) {
		if (node >= NUMA_NODE_COUNT)
			continue;
		seq_printf(m, "Node %d: %d\n", node, atomic_read(&hydra_cache[node].count));
	}

	return 0;
}

static ssize_t hydra_cache_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char kbuf[16];
	long val;
	int node;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtol(kbuf, 10, &val))
		return -EINVAL;

	if (val == -1) {
		int freed = hydra_cache_drain_all();
		pr_info("[HYDRA]: Drained %d pages from cache\n", freed);
	} else if (val == -2) {
		for (node = 0; node < NUMA_NODE_COUNT; node++) {
			struct hydra_cache_head *cache = &hydra_cache[node];
			atomic64_set(&cache->hits, 0);
			atomic64_set(&cache->misses, 0);
			atomic64_set(&cache->returns, 0);
			atomic64_set(&cache->evictions, 0);
		}
		pr_info("[HYDRA]: Cache statistics reset\n");
	} else if (val > 0) {
		int total_added = 0;
		int target = (int)val;

		for_each_online_node(node) {
			struct hydra_cache_head *cache;
			int current_count, to_add, added = 0;

			if (node >= NUMA_NODE_COUNT)
				continue;

			cache = &hydra_cache[node];
			current_count = atomic_read(&cache->count);
			to_add = target - current_count;

			while (added < to_add) {
				struct page *page;
				nodemask_t nm = NODE_MASK_NONE;

				node_set(node, nm);
				page = __alloc_pages(GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE,
						     0, node, &nm);
				if (!page)
					break;

				page->next_replica = NULL;

				if (!hydra_cache_push(page, node, 0)) {
					__free_page(page);
					break;
				}
				added++;
				total_added++;
			}
		}
		pr_info("[HYDRA]: Populated cache with %d pages\n", total_added);
	} else {
		return -EINVAL;
	}

	return count;
}

static int hydra_cache_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_cache_show, NULL);
}

static const struct proc_ops hydra_cache_ops = {
	.proc_open = hydra_cache_open,
	.proc_read = seq_read,
	.proc_write = hydra_cache_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static void hydra_status_print_header(struct seq_file *m, int nr_online,
				      int *online_nodes)
{
	seq_printf(m,
		"======================================================\n"
		"                  HYDRA STATUS REPORT                  \n"
		"======================================================\n\n");

	seq_printf(m, "  Online nodes:       %d\n", nr_online);

	switch (sysctl_hydra_tlbflush_opt) {
	case 0:
		seq_printf(m, "  TLB flush opt:      off\n");
		break;
	case 1:
		seq_printf(m, "  TLB flush opt:      precise (single-PMD only)\n");
		break;
	case 2:
		seq_printf(m, "  TLB flush opt:      precise (all ranges)\n");
		break;
	case 3:
		seq_printf(m, "  TLB flush opt:      all-or-none\n");
		break;
	default:
		seq_printf(m, "  TLB flush opt:      %d (unknown)\n",
			   sysctl_hydra_tlbflush_opt);
		break;
	}

	seq_printf(m, "  Replication order:  %d (%lu PTEs per fault)\n",
		   sysctl_hydra_repl_order,
		   sysctl_hydra_repl_order > 0
			   ? 1UL << sysctl_hydra_repl_order : 1UL);
	seq_printf(m, "  Auto-enable:        %s\n\n",
		   sysctl_hydra_auto_enable ? "yes" : "no");
}

static void hydra_status_print_cache(struct seq_file *m, int nr_online,
				     int *online_nodes)
{
	int i, total_pages = 0;
	long total_hits = 0, total_misses = 0, total_returns = 0;

	seq_printf(m, "  PAGE TABLE CACHE\n");
	seq_printf(m, "  %4s %8s %8s %8s %8s\n",
		   "Node", "Pages", "Hits", "Misses", "Returns");
	seq_printf(m, "  ---- -------- -------- -------- --------\n");

	for (i = 0; i < nr_online; i++) {
		int n = online_nodes[i];
		int count = atomic_read(&hydra_cache[n].count);
		long hits = atomic64_read(&hydra_cache[n].hits);
		long misses = atomic64_read(&hydra_cache[n].misses);
		long returns = atomic64_read(&hydra_cache[n].returns);

		total_pages += count;
		total_hits += hits;
		total_misses += misses;
		total_returns += returns;

		seq_printf(m, "  %4d %8d %8ld %8ld %8ld\n",
			   n, count, hits, misses, returns);
	}

	seq_printf(m, "  ---- -------- -------- -------- --------\n");
	seq_printf(m, "  Tot. %8d %8ld %8ld %8ld\n",
		   total_pages, total_hits, total_misses, total_returns);
	seq_printf(m, "       (%lu KB)\n\n",
		   (unsigned long)total_pages * PAGE_SIZE / 1024);
}

static void hydra_status_print_tlb_table(struct seq_file *m,
					 struct mm_struct *mm)
{
	long tlb_total = atomic_long_read(&mm->hydra_tlb_shootdowns_total);
	long tlb_sent = atomic_long_read(&mm->hydra_tlb_shootdowns_sent);
	long tlb_saved = atomic_long_read(&mm->hydra_tlb_shootdowns_saved);
	long pct = (tlb_total > 0) ? (tlb_saved * 100) / tlb_total : 0;

	seq_printf(m, "    TLB Shootdowns\n");
	seq_printf(m, "    %10s %10s %10s %7s\n",
		   "Total", "Sent", "Saved", "Saved%");
	seq_printf(m, "    ---------- ---------- ---------- -------\n");
	seq_printf(m, "    %10ld %10ld %10ld %5ld%%\n\n",
		   tlb_total, tlb_sent, tlb_saved, pct);
}

static void hydra_status_print_repl_table(struct seq_file *m,
					  struct mm_struct *mm)
{
	long pte_faults = atomic_long_read(&mm->hydra_repl_pte_faults);
	long ptes_copied = atomic_long_read(&mm->hydra_repl_ptes_copied);
	long hpmd_faults = atomic_long_read(&mm->hydra_repl_hugepmd_faults);
	long hpmd_copied = atomic_long_read(&mm->hydra_repl_hugepmd_copied);
	long avg_pte = (pte_faults > 0) ? ptes_copied / pte_faults : 0;
	long avg_hpmd = (hpmd_faults > 0) ? hpmd_copied / hpmd_faults : 0;

	seq_printf(m, "    Replication Faults\n");
	seq_printf(m, "    %-8s %10s %10s %10s\n",
		   "Type", "Faults", "Copied", "Avg/fault");
	seq_printf(m, "    -------- ---------- ---------- ----------\n");
	seq_printf(m, "    %-8s %10ld %10ld %10ld\n",
		   "PTE", pte_faults, ptes_copied, avg_pte);
	seq_printf(m, "    %-8s %10ld %10ld %10ld\n\n",
		   "HugePMD", hpmd_faults, hpmd_copied, avg_hpmd);
}

static void hydra_status_print_vma_dist(struct seq_file *m,
					struct mm_struct *mm,
					int nr_online, int *online_nodes)
{
	struct vm_area_struct *vma;
	unsigned long vma_counts[NUMA_NODE_COUNT] = {};
	unsigned long nr_vmas = 0;
	unsigned long bar_max;
	bool got_lock;
	int i;

	got_lock = mmap_read_trylock(mm);
	if (!got_lock) {
		seq_printf(m, "    VMA Distribution: (lock busy)\n\n");
		return;
	}

	{
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			unsigned long n = vma->master_pgd_node;
			if (n < NUMA_NODE_COUNT)
				vma_counts[n]++;
			nr_vmas++;
		}
	}
	mmap_read_unlock(mm);

	seq_printf(m, "    VMA Distribution (%lu total)\n", nr_vmas);
	seq_printf(m, "    %4s %6s %5s  %s\n",
		   "Node", "VMAs", "%", "");
	seq_printf(m, "    ---- ------ -----  --------------------\n");

	bar_max = 0;
	for (i = 0; i < nr_online; i++) {
		if (vma_counts[online_nodes[i]] > bar_max)
			bar_max = vma_counts[online_nodes[i]];
	}

	for (i = 0; i < nr_online; i++) {
		int n = online_nodes[i];
		unsigned long cnt = vma_counts[n];
		int pct = nr_vmas > 0 ? (int)((cnt * 100) / nr_vmas) : 0;
		int bar_len = bar_max > 0 ? (int)((cnt * 20) / bar_max) : 0;
		int j;

		seq_printf(m, "    %4d %6lu %4d%%  ", n, cnt, pct);
		for (j = 0; j < bar_len; j++)
			seq_printf(m, "#");
		seq_printf(m, "\n");
	}
	seq_printf(m, "\n");
}

static void hydra_status_print_migration(struct seq_file *m,
					 struct mm_struct *mm,
					 int nr_online, int *online_nodes)
{
	long matrix[NUMA_NODE_COUNT][NUMA_NODE_COUNT];
	long row_sum[NUMA_NODE_COUNT];
	long matrix_total = 0;
	int i, j;
	int cols;

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		row_sum[i] = 0;
		for (j = 0; j < NUMA_NODE_COUNT; j++) {
			matrix[i][j] = atomic_long_read(
				&mm->hydra_migration_matrix[i][j]);
			matrix_total += matrix[i][j];
			row_sum[i] += matrix[i][j];
		}
	}

	if (matrix_total == 0)
		return;

	cols = nr_online;
	if (cols > 4) {
		seq_printf(m, "    Migration (%ld pages migrated)\n", matrix_total);
		seq_printf(m, "    %4s %10s  Top destinations\n",
			   "From", "Total");
		seq_printf(m, "    ---- ----------  "
			   "-----------------------------\n");

		for (i = 0; i < nr_online; i++) {
			int src = online_nodes[i];
			int best[3] = {-1, -1, -1};
			long best_v[3] = {0, 0, 0};

			if (row_sum[src] == 0)
				continue;

			for (j = 0; j < nr_online; j++) {
				int dst = online_nodes[j];
				long v = matrix[src][dst];
				if (v <= 0)
					continue;
				if (v > best_v[0]) {
					best_v[2] = best_v[1]; best[2] = best[1];
					best_v[1] = best_v[0]; best[1] = best[0];
					best_v[0] = v; best[0] = dst;
				} else if (v > best_v[1]) {
					best_v[2] = best_v[1]; best[2] = best[1];
					best_v[1] = v; best[1] = dst;
				} else if (v > best_v[2]) {
					best_v[2] = v; best[2] = dst;
				}
			}

			seq_printf(m, "    %4d %10ld  ", src, row_sum[src]);
			for (j = 0; j < 3; j++) {
				if (best[j] < 0)
					break;
				if (j > 0)
					seq_printf(m, ", ");
				seq_printf(m, "n%d:%ld", best[j], best_v[j]);
			}
			seq_printf(m, "\n");
		}
	} else {
		seq_printf(m, "    Migration Matrix (%ld pages)\n", matrix_total);
		seq_printf(m, "    %4s", "s\\d");
		for (i = 0; i < cols; i++)
			seq_printf(m, " %8d", online_nodes[i]);
		seq_printf(m, "\n");

		seq_printf(m, "    ----");
		for (i = 0; i < cols; i++)
			seq_printf(m, " --------");
		seq_printf(m, "\n");

		for (i = 0; i < nr_online; i++) {
			int src = online_nodes[i];
			seq_printf(m, "    %4d", src);
			for (j = 0; j < cols; j++)
				seq_printf(m, " %8ld", matrix[src][online_nodes[j]]);
			seq_printf(m, "\n");
		}
	}
	seq_printf(m, "\n");
}

static int hydra_status_show(struct seq_file *m, void *v)
{
	struct task_struct *task;
	int i, online_nodes[NUMA_NODE_COUNT], nr_online = 0;
	int total_hydra = 0;

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (node_online(i))
			online_nodes[nr_online++] = i;
	}

	hydra_status_print_header(m, nr_online, online_nodes);
	hydra_status_print_cache(m, nr_online, online_nodes);

	seq_printf(m,
		"------------------------------------------------------\n"
		"  PER-PROCESS DETAILS\n"
		"------------------------------------------------------\n\n");

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;

		mm = task->mm;
		if (!mm || !READ_ONCE(mm->lazy_repl_enabled))
			continue;

		total_hydra++;

		rcu_read_unlock();

		seq_printf(m, "  [%s]  pid %d\n", task->comm, task->pid);
		seq_printf(m,
			"  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");

		hydra_status_print_tlb_table(m, mm);
		hydra_status_print_repl_table(m, mm);
		hydra_status_print_vma_dist(m, mm, nr_online, online_nodes);
		hydra_status_print_migration(m, mm, nr_online, online_nodes);

		rcu_read_lock();
	}
	rcu_read_unlock();

	seq_printf(m,
		"======================================================\n"
		"  %d hydra-enabled process(es)\n"
		"======================================================\n",
		total_hydra);

	return 0;
}

static int hydra_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_status_show, NULL);
}

static ssize_t hydra_status_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char kbuf[16];
	long val;
	struct task_struct *task;
	int i, j;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtol(kbuf, 10, &val))
		return -EINVAL;

	if (val == -1) {
		rcu_read_lock();
		for_each_process(task) {
			struct mm_struct *mm = task->mm;
			if (!mm || !READ_ONCE(mm->lazy_repl_enabled))
				continue;
			atomic_long_set(&mm->hydra_tlb_shootdowns_total, 0);
			atomic_long_set(&mm->hydra_tlb_shootdowns_sent, 0);
			atomic_long_set(&mm->hydra_tlb_shootdowns_saved, 0);
			atomic_long_set(&mm->hydra_repl_pte_faults, 0);
			atomic_long_set(&mm->hydra_repl_ptes_copied, 0);
			atomic_long_set(&mm->hydra_repl_hugepmd_faults, 0);
			atomic_long_set(&mm->hydra_repl_hugepmd_copied, 0);
			for (i = 0; i < NUMA_NODE_COUNT; i++)
				for (j = 0; j < NUMA_NODE_COUNT; j++)
					atomic_long_set(&mm->hydra_migration_matrix[i][j], 0);
		}
		rcu_read_unlock();
		pr_info("[HYDRA]: All counters reset for all processes\n");
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct proc_ops hydra_status_ops = {
	.proc_open    = hydra_status_open,
	.proc_read    = seq_read,
	.proc_write   = hydra_status_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static void verify_replica_walk(struct seq_file *m, struct mm_struct *mm,
				int node, unsigned long *violations,
				unsigned long *checked)
{
	pgd_t *pgd_base = mm->repl_pgd[node];
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long addr, next_pgd, next_pud, next_pmd;
	int pg_node;

	if (!virt_addr_valid(pgd_base))
		return;

	pg_node = page_to_nid(virt_to_page(pgd_base));
	(*checked)++;
	if (pg_node != node) {
		seq_printf(m, "      PGD page on node %d (expected %d)\n",
			   pg_node, node);
		(*violations)++;
	}

	addr = 0;
	pgd = pgd_offset_pgd(pgd_base, addr);
	do {
		next_pgd = pgd_addr_end(addr, TASK_SIZE);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			goto next_pgd;

		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			goto next_pgd;

		pud = pud_offset(p4d, addr);
		if (!virt_addr_valid(pud))
			goto next_pgd;

		pg_node = page_to_nid(virt_to_page(pud));
		(*checked)++;
		if (pg_node != node) {
			seq_printf(m, "      PUD page at 0x%lx on node %d (expected %d)\n",
				   addr, pg_node, node);
			(*violations)++;
		}

		do {
			next_pud = pud_addr_end(addr, next_pgd);
			if (pud_none(*pud) || pud_bad(*pud) ||
			    pud_trans_huge(*pud) || pud_leaf(*pud))
				goto next_pud;

			pmd = pmd_offset(pud, addr);
			if (!virt_addr_valid(pmd))
				goto next_pud;

			pg_node = page_to_nid(virt_to_page(pmd));
			(*checked)++;
			if (pg_node != node) {
				seq_printf(m, "      PMD page at 0x%lx on node %d (expected %d)\n",
					   addr, pg_node, node);
				(*violations)++;
			}

			do {
				next_pmd = pmd_addr_end(addr, next_pud);
				if (pmd_none(*pmd) || pmd_trans_huge(*pmd) || pmd_bad(*pmd))
					goto next_pmd;

				pte = pte_offset_kernel(pmd, addr);
				if (!virt_addr_valid(pte))
					goto next_pmd;

				pg_node = page_to_nid(virt_to_page(pte));
				(*checked)++;
				if (pg_node != node) {
					seq_printf(m, "      PTE page at 0x%lx on node %d (expected %d)\n",
						   addr, pg_node, node);
					(*violations)++;
				}
next_pmd:
				addr = next_pmd;
			} while (pmd++, addr != next_pud);
next_pud:
			addr = next_pud;
		} while (pud++, addr != next_pgd);
next_pgd:
		addr = next_pgd;
	} while (pgd++, addr != TASK_SIZE);
}

static bool master_pte_present(struct mm_struct *mm, int master_node,
			       unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset_pgd(mm->repl_pgd[master_node], addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return false;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return false;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return false;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return false;
	if (pmd_trans_huge(*pmd))
		return pmd_present(*pmd);
	if (pmd_bad(*pmd))
		return false;
	pte = pte_offset_kernel(pmd, addr);
	return pte_present(*pte);
}

static bool master_huge_pmd_present(struct mm_struct *mm, int master_node,
				    unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset_pgd(mm->repl_pgd[master_node], addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return false;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return false;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return false;
	pmd = pmd_offset(pud, addr);
	return pmd_present(*pmd) && pmd_trans_huge(*pmd);
}

static void verify_master_has_mapping(struct seq_file *m,
				      struct mm_struct *mm,
				      struct vm_area_struct *vma,
				      int replica_node,
				      int master_node,
				      unsigned long *violations,
				      unsigned long *checked)
{
	pgd_t *r_pgd;
	p4d_t *r_p4d;
	pud_t *r_pud;
	pmd_t *r_pmd;
	pte_t *r_pte;
	unsigned long addr, end, next_pgd, next_p4d, next_pud, next_pmd;

	addr = vma->vm_start;
	end = vma->vm_end;

	r_pgd = pgd_offset_pgd(mm->repl_pgd[replica_node], addr);
	do {
		next_pgd = pgd_addr_end(addr, end);
		if (pgd_none(*r_pgd) || pgd_bad(*r_pgd))
			goto next_pgd;

		r_p4d = p4d_offset(r_pgd, addr);
		do {
			next_p4d = p4d_addr_end(addr, next_pgd);
			if (p4d_none(*r_p4d) || p4d_bad(*r_p4d))
				goto next_p4d;

			r_pud = pud_offset(r_p4d, addr);
			do {
				next_pud = pud_addr_end(addr, next_p4d);
				if (pud_none(*r_pud) || pud_bad(*r_pud))
					goto next_pud;

				r_pmd = pmd_offset(r_pud, addr);
				do {
					next_pmd = pmd_addr_end(addr, next_pud);

					if (pmd_none(*r_pmd))
						goto next_pmd;

					if (pmd_trans_huge(*r_pmd)) {
						if (pmd_present(*r_pmd)) {
							(*checked)++;
							if (!master_huge_pmd_present(mm, master_node, addr)) {
								(*violations)++;
								seq_printf(m, "      node %d has huge PMD at 0x%lx, master node %d does not\n",
									   replica_node, addr, master_node);
							}
						}
						goto next_pmd;
					}

					if (pmd_bad(*r_pmd))
						goto next_pmd;

					r_pte = pte_offset_kernel(r_pmd, addr);
					do {
						if (pte_present(*r_pte)) {
							(*checked)++;
							if (!master_pte_present(mm, master_node, addr)) {
								(*violations)++;
								seq_printf(m, "      node %d has PTE at 0x%lx, master node %d does not\n",
									   replica_node, addr, master_node);
							}
						}
					} while (r_pte++, addr += PAGE_SIZE, addr != next_pmd);
next_pmd:
					addr = next_pmd;
				} while (r_pmd++, addr != next_pud);
next_pud:
				addr = next_pud;
			} while (r_pud++, addr != next_p4d);
next_p4d:
			addr = next_p4d;
		} while (r_p4d++, addr != next_pgd);
next_pgd:
		addr = next_pgd;
	} while (r_pgd++, addr != end);
}

static int hydra_verify_show(struct seq_file *m, void *v)
{
	struct task_struct *task;
	int i, total_checked = 0, total_passed = 0;

	if (!sysctl_hydra_verify_enabled) {
		seq_printf(m, "Fault-time verification: disabled\n");
		seq_printf(m, "Write 1 to enable, 0 to disable.\n");
		return 0;
	}

	seq_printf(m, "Fault-time verification: enabled\n\n");

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;
		bool inv1_pass, inv2_pass, inv3_pass;
		int inv1_missing, online_nodes[NUMA_NODE_COUNT], nr_online;
		unsigned long inv2_total_checked, inv2_total_violations;
		unsigned long inv3_total_checked, inv3_total_violations;
		int inv2_nodes_checked, inv3_nodes_checked;
		unsigned long inv2_node_violations[NUMA_NODE_COUNT] = {};
		unsigned long inv2_node_checked[NUMA_NODE_COUNT] = {};
		unsigned long inv3_node_violations[NUMA_NODE_COUNT] = {};
		unsigned long inv3_node_checked[NUMA_NODE_COUNT] = {};

		mm = task->mm;
		if (!mm || !READ_ONCE(mm->lazy_repl_enabled))
			continue;

		total_checked++;
		nr_online = 0;
		for (i = 0; i < NUMA_NODE_COUNT; i++) {
			if (node_online(i))
				online_nodes[nr_online++] = i;
		}

		rcu_read_unlock();

		inv1_pass = true;
		inv1_missing = 0;
		for (i = 0; i < nr_online; i++) {
			if (!mm->repl_pgd[online_nodes[i]]) {
				inv1_pass = false;
				inv1_missing++;
			}
		}

		inv2_pass = true;
		inv3_pass = true;
		inv2_total_checked = 0;
		inv2_total_violations = 0;
		inv2_nodes_checked = 0;
		inv3_total_checked = 0;
		inv3_total_violations = 0;
		inv3_nodes_checked = 0;

		if (mmap_read_trylock(mm)) {
			for (i = 0; i < nr_online; i++) {
				int n = online_nodes[i];

				if (!mm->repl_pgd[n])
					continue;

				verify_replica_walk(m, mm, n,
						    &inv2_node_violations[n],
						    &inv2_node_checked[n]);
				inv2_total_checked += inv2_node_checked[n];
				inv2_total_violations += inv2_node_violations[n];
				inv2_nodes_checked++;
				if (inv2_node_violations[n])
					inv2_pass = false;
			}

			for (i = 0; i < nr_online; i++) {
				int n = online_nodes[i];
				struct vm_area_struct *vma;
				VMA_ITERATOR(vmi, mm, 0);

				if (!mm->repl_pgd[n] || mm->repl_pgd[n] == mm->pgd)
					continue;

				for_each_vma(vmi, vma) {
					if (n == vma->master_pgd_node)
						continue;
					verify_master_has_mapping(m, mm, vma, n,
								  vma->master_pgd_node,
								  &inv3_node_violations[n],
								  &inv3_node_checked[n]);
				}

				inv3_total_checked += inv3_node_checked[n];
				inv3_total_violations += inv3_node_violations[n];
				inv3_nodes_checked++;
				if (inv3_node_violations[n])
					inv3_pass = false;
			}

			mmap_read_unlock(mm);

			seq_printf(m, "%-20s  PID %-6d  ", task->comm, task->pid);

			if (inv1_pass && inv2_pass && inv3_pass) {
				seq_printf(m, "ALL PASS");
				seq_printf(m, "  [locality: %lu/%d nodes]",
					   inv2_total_checked, inv2_nodes_checked);
				seq_printf(m, "  [master: %lu/%d replicas]\n",
					   inv3_total_checked, inv3_nodes_checked);
				total_passed++;
			} else {
				seq_printf(m, "FAIL\n");

				if (!inv1_pass)
					seq_printf(m, "    INV1 FAIL: %d nodes missing PGD\n",
						   inv1_missing);

				if (!inv2_pass) {
					seq_printf(m, "    INV2 FAIL: %lu locality violations in %lu checks\n",
						   inv2_total_violations, inv2_total_checked);
					for (i = 0; i < nr_online; i++) {
						int n = online_nodes[i];
						if (inv2_node_violations[n])
							seq_printf(m, "      node %d: %lu violations / %lu checked\n",
								   n, inv2_node_violations[n],
								   inv2_node_checked[n]);
					}
				}

				if (!inv3_pass) {
					seq_printf(m, "    INV3 FAIL: %lu master-missing violations in %lu checks\n",
						   inv3_total_violations, inv3_total_checked);
					for (i = 0; i < nr_online; i++) {
						int n = online_nodes[i];
						if (inv3_node_violations[n])
							seq_printf(m, "      node %d: %lu violations / %lu checked\n",
								   n, inv3_node_violations[n],
								   inv3_node_checked[n]);
					}
				}
			}
		} else {
			seq_printf(m, "%-20s  PID %-6d  SKIP (lock busy)\n",
				   task->comm, task->pid);
			total_passed++;
		}

		rcu_read_lock();
	}
	rcu_read_unlock();

	seq_printf(m, "\n%d processes checked, %d passed, %d failed\n",
		   total_checked, total_passed, total_checked - total_passed);

	return 0;
}

static ssize_t hydra_verify_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char kbuf[16];
	long val;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtol(kbuf, 10, &val))
		return -EINVAL;

	if (val != 0 && val != 1)
		return -EINVAL;

	sysctl_hydra_verify_enabled = val;
	pr_info("[HYDRA]: Fault-time verification %s\n", val ? "enabled" : "disabled");
	return count;
}

static int hydra_verify_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_verify_show, NULL);
}

static const struct proc_ops hydra_verify_ops = {
	.proc_open    = hydra_verify_open,
	.proc_read    = seq_read,
	.proc_write   = hydra_verify_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static int __init hydra_check_numa_node_count(void)
{
	int online = num_online_nodes();

	if (online != NUMA_NODE_COUNT) {
		pr_emerg("HYDRA: CONFIG_HYDRA_NUMA_NODE_COUNT=%d but system has %d NUMA nodes.\n",
			 NUMA_NODE_COUNT, online);
		pr_emerg("HYDRA: Reconfigure kernel with CONFIG_HYDRA_NUMA_NODE_COUNT=%d\n",
			 online);
		pr_emerg("HYDRA: Check node count with: numactl --hardware\n");
		BUG();
	}

	pr_info("HYDRA: NUMA node count matches: %d nodes\n", online);
	return 0;
}
early_initcall(hydra_check_numa_node_count);

static int hydra_history_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	struct hydra_history_entry *e;
	int i, j, nr_online = 0;
	int online_nodes[NUMA_NODE_COUNT];
	int entry_num = 0;

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (node_online(i))
			online_nodes[nr_online++] = i;
	}

	spin_lock_irqsave(&hydra_history_lock, flags);

	if (hydra_history_count == 0) {
		spin_unlock_irqrestore(&hydra_history_lock, flags);
		seq_printf(m,
			"======================================================\n"
			"                   HYDRA EXIT HISTORY                  \n"
			"======================================================\n\n"
			"  No history entries.\n\n"
			"======================================================\n");
		return 0;
	}

	seq_printf(m,
		"======================================================\n"
		"                   HYDRA EXIT HISTORY                  \n"
		"======================================================\n\n");

	list_for_each_entry(e, &hydra_history_list, list) {
		u64 sec = e->exit_time_ns;
		u32 nsec = do_div(sec, 1000000000ULL);
		long tlb_pct;
		long avg_pte, avg_hpmd;
		long matrix_total = 0;

		entry_num++;

		tlb_pct = (e->tlb_total > 0)
			? (e->tlb_saved * 100) / e->tlb_total : 0;
		avg_pte = (e->pte_faults > 0)
			? e->ptes_copied / e->pte_faults : 0;
		avg_hpmd = (e->hugepmd_faults > 0)
			? e->hugepmd_copied / e->hugepmd_faults : 0;

		seq_printf(m, "  [%s]  pid %d  exited %llu.%09u\n",
			   e->comm, e->pid, sec, nsec);
		seq_printf(m,
			"  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");

		seq_printf(m, "    TLB Shootdowns\n");
		seq_printf(m, "    %10s %10s %10s %7s\n",
			   "Total", "Sent", "Saved", "Saved%");
		seq_printf(m, "    ---------- ---------- ---------- -------\n");
		seq_printf(m, "    %10ld %10ld %10ld %5ld%%\n\n",
			   e->tlb_total, e->tlb_sent, e->tlb_saved, tlb_pct);

		seq_printf(m, "    Replication Faults\n");
		seq_printf(m, "    %-8s %10s %10s %10s\n",
			   "Type", "Faults", "Copied", "Avg/fault");
		seq_printf(m, "    -------- ---------- ---------- ----------\n");
		seq_printf(m, "    %-8s %10ld %10ld %10ld\n",
			   "PTE", e->pte_faults, e->ptes_copied, avg_pte);
		seq_printf(m, "    %-8s %10ld %10ld %10ld\n\n",
			   "HugePMD", e->hugepmd_faults, e->hugepmd_copied,
			   avg_hpmd);

		for (i = 0; i < nr_online; i++)
			for (j = 0; j < nr_online; j++)
				matrix_total += e->migration_matrix[online_nodes[i]][online_nodes[j]];

		if (matrix_total > 0) {
			int cols = nr_online;

			if (cols > 4) {
				long row_sum[NUMA_NODE_COUNT] = {};

				for (i = 0; i < nr_online; i++)
					for (j = 0; j < nr_online; j++)
						row_sum[online_nodes[i]] +=
							e->migration_matrix[online_nodes[i]][online_nodes[j]];

				seq_printf(m, "    Migration (%ld pages migrated)\n",
					   matrix_total);
				seq_printf(m, "    %4s %10s  Top destinations\n",
					   "From", "Total");
				seq_printf(m, "    ---- ----------  "
					   "-----------------------------\n");

				for (i = 0; i < nr_online; i++) {
					int src = online_nodes[i];
					int best[3] = {-1, -1, -1};
					long best_v[3] = {0, 0, 0};

					if (row_sum[src] == 0)
						continue;

					for (j = 0; j < nr_online; j++) {
						int dst = online_nodes[j];
						long val = e->migration_matrix[src][dst];
						if (val <= 0)
							continue;
						if (val > best_v[0]) {
							best_v[2] = best_v[1]; best[2] = best[1];
							best_v[1] = best_v[0]; best[1] = best[0];
							best_v[0] = val; best[0] = dst;
						} else if (val > best_v[1]) {
							best_v[2] = best_v[1]; best[2] = best[1];
							best_v[1] = val; best[1] = dst;
						} else if (val > best_v[2]) {
							best_v[2] = val; best[2] = dst;
						}
					}

					seq_printf(m, "    %4d %10ld  ", src, row_sum[src]);
					for (j = 0; j < 3; j++) {
						if (best[j] < 0)
							break;
						if (j > 0)
							seq_printf(m, ", ");
						seq_printf(m, "n%d:%ld", best[j], best_v[j]);
					}
					seq_printf(m, "\n");
				}
			} else {
				seq_printf(m, "    Migration Matrix (%ld pages)\n",
					   matrix_total);
				seq_printf(m, "    %4s", "s\\d");
				for (i = 0; i < cols; i++)
					seq_printf(m, " %8d", online_nodes[i]);
				seq_printf(m, "\n");

				seq_printf(m, "    ----");
				for (i = 0; i < cols; i++)
					seq_printf(m, " --------");
				seq_printf(m, "\n");

				for (i = 0; i < nr_online; i++) {
					int src = online_nodes[i];
					seq_printf(m, "    %4d", src);
					for (j = 0; j < cols; j++)
						seq_printf(m, " %8ld",
							   e->migration_matrix[src][online_nodes[j]]);
					seq_printf(m, "\n");
				}
			}
			seq_printf(m, "\n");
		}
	}

	seq_printf(m,
		"======================================================\n"
		"  %d entr%s\n"
		"======================================================\n",
		hydra_history_count,
		hydra_history_count == 1 ? "y" : "ies");

	spin_unlock_irqrestore(&hydra_history_lock, flags);

	return 0;
}

static ssize_t hydra_history_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	char kbuf[16];
	long val;
	unsigned long flags;
	struct hydra_history_entry *e, *tmp;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtol(kbuf, 10, &val))
		return -EINVAL;

	if (val == -1) {
		spin_lock_irqsave(&hydra_history_lock, flags);
		list_for_each_entry_safe(e, tmp, &hydra_history_list, list) {
			list_del(&e->list);
			kfree(e);
		}
		hydra_history_count = 0;
		spin_unlock_irqrestore(&hydra_history_lock, flags);
		pr_info("[HYDRA]: History cleared\n");
	} else {
		return -EINVAL;
	}

	return count;
}

static int hydra_history_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_history_show, NULL);
}

static const struct proc_ops hydra_history_ops = {
	.proc_open    = hydra_history_open,
	.proc_read    = seq_read,
	.proc_write   = hydra_history_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static pid_t walk_pid;
static unsigned long walk_addr;

static void walk_show_pte_flags(struct seq_file *m, pte_t pte)
{
	seq_printf(m, " [%s%s%s%s%s%s pfn=0x%lx]",
		   pte_present(pte) ? "P" : "-",
		   pte_write(pte) ? "W" : "-",
		   pte_dirty(pte) ? "D" : "-",
		   pte_young(pte) ? "A" : "-",
		   pte_protnone(pte) ? "N" : "-",
		   pte_special(pte) ? "S" : "-",
		   pte_pfn(pte));
}

static void walk_show_pmd_flags(struct seq_file *m, pmd_t pmd)
{
	seq_printf(m, " [%s%s%s%s%s%s pfn=0x%lx]",
		   pmd_present(pmd) ? "P" : "-",
		   pmd_write(pmd) ? "W" : "-",
		   pmd_dirty(pmd) ? "D" : "-",
		   pmd_young(pmd) ? "A" : "-",
		   pmd_protnone(pmd) ? "N" : "-",
		   pmd_trans_huge(pmd) ? "H" : "-",
		   pmd_pfn(pmd));
}

static void walk_one_node(struct seq_file *m, struct mm_struct *mm,
			  unsigned long addr, int node)
{
	pgd_t *pgd, pgdent;
	p4d_t *p4d, p4dent;
	pud_t *pud, pudent;
	pmd_t *pmd, pmdent;
	pte_t *pte, ptent;
	int pg_node;

	if (!mm->repl_pgd[node]) {
		seq_printf(m, "  node %d: no PGD\n", node);
		return;
	}

	pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
	pgdent = *pgd;
	pg_node = page_to_nid(virt_to_page(mm->repl_pgd[node]));
	seq_printf(m, "  node %d:\n", node);
	seq_printf(m, "    PGD: va=%px pgd_val=0x%lx page_node=%d",
		   pgd, pgd_val(pgdent), pg_node);

	if (pgd_none(pgdent)) {
		seq_printf(m, " NONE\n");
		return;
	}
	if (pgd_bad(pgdent)) {
		seq_printf(m, " BAD\n");
		return;
	}
	seq_printf(m, "\n");

	p4d = p4d_offset(pgd, addr);
	p4dent = *p4d;
	seq_printf(m, "    P4D: va=%px p4d_val=0x%lx",
		   p4d, p4d_val(p4dent));

	if (p4d_none(p4dent)) {
		seq_printf(m, " NONE\n");
		return;
	}
	if (p4d_bad(p4dent)) {
		seq_printf(m, " BAD\n");
		return;
	}
	seq_printf(m, "\n");

	pud = pud_offset(p4d, addr);
	pudent = *pud;
	if (virt_addr_valid(pud))
		pg_node = page_to_nid(virt_to_page(pud));
	else
		pg_node = -1;
	seq_printf(m, "    PUD: va=%px pud_val=0x%lx page_node=%d",
		   pud, pud_val(pudent), pg_node);

	if (pud_none(pudent)) {
		seq_printf(m, " NONE\n");
		return;
	}
	if (pud_trans_huge(pudent)) {
		seq_printf(m, " HUGE_PUD\n");
		return;
	}
	if (pud_bad(pudent)) {
		seq_printf(m, " BAD\n");
		return;
	}
	seq_printf(m, "\n");

	pmd = pmd_offset(pud, addr);
	pmdent = *pmd;
	if (virt_addr_valid(pmd))
		pg_node = page_to_nid(virt_to_page(pmd));
	else
		pg_node = -1;

	seq_printf(m, "    PMD: va=%px pmd_val=0x%lx page_node=%d",
		   pmd, pmd_val(pmdent), pg_node);

	if (pmd_none(pmdent)) {
		seq_printf(m, " NONE\n");
		return;
	}

	if (pmd_trans_huge(pmdent)) {
		seq_printf(m, " THP");
		walk_show_pmd_flags(m, pmdent);

		if (virt_addr_valid(pmd) && READ_ONCE(virt_to_page(pmd)->next_replica)) {
			struct page *pmd_page = virt_to_page(pmd);
			struct page *cur;
			unsigned long offset = ((unsigned long)pmd) & ~PAGE_MASK;
			int rcount = 0;

			seq_printf(m, " replicas={");
			for_each_replica(pmd_page, cur) {
				pmd_t *rpmd = (pmd_t *)(page_address(cur) + offset);
				pmd_t rv = *rpmd;
				int rn = page_to_nid(cur);

				if (rcount > 0)
					seq_printf(m, ", ");
				seq_printf(m, "n%d:", rn);
				if (pmd_none(rv))
					seq_printf(m, "NONE");
				else if (pmd_trans_huge(rv))
					seq_printf(m, "THP pfn=0x%lx %s%s%s",
						   pmd_pfn(rv),
						   pmd_present(rv) ? "P" : "-",
						   pmd_young(rv) ? "A" : "-",
						   pmd_dirty(rv) ? "D" : "-");
				else
					seq_printf(m, "NORMAL");
				rcount++;
			}
			seq_printf(m, "}");
		}

		seq_printf(m, "\n");
		return;
	}

	if (pmd_bad(pmdent)) {
		seq_printf(m, " BAD\n");
		return;
	}

	walk_show_pmd_flags(m, pmdent);
	seq_printf(m, "\n");

	pte = pte_offset_kernel(pmd, addr);
	ptent = *pte;
	if (virt_addr_valid(pte))
		pg_node = page_to_nid(virt_to_page(pte));
	else
		pg_node = -1;

	seq_printf(m, "    PTE: va=%px pte_val=0x%lx page_node=%d",
		   pte, pte_val(ptent), pg_node);

	if (pte_none(ptent)) {
		seq_printf(m, " NONE\n");
		return;
	}

	if (!pte_present(ptent)) {
		seq_printf(m, " NOT_PRESENT (swap/migration)\n");
		return;
	}

	walk_show_pte_flags(m, ptent);

	if (virt_addr_valid(pte) && READ_ONCE(virt_to_page(pte)->next_replica)) {
		struct page *pte_page = virt_to_page(pte);
		struct page *cur;
		long offset = (long)pte - (long)page_to_virt(pte_page);
		int rcount = 0;

		seq_printf(m, " replicas={");
		for_each_replica(pte_page, cur) {
			pte_t *rpte = (pte_t *)((long)page_to_virt(cur) + offset);
			pte_t rv = *rpte;
			int rn = page_to_nid(cur);

			if (rcount > 0)
				seq_printf(m, ", ");
			seq_printf(m, "n%d:", rn);
			if (pte_none(rv))
				seq_printf(m, "NONE");
			else if (!pte_present(rv))
				seq_printf(m, "SWAP");
			else
				seq_printf(m, "pfn=0x%lx %s%s%s",
					   pte_pfn(rv),
					   pte_present(rv) ? "P" : "-",
					   pte_young(rv) ? "A" : "-",
					   pte_dirty(rv) ? "D" : "-");
			rcount++;
		}
		seq_printf(m, "}");
	}

	seq_printf(m, "\n");
}

static int hydra_walk_show(struct seq_file *m, void *v)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int i, nr_online = 0;
	int online_nodes[NUMA_NODE_COUNT];

	if (!walk_pid) {
		seq_printf(m, "Usage: echo '<pid> <hex_addr>' > /proc/hydra/walk\n");
		seq_printf(m, "Then read this file to see page table state.\n");
		return 0;
	}

	rcu_read_lock();
	task = find_task_by_vpid(walk_pid);
	if (!task) {
		rcu_read_unlock();
		seq_printf(m, "PID %d not found\n", walk_pid);
		return 0;
	}
	mm = get_task_mm(task);
	rcu_read_unlock();

	if (!mm) {
		seq_printf(m, "PID %d has no mm\n", walk_pid);
		return 0;
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (node_online(i))
			online_nodes[nr_online++] = i;
	}

	seq_printf(m, "PID: %d  Address: 0x%lx  Replication: %s\n",
		   walk_pid, walk_addr,
		   mm->lazy_repl_enabled ? "enabled" : "disabled");

	mmap_read_lock(mm);

	vma = find_vma(mm, walk_addr);
	if (vma && vma->vm_start <= walk_addr) {
		seq_printf(m, "VMA: 0x%lx-0x%lx flags=0x%lx master_node=%lu",
			   vma->vm_start, vma->vm_end,
			   vma->vm_flags, vma->master_pgd_node);
		if (vma->vm_file)
			seq_printf(m, " file-backed");
		else
			seq_printf(m, " anon");
		seq_printf(m, "\n");
	} else {
		seq_printf(m, "VMA: not found for address\n");
	}

	seq_printf(m, "\n");

	if (!mm->lazy_repl_enabled) {
		seq_printf(m, "Single page table (no replication):\n");
		walk_one_node(m, mm, walk_addr, 0);
	} else {
		for (i = 0; i < nr_online; i++) {
			int n = online_nodes[i];
			if (mm->repl_pgd[n] == mm->pgd)
				seq_printf(m, "--- Node %d (PRIMARY) ---\n", n);
			else
				seq_printf(m, "--- Node %d (REPLICA) ---\n", n);
			walk_one_node(m, mm, walk_addr, n);
		}
	}

	mmap_read_unlock(mm);
	mmput(mm);
	return 0;
}

static ssize_t hydra_walk_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	char kbuf[64];
	int pid;
	unsigned long addr;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (sscanf(kbuf, "%d %lx", &pid, &addr) != 2)
		return -EINVAL;

	walk_pid = pid;
	walk_addr = addr;
	return count;
}

static int hydra_walk_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_walk_show, NULL);
}

static const struct proc_ops hydra_walk_ops = {
	.proc_open = hydra_walk_open,
	.proc_read = seq_read,
	.proc_write = hydra_walk_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static pid_t thp_check_pid;

struct thp_check_stats {
	unsigned long pmds_checked;
	unsigned long thp_master_count;
	unsigned long thp_replica_count;
	unsigned long thp_match;
	unsigned long thp_not_replicated;
	unsigned long err_stale_thp;
	unsigned long err_orphan_thp;
	unsigned long err_pfn_mismatch;
	unsigned long err_excess_write;
	unsigned long err_type_mismatch;
};

static void thp_check_one_pmd(struct seq_file *m,
			      struct mm_struct *mm,
			      unsigned long addr,
			      int master_node,
			      int replica_node,
			      struct thp_check_stats *stats)
{
	pgd_t *m_pgd, *r_pgd;
	p4d_t *m_p4d, *r_p4d;
	pud_t *m_pud, *r_pud;
	pmd_t *m_pmd, *r_pmd;
	pmd_t m_val, r_val;
	bool m_present, m_thp, m_normal;
	bool r_present, r_thp, r_normal;

	stats->pmds_checked++;

	m_pgd = pgd_offset_pgd(mm->repl_pgd[master_node], addr);
	if (pgd_none(*m_pgd) || pgd_bad(*m_pgd))
		return;
	m_p4d = p4d_offset(m_pgd, addr);
	if (p4d_none(*m_p4d) || p4d_bad(*m_p4d))
		return;
	m_pud = pud_offset(m_p4d, addr);
	if (pud_none(*m_pud) || pud_bad(*m_pud))
		return;
	m_pmd = pmd_offset(m_pud, addr);
	m_val = *m_pmd;

	m_present = pmd_present(m_val) && !pmd_none(m_val);
	m_thp = m_present && pmd_trans_huge(m_val);
	m_normal = m_present && !pmd_trans_huge(m_val) && !pmd_bad(m_val);

	if (m_thp)
		stats->thp_master_count++;

	r_pgd = pgd_offset_pgd(mm->repl_pgd[replica_node], addr);
	if (pgd_none(*r_pgd) || pgd_bad(*r_pgd)) {
		if (m_thp)
			stats->thp_not_replicated++;
		return;
	}
	r_p4d = p4d_offset(r_pgd, addr);
	if (p4d_none(*r_p4d) || p4d_bad(*r_p4d)) {
		if (m_thp)
			stats->thp_not_replicated++;
		return;
	}
	r_pud = pud_offset(r_p4d, addr);
	if (pud_none(*r_pud) || pud_bad(*r_pud)) {
		if (m_thp)
			stats->thp_not_replicated++;
		return;
	}
	r_pmd = pmd_offset(r_pud, addr);
	r_val = *r_pmd;

	r_present = pmd_present(r_val) && !pmd_none(r_val);
	r_thp = r_present && pmd_trans_huge(r_val);
	r_normal = r_present && !pmd_trans_huge(r_val) && !pmd_bad(r_val);

	if (r_thp)
		stats->thp_replica_count++;

	if (m_thp && r_thp) {
		stats->thp_match++;

		if (pmd_pfn(m_val) != pmd_pfn(r_val)) {
			stats->err_pfn_mismatch++;
			seq_printf(m, "  ERR PFN_MISMATCH at 0x%lx: master_node=%d pfn=0x%lx "
				   "replica_node=%d pfn=0x%lx\n",
				   addr, master_node, pmd_pfn(m_val),
				   replica_node, pmd_pfn(r_val));
		}

		if (!pmd_write(m_val) && pmd_write(r_val)) {
			stats->err_excess_write++;
			seq_printf(m, "  ERR EXCESS_WRITE at 0x%lx: master_node=%d RO "
				   "replica_node=%d RW\n",
				   addr, master_node, replica_node);
		}
		return;
	}

	if (m_thp && pmd_none(r_val)) {
		stats->thp_not_replicated++;
		return;
	}

	if (m_thp && r_normal) {
		stats->err_type_mismatch++;
		seq_printf(m, "  ERR TYPE_MISMATCH at 0x%lx: master_node=%d has THP "
			   "but replica_node=%d has normal PMD (PTE-backed)\n",
			   addr, master_node, replica_node);
		return;
	}

	if (m_normal && r_thp) {
		stats->err_stale_thp++;
		seq_printf(m, "  ERR STALE_THP at 0x%lx: master_node=%d has normal PMD "
			   "(split done) but replica_node=%d still has THP pfn=0x%lx\n",
			   addr, master_node, replica_node, pmd_pfn(r_val));
		return;
	}

	if (!m_present && r_thp) {
		stats->err_orphan_thp++;
		seq_printf(m, "  ERR ORPHAN_THP at 0x%lx: master_node=%d has no PMD "
			   "but replica_node=%d has THP pfn=0x%lx\n",
			   addr, master_node, replica_node, pmd_pfn(r_val));
		return;
	}

	if (pmd_none(m_val) && r_normal) {
		seq_printf(m, "  WARN ORPHAN_NORMAL at 0x%lx: master_node=%d has no PMD "
			   "but replica_node=%d has normal PMD\n",
			   addr, master_node, replica_node);
		return;
	}
}

static void thp_check_pmd_chain(struct seq_file *m,
				struct mm_struct *mm,
				unsigned long addr,
				int master_node)
{
	pgd_t *m_pgd;
	p4d_t *m_p4d;
	pud_t *m_pud;
	pmd_t *m_pmd;
	struct page *m_pmd_page;
	struct page *cur;
	unsigned long offset;
	pmd_t m_val;

	m_pgd = pgd_offset_pgd(mm->repl_pgd[master_node], addr);
	if (pgd_none(*m_pgd) || pgd_bad(*m_pgd))
		return;
	m_p4d = p4d_offset(m_pgd, addr);
	if (p4d_none(*m_p4d) || p4d_bad(*m_p4d))
		return;
	m_pud = pud_offset(m_p4d, addr);
	if (pud_none(*m_pud) || pud_bad(*m_pud))
		return;
	m_pmd = pmd_offset(m_pud, addr);
	m_val = *m_pmd;

	if (!pmd_present(m_val) || !pmd_trans_huge(m_val))
		return;

	if (!virt_addr_valid(m_pmd))
		return;

	m_pmd_page = virt_to_page(m_pmd);
	if (!READ_ONCE(m_pmd_page->next_replica))
		return;

	offset = ((unsigned long)m_pmd) & ~PAGE_MASK;

	seq_printf(m, "  CHAIN at 0x%lx master pfn=0x%lx: ", addr, pmd_pfn(m_val));

	for_each_replica(m_pmd_page, cur) {
		pmd_t *rpmd = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = *rpmd;
		int rn = page_to_nid(cur);

		if (pmd_none(rv))
			seq_printf(m, "n%d:NONE ", rn);
		else if (pmd_trans_huge(rv))
			seq_printf(m, "n%d:THP(pfn=0x%lx,%s%s%s) ",
				   rn, pmd_pfn(rv),
				   pmd_present(rv) ? "P" : "-",
				   pmd_young(rv) ? "A" : "-",
				   pmd_write(rv) ? "W" : "-");
		else
			seq_printf(m, "n%d:NORMAL ", rn);
	}
	seq_printf(m, "\n");
}

static int hydra_thp_check_show(struct seq_file *m, void *v)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int i, nr_online = 0;
	int online_nodes[NUMA_NODE_COUNT];
	struct thp_check_stats total;
	int master_node;
	unsigned long addr;

	if (!thp_check_pid) {
		seq_printf(m, "Usage: echo '<pid>' > /proc/hydra/thp_check\n");
		seq_printf(m, "Then read this file to see THP consistency report.\n");
		return 0;
	}

	rcu_read_lock();
	task = find_task_by_vpid(thp_check_pid);
	if (!task) {
		rcu_read_unlock();
		seq_printf(m, "PID %d not found\n", thp_check_pid);
		return 0;
	}
	mm = get_task_mm(task);
	rcu_read_unlock();

	if (!mm) {
		seq_printf(m, "PID %d has no mm\n", thp_check_pid);
		return 0;
	}

	if (!mm->lazy_repl_enabled) {
		seq_printf(m, "PID %d does not have replication enabled\n", thp_check_pid);
		mmput(mm);
		return 0;
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (node_online(i))
			online_nodes[nr_online++] = i;
	}

	memset(&total, 0, sizeof(total));

	seq_printf(m, "THP Consistency Check for PID %d (%s)\n", thp_check_pid, task->comm);
	seq_printf(m, "Online nodes: %d\n\n", nr_online);

	mmap_read_lock(mm);

	{
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			int vma_errors = 0;
			struct thp_check_stats vma_stats;
			unsigned long vma_start_aligned;
			unsigned long vma_end_aligned;

			master_node = vma->master_pgd_node;

			vma_start_aligned = vma->vm_start & HPAGE_PMD_MASK;
			if (vma_start_aligned < vma->vm_start)
				vma_start_aligned += HPAGE_PMD_SIZE;
			vma_end_aligned = vma->vm_end & HPAGE_PMD_MASK;

			if (vma_start_aligned >= vma_end_aligned)
				continue;

			memset(&vma_stats, 0, sizeof(vma_stats));

			for (addr = vma_start_aligned; addr < vma_end_aligned;
			     addr += HPAGE_PMD_SIZE) {
				for (i = 0; i < nr_online; i++) {
					int n = online_nodes[i];
					if (n == master_node)
						continue;

					thp_check_one_pmd(m, mm, addr,
							  master_node, n, &vma_stats);
				}

				thp_check_pmd_chain(m, mm, addr, master_node);
			}

			vma_errors = vma_stats.err_stale_thp +
				     vma_stats.err_orphan_thp +
				     vma_stats.err_pfn_mismatch +
				     vma_stats.err_excess_write +
				     vma_stats.err_type_mismatch;

			if (vma_stats.thp_master_count > 0 || vma_errors > 0) {
				seq_printf(m, "VMA 0x%lx-0x%lx master_node=%d",
					   vma->vm_start, vma->vm_end, master_node);
				if (vma->vm_file)
					seq_printf(m, " file-backed");
				else
					seq_printf(m, " anon");
				seq_printf(m, ": checked=%lu master_thp=%lu replica_thp=%lu "
					   "match=%lu not_repl=%lu errors=%d\n",
					   vma_stats.pmds_checked,
					   vma_stats.thp_master_count,
					   vma_stats.thp_replica_count,
					   vma_stats.thp_match,
					   vma_stats.thp_not_replicated,
					   vma_errors);
			}

			total.pmds_checked += vma_stats.pmds_checked;
			total.thp_master_count += vma_stats.thp_master_count;
			total.thp_replica_count += vma_stats.thp_replica_count;
			total.thp_match += vma_stats.thp_match;
			total.thp_not_replicated += vma_stats.thp_not_replicated;
			total.err_stale_thp += vma_stats.err_stale_thp;
			total.err_orphan_thp += vma_stats.err_orphan_thp;
			total.err_pfn_mismatch += vma_stats.err_pfn_mismatch;
			total.err_excess_write += vma_stats.err_excess_write;
			total.err_type_mismatch += vma_stats.err_type_mismatch;
		}
	}

	mmap_read_unlock(mm);

	seq_printf(m, "\n--- Summary ---\n");
	seq_printf(m, "PMDs checked:        %lu\n", total.pmds_checked);
	seq_printf(m, "Master THP count:    %lu\n", total.thp_master_count);
	seq_printf(m, "Replica THP count:   %lu\n", total.thp_replica_count);
	seq_printf(m, "THP matched:         %lu\n", total.thp_match);
	seq_printf(m, "THP not replicated:  %lu\n", total.thp_not_replicated);
	seq_printf(m, "\n");
	seq_printf(m, "ERR stale_thp:       %lu  (master split, replica still THP)\n",
		   total.err_stale_thp);
	seq_printf(m, "ERR orphan_thp:      %lu  (no master, replica has THP)\n",
		   total.err_orphan_thp);
	seq_printf(m, "ERR pfn_mismatch:    %lu  (both THP but different PFN)\n",
		   total.err_pfn_mismatch);
	seq_printf(m, "ERR excess_write:    %lu  (replica writable, master not)\n",
		   total.err_excess_write);
	seq_printf(m, "ERR type_mismatch:   %lu  (master THP vs replica normal)\n",
		   total.err_type_mismatch);

	if (total.err_stale_thp + total.err_orphan_thp + total.err_pfn_mismatch +
	    total.err_excess_write + total.err_type_mismatch == 0)
		seq_printf(m, "\nRESULT: PASS\n");
	else
		seq_printf(m, "\nRESULT: FAIL\n");

	mmput(mm);
	return 0;
}

static ssize_t hydra_thp_check_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char kbuf[32];
	int pid;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtoint(kbuf, 10, &pid))
		return -EINVAL;

	if (pid <= 0)
		return -EINVAL;

	thp_check_pid = pid;
	return count;
}

static int hydra_thp_check_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_thp_check_show, NULL);
}

static const struct proc_ops hydra_thp_check_ops = {
	.proc_open = hydra_thp_check_open,
	.proc_read = seq_read,
	.proc_write = hydra_thp_check_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static pid_t numa_check_pid;

struct numa_check_stats {
	unsigned long ptes_checked;
	unsigned long pmds_checked;
	unsigned long pte_protnone_master;
	unsigned long pte_protnone_replica;
	unsigned long pmd_protnone_master;
	unsigned long pmd_protnone_replica;
	unsigned long err_protnone_stale;
	unsigned long err_protnone_missing;
	unsigned long err_pfn_diverged;
	unsigned long err_accessed_lost;
	unsigned long err_dirty_lost;
	unsigned long err_write_excess;
	unsigned long err_replica_protnone_loop;
};

static void numa_check_pte_range(struct seq_file *m,
				 struct mm_struct *mm,
				 struct vm_area_struct *vma,
				 pmd_t *master_pmd,
				 pmd_t *replica_pmd,
				 unsigned long addr,
				 unsigned long end,
				 int master_node,
				 int replica_node,
				 struct numa_check_stats *stats)
{
	pte_t *m_pte, *r_pte;
	pte_t m_val, r_val;
	unsigned long a;

	if (pmd_none(*master_pmd) || pmd_trans_huge(*master_pmd) || pmd_bad(*master_pmd))
		return;
	if (pmd_none(*replica_pmd) || pmd_trans_huge(*replica_pmd) || pmd_bad(*replica_pmd))
		return;

	for (a = addr; a < end; a += PAGE_SIZE) {
		m_pte = pte_offset_kernel(master_pmd, a);
		r_pte = pte_offset_kernel(replica_pmd, a);
		m_val = *m_pte;
		r_val = *r_pte;

		if (pte_none(m_val) && pte_none(r_val))
			continue;

		if (!pte_present(m_val) && !pte_present(r_val))
			continue;

		stats->ptes_checked++;

		if (pte_present(m_val) && pte_protnone(m_val))
			stats->pte_protnone_master++;

		if (pte_present(r_val) && pte_protnone(r_val))
			stats->pte_protnone_replica++;

		if (pte_present(m_val) && pte_protnone(m_val) &&
		    pte_present(r_val) && pte_protnone(r_val)) {
			if (pte_pfn(m_val) != pte_pfn(r_val)) {
				stats->err_pfn_diverged++;
				seq_printf(m, "  ERR PTE_PFN_DIVERGED at 0x%lx: "
					   "master(n%d) pfn=0x%lx protnone "
					   "replica(n%d) pfn=0x%lx protnone\n",
					   a, master_node, pte_pfn(m_val),
					   replica_node, pte_pfn(r_val));
			}
			continue;
		}

		if (!pte_present(m_val) && pte_present(r_val) && pte_protnone(r_val)) {
			stats->err_protnone_stale++;
			seq_printf(m, "  ERR PTE_PROTNONE_STALE at 0x%lx: "
				   "master(n%d) not present "
				   "replica(n%d) protnone pfn=0x%lx\n",
				   a, master_node, replica_node, pte_pfn(r_val));
			continue;
		}

		if (pte_present(m_val) && !pte_protnone(m_val) &&
		    pte_present(r_val) && pte_protnone(r_val)) {
			stats->err_replica_protnone_loop++;
			seq_printf(m, "  ERR PTE_PROTNONE_LOOP at 0x%lx: "
				   "master(n%d) normal pfn=0x%lx "
				   "replica(n%d) still protnone pfn=0x%lx\n",
				   a, master_node, pte_pfn(m_val),
				   replica_node, pte_pfn(r_val));
			continue;
		}

		if (pte_present(m_val) && pte_protnone(m_val) &&
		    pte_present(r_val) && !pte_protnone(r_val)) {
			stats->err_protnone_missing++;
			seq_printf(m, "  ERR PTE_PROTNONE_MISSING at 0x%lx: "
				   "master(n%d) protnone pfn=0x%lx "
				   "replica(n%d) normal pfn=0x%lx\n",
				   a, master_node, pte_pfn(m_val),
				   replica_node, pte_pfn(r_val));
			continue;
		}

		if (!pte_present(m_val) || !pte_present(r_val))
			continue;

		if (pte_pfn(m_val) != pte_pfn(r_val)) {
			stats->err_pfn_diverged++;
			seq_printf(m, "  ERR PTE_PFN_DIVERGED at 0x%lx: "
				   "master(n%d) pfn=0x%lx "
				   "replica(n%d) pfn=0x%lx\n",
				   a, master_node, pte_pfn(m_val),
				   replica_node, pte_pfn(r_val));
		}

		if (!pte_write(m_val) && pte_write(r_val)) {
			stats->err_write_excess++;
			seq_printf(m, "  ERR PTE_WRITE_EXCESS at 0x%lx: "
				   "master(n%d) RO replica(n%d) RW\n",
				   a, master_node, replica_node);
		}

		if (pte_dirty(m_val) && !pte_dirty(r_val))
			stats->err_dirty_lost++;
	}
}

static void numa_check_one_vma(struct seq_file *m,
			       struct mm_struct *mm,
			       struct vm_area_struct *vma,
			       int master_node,
			       int replica_node,
			       struct numa_check_stats *stats)
{
	pgd_t *m_pgd, *r_pgd;
	p4d_t *m_p4d, *r_p4d;
	pud_t *m_pud, *r_pud;
	pmd_t *m_pmd, *r_pmd;
	pmd_t m_pmdval, r_pmdval;
	unsigned long addr, next_pgd, next_p4d, next_pud, next_pmd;
	unsigned long end = vma->vm_end;

	addr = vma->vm_start;
	m_pgd = pgd_offset_pgd(mm->repl_pgd[master_node], addr);
	r_pgd = pgd_offset_pgd(mm->repl_pgd[replica_node], addr);

	do {
		next_pgd = pgd_addr_end(addr, end);
		if (pgd_none(*m_pgd) || pgd_bad(*m_pgd))
			goto next_pgd;
		if (pgd_none(*r_pgd) || pgd_bad(*r_pgd))
			goto next_pgd;

		m_p4d = p4d_offset(m_pgd, addr);
		r_p4d = p4d_offset(r_pgd, addr);

		do {
			next_p4d = p4d_addr_end(addr, next_pgd);
			if (p4d_none(*m_p4d) || p4d_bad(*m_p4d))
				goto next_p4d;
			if (p4d_none(*r_p4d) || p4d_bad(*r_p4d))
				goto next_p4d;

			m_pud = pud_offset(m_p4d, addr);
			r_pud = pud_offset(r_p4d, addr);

			do {
				next_pud = pud_addr_end(addr, next_p4d);
				if (pud_none(*m_pud) || pud_bad(*m_pud))
					goto next_pud;
				if (pud_none(*r_pud) || pud_bad(*r_pud))
					goto next_pud;

				m_pmd = pmd_offset(m_pud, addr);
				r_pmd = pmd_offset(r_pud, addr);

				do {
					next_pmd = pmd_addr_end(addr, next_pud);
					m_pmdval = *m_pmd;
					r_pmdval = *r_pmd;

					stats->pmds_checked++;

					if (pmd_present(m_pmdval) && pmd_trans_huge(m_pmdval) &&
					    pmd_protnone(m_pmdval))
						stats->pmd_protnone_master++;

					if (pmd_present(r_pmdval) && pmd_trans_huge(r_pmdval) &&
					    pmd_protnone(r_pmdval))
						stats->pmd_protnone_replica++;

					if (pmd_present(m_pmdval) && pmd_trans_huge(m_pmdval) &&
					    pmd_present(r_pmdval) && pmd_trans_huge(r_pmdval)) {
						if (pmd_protnone(m_pmdval) && !pmd_protnone(r_pmdval)) {
							stats->err_protnone_missing++;
							seq_printf(m, "  ERR PMD_PROTNONE_MISSING at 0x%lx: "
								   "master(n%d) THP protnone pfn=0x%lx "
								   "replica(n%d) THP normal pfn=0x%lx\n",
								   addr, master_node, pmd_pfn(m_pmdval),
								   replica_node, pmd_pfn(r_pmdval));
						}

						if (!pmd_protnone(m_pmdval) && pmd_protnone(r_pmdval)) {
							stats->err_replica_protnone_loop++;
							seq_printf(m, "  ERR PMD_PROTNONE_LOOP at 0x%lx: "
								   "master(n%d) THP normal pfn=0x%lx "
								   "replica(n%d) THP still protnone pfn=0x%lx\n",
								   addr, master_node, pmd_pfn(m_pmdval),
								   replica_node, pmd_pfn(r_pmdval));
						}

						if (pmd_pfn(m_pmdval) != pmd_pfn(r_pmdval)) {
							stats->err_pfn_diverged++;
							seq_printf(m, "  ERR PMD_PFN_DIVERGED at 0x%lx: "
								   "master(n%d) pfn=0x%lx "
								   "replica(n%d) pfn=0x%lx\n",
								   addr, master_node, pmd_pfn(m_pmdval),
								   replica_node, pmd_pfn(r_pmdval));
						}

						if (!pmd_write(m_pmdval) && pmd_write(r_pmdval)) {
							stats->err_write_excess++;
							seq_printf(m, "  ERR PMD_WRITE_EXCESS at 0x%lx: "
								   "master(n%d) RO "
								   "replica(n%d) RW\n",
								   addr, master_node, replica_node);
						}

						if (pmd_dirty(m_pmdval) && !pmd_dirty(r_pmdval))
							stats->err_dirty_lost++;

						goto next_pmd;
					}

					if (!pmd_present(m_pmdval) &&
					    pmd_present(r_pmdval) && pmd_trans_huge(r_pmdval) &&
					    pmd_protnone(r_pmdval)) {
						stats->err_protnone_stale++;
						seq_printf(m, "  ERR PMD_PROTNONE_STALE at 0x%lx: "
							   "master(n%d) not present "
							   "replica(n%d) THP protnone pfn=0x%lx\n",
							   addr, master_node,
							   replica_node, pmd_pfn(r_pmdval));
						goto next_pmd;
					}

					if (pmd_present(m_pmdval) && !pmd_trans_huge(m_pmdval) &&
					    !pmd_bad(m_pmdval) &&
					    pmd_present(r_pmdval) && !pmd_trans_huge(r_pmdval) &&
					    !pmd_bad(r_pmdval)) {
						numa_check_pte_range(m, mm, vma,
								     m_pmd, r_pmd,
								     addr, next_pmd,
								     master_node,
								     replica_node,
								     stats);
					}

next_pmd:
					addr = next_pmd;
				} while (m_pmd++, r_pmd++, addr != next_pud);
next_pud:
				addr = next_pud;
			} while (m_pud++, r_pud++, addr != next_p4d);
next_p4d:
			addr = next_p4d;
		} while (m_p4d++, r_p4d++, addr != next_pgd);
next_pgd:
		addr = next_pgd;
	} while (m_pgd++, r_pgd++, addr != end);
}

static int hydra_numa_check_show(struct seq_file *m, void *v)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int i, nr_online = 0;
	int online_nodes[NUMA_NODE_COUNT];
	struct numa_check_stats total;
	struct numa_check_stats per_pair;
	int master_node;
	int total_errors;

	if (!numa_check_pid) {
		seq_printf(m, "Usage: echo '<pid>' > /proc/hydra/numa_check\n");
		seq_printf(m, "Then read this file to see autoNUMA consistency report.\n");
		return 0;
	}

	rcu_read_lock();
	task = find_task_by_vpid(numa_check_pid);
	if (!task) {
		rcu_read_unlock();
		seq_printf(m, "PID %d not found\n", numa_check_pid);
		return 0;
	}
	mm = get_task_mm(task);
	rcu_read_unlock();

	if (!mm) {
		seq_printf(m, "PID %d has no mm\n", numa_check_pid);
		return 0;
	}

	if (!mm->lazy_repl_enabled) {
		seq_printf(m, "PID %d does not have replication enabled\n", numa_check_pid);
		mmput(mm);
		return 0;
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (node_online(i))
			online_nodes[nr_online++] = i;
	}

	memset(&total, 0, sizeof(total));

	seq_printf(m, "AutoNUMA Consistency Check for PID %d (%s)\n",
		   numa_check_pid, task->comm);
	seq_printf(m, "Online nodes: %d\n\n", nr_online);

	mmap_read_lock(mm);

	{
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			master_node = vma->master_pgd_node;

			for (i = 0; i < nr_online; i++) {
				int n = online_nodes[i];
				if (n == master_node)
					continue;
				if (!mm->repl_pgd[n] || mm->repl_pgd[n] == mm->pgd)
					continue;

				memset(&per_pair, 0, sizeof(per_pair));

				numa_check_one_vma(m, mm, vma,
						   master_node, n, &per_pair);

				total.ptes_checked += per_pair.ptes_checked;
				total.pmds_checked += per_pair.pmds_checked;
				total.pte_protnone_master += per_pair.pte_protnone_master;
				total.pte_protnone_replica += per_pair.pte_protnone_replica;
				total.pmd_protnone_master += per_pair.pmd_protnone_master;
				total.pmd_protnone_replica += per_pair.pmd_protnone_replica;
				total.err_protnone_stale += per_pair.err_protnone_stale;
				total.err_protnone_missing += per_pair.err_protnone_missing;
				total.err_pfn_diverged += per_pair.err_pfn_diverged;
				total.err_accessed_lost += per_pair.err_accessed_lost;
				total.err_dirty_lost += per_pair.err_dirty_lost;
				total.err_write_excess += per_pair.err_write_excess;
				total.err_replica_protnone_loop += per_pair.err_replica_protnone_loop;
			}
		}
	}

	mmap_read_unlock(mm);

	seq_printf(m, "\n--- Summary ---\n");
	seq_printf(m, "PTEs checked:                 %lu\n", total.ptes_checked);
	seq_printf(m, "PMDs checked:                 %lu\n", total.pmds_checked);
	seq_printf(m, "\n");
	seq_printf(m, "PTE protnone (master):        %lu\n", total.pte_protnone_master);
	seq_printf(m, "PTE protnone (replica):       %lu\n", total.pte_protnone_replica);
	seq_printf(m, "PMD protnone (master):        %lu\n", total.pmd_protnone_master);
	seq_printf(m, "PMD protnone (replica):       %lu\n", total.pmd_protnone_replica);
	seq_printf(m, "\n");
	seq_printf(m, "ERR protnone_stale:           %lu  "
		   "(master cleared, replica still protnone)\n",
		   total.err_protnone_stale);
	seq_printf(m, "ERR protnone_missing:         %lu  "
		   "(master protnone, replica not)\n",
		   total.err_protnone_missing);
	seq_printf(m, "ERR protnone_loop:            %lu  "
		   "(master resolved, replica stuck protnone)\n",
		   total.err_replica_protnone_loop);
	seq_printf(m, "ERR pfn_diverged:             %lu  "
		   "(different physical page after migration)\n",
		   total.err_pfn_diverged);
	seq_printf(m, "ERR write_excess:             %lu  "
		   "(replica writable, master not)\n",
		   total.err_write_excess);
	seq_printf(m, "ERR dirty_lost:               %lu  "
		   "(master dirty, replica clean)\n",
		   total.err_dirty_lost);

	total_errors = total.err_protnone_stale +
		       total.err_protnone_missing +
		       total.err_pfn_diverged +
		       total.err_write_excess +
		       total.err_dirty_lost +
		       total.err_replica_protnone_loop;

	if (total_errors == 0)
		seq_printf(m, "\nRESULT: PASS");
	else
		seq_printf(m, "\nRESULT: FAIL (%d errors)", total_errors);

	seq_printf(m, "\n");

	mmput(mm);
	return 0;
}

static ssize_t hydra_numa_check_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char kbuf[32];
	int pid;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtoint(kbuf, 10, &pid))
		return -EINVAL;

	if (pid <= 0)
		return -EINVAL;

	numa_check_pid = pid;
	return count;
}

static int hydra_numa_check_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_numa_check_show, NULL);
}

static const struct proc_ops hydra_numa_check_ops = {
	.proc_open = hydra_numa_check_open,
	.proc_read = seq_read,
	.proc_write = hydra_numa_check_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int __init hydra_stats_init(void)
{
	hydra_proc_dir = proc_mkdir("hydra", NULL);
	if (!hydra_proc_dir)
		return -ENOMEM;

	if (!proc_create("tlbflush_opt", 0644, hydra_proc_dir, &hydra_tlbflush_opt_ops))
		goto err_tlbflush;

	if (!proc_create("repl_order", 0644, hydra_proc_dir, &hydra_repl_order_ops))
		goto err_repl_order;

	if (!proc_create("cache", 0644, hydra_proc_dir, &hydra_cache_ops))
		goto err_cache;

	if (!proc_create("status", 0644, hydra_proc_dir, &hydra_status_ops))
		goto err_status;

	if (!proc_create("verify", 0644, hydra_proc_dir, &hydra_verify_ops))
		goto err_verify;

	if (!proc_create("history", 0644, hydra_proc_dir, &hydra_history_ops))
		goto err_history;

	if (!proc_create("walk", 0644, hydra_proc_dir, &hydra_walk_ops))
		goto err_walk;

	if (!proc_create("thp_check", 0644, hydra_proc_dir, &hydra_thp_check_ops))
		goto err_thp_check;

	if (!proc_create("numa_check", 0644, hydra_proc_dir, &hydra_numa_check_ops))
		goto err_numa_check;

	printk(KERN_INFO "[HYDRA]: Proc interface initialized\n");
	return 0;

err_numa_check:
	remove_proc_entry("thp_check", hydra_proc_dir);
err_thp_check:
	remove_proc_entry("walk", hydra_proc_dir);
err_walk:
	remove_proc_entry("history", hydra_proc_dir);
err_history:
	remove_proc_entry("verify", hydra_proc_dir);
err_verify:
	remove_proc_entry("status", hydra_proc_dir);
err_status:
	remove_proc_entry("cache", hydra_proc_dir);
err_cache:
	remove_proc_entry("repl_order", hydra_proc_dir);
err_repl_order:
	remove_proc_entry("tlbflush_opt", hydra_proc_dir);
err_tlbflush:
	remove_proc_entry("hydra", NULL);
	return -ENOMEM;
}

static void __maybe_unused hydra_stats_exit(void)
{
	hydra_cache_drain_all();

	remove_proc_entry("numa_check", hydra_proc_dir);
	remove_proc_entry("thp_check", hydra_proc_dir);
	remove_proc_entry("walk", hydra_proc_dir);
	remove_proc_entry("history", hydra_proc_dir);
	remove_proc_entry("verify", hydra_proc_dir);
	remove_proc_entry("status", hydra_proc_dir);
	remove_proc_entry("cache", hydra_proc_dir);
	remove_proc_entry("repl_order", hydra_proc_dir);
	remove_proc_entry("tlbflush_opt", hydra_proc_dir);
	remove_proc_entry("hydra", NULL);
}

void hydra_verify_fault_walk(struct mm_struct *mm, unsigned long address)
{
	unsigned long cr3_pa;
	pgd_t *pgd_base;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int expected_node;
	int pg_node;

	if (!sysctl_hydra_verify_enabled)
		return;

	cr3_pa = __read_cr3() & PAGE_MASK;
	if (!cr3_pa || !pfn_valid(cr3_pa >> PAGE_SHIFT))
		return;

	pgd_base = __va(cr3_pa);
	expected_node = page_to_nid(pfn_to_page(cr3_pa >> PAGE_SHIFT));

	if (expected_node < 0 || expected_node >= NUMA_NODE_COUNT)
		return;

	pgd = pgd_offset_pgd(pgd_base, address);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return;

	pg_node = page_to_nid(virt_to_page(pgd_offset_pgd(pgd_base, 0)));
	if (pg_node != expected_node) {
		printk(KERN_ERR "[HYDRA] fault walk violation: PGD on node %d expected %d (CR3=0x%lx) addr=0x%lx comm=%s pid=%d\n",
		       pg_node, expected_node, cr3_pa, address, current->comm, current->pid);
		BUG();
	}

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return;

	pud = pud_offset(p4d, address);
	if (!virt_addr_valid(pud))
		return;

	pg_node = page_to_nid(virt_to_page(pud));
	if (pg_node != expected_node) {
		printk(KERN_ERR "[HYDRA] fault walk violation: PUD on node %d expected %d (CR3=0x%lx) addr=0x%lx comm=%s pid=%d\n",
		       pg_node, expected_node, cr3_pa, address, current->comm, current->pid);
		BUG();
	}

	if (pud_none(*pud) || pud_bad(*pud) || pud_trans_huge(*pud) || pud_leaf(*pud))
		return;

	pmd = pmd_offset(pud, address);
	if (!virt_addr_valid(pmd))
		return;

	pg_node = page_to_nid(virt_to_page(pmd));
	if (pg_node != expected_node) {
		printk(KERN_ERR "[HYDRA] fault walk violation: PMD on node %d expected %d (CR3=0x%lx) addr=0x%lx comm=%s pid=%d\n",
		       pg_node, expected_node, cr3_pa, address, current->comm, current->pid);
		BUG();
	}

	if (pmd_none(*pmd) || pmd_trans_huge(*pmd) || pmd_bad(*pmd))
		return;

	pte = pte_offset_kernel(pmd, address);
	if (!virt_addr_valid(pte))
		return;

	pg_node = page_to_nid(virt_to_page(pte));
	if (pg_node != expected_node) {
		printk(KERN_ERR "[HYDRA] fault walk violation: PTE on node %d expected %d (CR3=0x%lx) addr=0x%lx comm=%s pid=%d\n",
		       pg_node, expected_node, cr3_pa, address, current->comm, current->pid);
		BUG();
	}
}
