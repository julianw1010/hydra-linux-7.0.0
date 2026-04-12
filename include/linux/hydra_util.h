#ifndef _LINUX_HYDRA_UTIL_H
#define _LINUX_HYDRA_UTIL_H

#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/nodemask.h>
#include <linux/atomic.h>
#include <linux/highmem.h>
#include <linux/bitmap.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

static inline void hydra_pt_inc(atomic_long_t *cur, atomic_long_t *max)
{
	long new_val, old_max;

	if (!cur || !max)
		return;

	new_val = atomic_long_inc_return(cur);

	old_max = atomic_long_read(max);
	while (new_val > old_max) {
		if (atomic_long_try_cmpxchg(max, &old_max, new_val))
			break;
	}
}

static inline void hydra_pt_dec(atomic_long_t *cur)
{
	if (!cur)
		return;
	atomic_long_dec(cur);
}

extern int hydra_stats_init(void);
void hydra_verify_fault_walk(struct mm_struct *mm, unsigned long address);
extern int sysctl_hydra_verify_enabled;

void migrate_pgtables_to_node(struct mm_struct *mm, pgd_t *pgd, int target_node);
void hydra_reload_cr3(void *info);
int hydra_enable_replication(struct mm_struct *mm);
void hydra_record_exit(struct mm_struct *mm, const char *comm, pid_t pid);

#define for_each_replica(start, cur)                                     \
    for (unsigned int __repl_n = 1,                                      \
             __repl_go = 1;                                              \
         __repl_go; __repl_go = 0)                                       \
        for (cur = READ_ONCE((start)->next_replica);                     \
             cur && cur != (start) && __repl_n < NUMA_NODE_COUNT * 2;    \
             cur = READ_ONCE(cur->next_replica), ++__repl_n)

static inline int hydra_alloc_node(struct mm_struct *mm)
{
	if (!mm->lazy_repl_enabled)
		return numa_node_id();
	if (current->hydra_fault_target_node >= 0)
		return current->hydra_fault_target_node;
	return numa_node_id();
}

struct hydra_node_scope {
	int saved;
	bool active;
};

static inline struct hydra_node_scope
hydra_enter_node_scope(struct mm_struct *mm, int node)
{
	struct hydra_node_scope s = { .saved = -1, .active = false };

	if (mm->lazy_repl_enabled) {
		s.saved = current->hydra_fault_target_node;
		s.active = true;
		current->hydra_fault_target_node = node;
	}
	return s;
}

static inline void hydra_exit_node_scope(struct hydra_node_scope *s)
{
	if (s->active)
		current->hydra_fault_target_node = s->saved;
}

#define HYDRA_FIND_PGD_NONE ((void *)0x1)
#define HYDRA_FIND_P4D_NONE ((void *)0x11)
#define HYDRA_FIND_PUD_NONE ((void *)0x21)
#define HYDRA_FIND_PMD_NONE ((void *)0x31)

#define HYDRA_FIND_BAD(r) (((unsigned long)(r) & 1) == 1)

struct mitosis_pte_tracking {
    DECLARE_BITMAP(propagated, PTRS_PER_PTE);
    DECLARE_BITMAP(ever_accessed, PTRS_PER_PTE);
    atomic_long_t propagation_count;
    atomic_long_t access_count;
};

/*
 * Hydra lockless page table page cache
 *
 * Uses next_replica field for linking (already exists in struct page).
 * One cache per NUMA node.
 * Lockless push/pop using cmpxchg on head pointer.
 */

/* Page table level constants - kept for API compatibility, ignored internally */
#define HYDRA_CACHE_PTE   0
#define HYDRA_CACHE_PMD   1
#define HYDRA_CACHE_PUD   2
#define HYDRA_CACHE_P4D   3
#define HYDRA_CACHE_PGD   4

/* Per-node cache structure with spinlock protection */
struct hydra_cache_head {
	spinlock_t lock;	/* Protects head pointer */
	struct page *head;	/* Head of the free list */
	atomic_t count;		/* Number of pages in cache */
	atomic64_t hits;	/* Cache hit count */
	atomic64_t misses;	/* Cache miss count */
	atomic64_t returns;	/* Pages returned to cache */
	atomic64_t evictions;	/* Pages evicted (cache full) */
} ____cacheline_aligned_in_smp;

/* Global cache array: [node] */
extern struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT];

/*
 * hydra_cache_push - Push a page onto the cache
 * @page: Page to push (must have next_replica field available)
 * @node: NUMA node for cache
 * @level: IGNORED - kept for API compatibility
 *
 * Returns: true if page was cached, false if cache full (page should be freed)
 */
static inline bool hydra_cache_push(struct page *page, int node, int level)
{
	struct hydra_cache_head *cache;
	unsigned long flags;

	(void)level;  /* Unused - kept for API compatibility */

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return false;

	cache = &hydra_cache[node];

	/* Clear any stale flags */
	ClearPageHydraFromCache(page);

	spin_lock_irqsave(&cache->lock, flags);
	page->next_replica = cache->head;
	cache->head = page;
	spin_unlock_irqrestore(&cache->lock, flags);

	atomic_inc(&cache->count);
	atomic64_inc(&cache->returns);

	return true;
}

/*
 * hydra_cache_pop - Pop a page from the cache
 * @node: NUMA node to get page from
 * @level: IGNORED - kept for API compatibility
 *
 * Returns: Page from cache, or NULL if cache empty
 */
static inline struct page *hydra_cache_pop(int node, int level)
{
	struct hydra_cache_head *cache;
	struct page *page;
	unsigned long flags;

	(void)level;  /* Unused - kept for API compatibility */

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return NULL;

	cache = &hydra_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	if (!page) {
		spin_unlock_irqrestore(&cache->lock, flags);
		atomic64_inc(&cache->misses);
		return NULL;
	}
	cache->head = page->next_replica;
	spin_unlock_irqrestore(&cache->lock, flags);

	atomic_dec(&cache->count);
	atomic64_inc(&cache->hits);

	/* Clear linking and mark as from cache */
	page->next_replica = NULL;
	SetPageHydraFromCache(page);

	/* Zero the page for security */
	clear_highpage(page);

	return page;
}

/*
 * hydra_cache_drain_node - Drain all pages from one node's cache
 * @node: NUMA node
 *
 * Returns: Number of pages freed
 */
static inline int hydra_cache_drain_node(int node)
{
	struct hydra_cache_head *cache;
	struct page *page, *next;
	unsigned long flags;
	int freed = 0;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return 0;

	cache = &hydra_cache[node];

	/* Grab entire list under lock */
	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	cache->head = NULL;
	spin_unlock_irqrestore(&cache->lock, flags);

	if (!page)
		return 0;

	atomic_set(&cache->count, 0);

	/* Free all pages */
	while (page) {
		next = page->next_replica;
		page->next_replica = NULL;
		ClearPageHydraFromCache(page);
		__free_page(page);
		freed++;
		page = next;
	}

	return freed;
}

/*
 * hydra_cache_drain_all - Drain all caches on all nodes
 *
 * Returns: Total number of pages freed
 */
static inline int hydra_cache_drain_all(void)
{
	int node, total = 0;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		total += hydra_cache_drain_node(node);
	}

	return total;
}

static inline pte_t *hydra_find_pte(struct mm_struct *mm, unsigned long address, int node) {
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset_node(mm, address, node);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return HYDRA_FIND_PGD_NONE;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return HYDRA_FIND_P4D_NONE;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return HYDRA_FIND_PUD_NONE;

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)) ||
	    unlikely(pmd_trans_huge(*pmd)))
		return HYDRA_FIND_PMD_NONE;

	return pte_offset_kernel(pmd, address);
}

static inline int hydra_collect_repl_nodes(struct page *const ptpage, nodemask_t *nodemask)
{
	struct page *cur;

	node_set(page_to_nid(ptpage), *nodemask);

	for_each_replica(ptpage, cur) {
		node_set(page_to_nid(cur), *nodemask);
	}

	return 0;
}

static inline int hydra_calculate_tlbflush_nodemask(struct page *const ptpage, nodemask_t *nodemask) {

	switch (sysctl_hydra_tlbflush_opt) {
	case 1:
		return hydra_collect_repl_nodes(ptpage, nodemask);
	case 2:
		return hydra_collect_repl_nodes(ptpage, nodemask);
	case 3:
		if (ptpage->next_replica && ptpage->next_replica != ptpage) {
			nodes_clear(*nodemask);
			nodes_or(*nodemask, *nodemask, node_online_map);
		}
		return 0;
	}
	return 1;
}

static inline void hydra_link_page_to_replica_chain(struct page *existing_page,
						    struct page *new_page)
{
	struct page *cur;
	struct page *next_repl;

	if (!existing_page || !new_page || existing_page == new_page)
		return;

	for_each_replica(existing_page, cur) {
		if (cur == new_page)
			return;
	}

	while (1) {
		next_repl = READ_ONCE(existing_page->next_replica);
		WRITE_ONCE(new_page->next_replica, next_repl ? next_repl : existing_page);
		smp_wmb();
		if (cmpxchg(&existing_page->next_replica, next_repl,
			    new_page) == next_repl)
			break;
	}

	/* DEBUG: verify chain integrity after insert */
	{
		struct page *walk;
		int new_count = 0;
		int total = 0;
		int saw_cycle = 0;

		walk = READ_ONCE(existing_page->next_replica);
		while (walk && walk != existing_page && total < NUMA_NODE_COUNT * 4) {
			if (walk == new_page)
				new_count++;
			total++;
			walk = READ_ONCE(walk->next_replica);
		}
		if (walk != existing_page && total >= NUMA_NODE_COUNT * 4)
			saw_cycle = 1;

		if (new_count != 1 || saw_cycle || total > NUMA_NODE_COUNT) {
			printk(KERN_ERR
				"HYDRA CHAIN CORRUPT: existing=%px(n%d) new=%px(n%d) "
				"new_count=%d total=%d cycle=%d caller=%pS\n",
				existing_page, page_to_nid(existing_page),
				new_page, page_to_nid(new_page),
				new_count, total, saw_cycle,
				__builtin_return_address(0));
			dump_stack();
		}
	}
}

static inline void hydra_break_chain(struct page *page)
{
	struct page *cur, *next;

	if (!page || !page->next_replica)
		return;

	cur = page->next_replica;
	WRITE_ONCE(page->next_replica, NULL);

	while (cur && cur != page) {
		next = READ_ONCE(cur->next_replica);
		WRITE_ONCE(cur->next_replica, NULL);
		cur = next;
	}
}

static inline void hydra_defer_pte_page_free(struct mm_struct *mm, struct page *page)
{
	unsigned long flags;

	hydra_break_chain(page);
	pagetable_dtor(page_ptdesc(page));

	if (!mm) {
		ClearPageHydraFromCache(page);
		__free_page(page);
		return;
	}

	spin_lock_irqsave(&mm->hydra_deferred_lock, flags);
	page->next_replica = mm->hydra_deferred_pages;
	mm->hydra_deferred_pages = page;
	spin_unlock_irqrestore(&mm->hydra_deferred_lock, flags);
}

static inline void hydra_drain_deferred_pages(struct mm_struct *mm)
{
	struct page *page, *next;
	unsigned long flags;

	if (!READ_ONCE(mm->hydra_deferred_pages))
		return;

	spin_lock_irqsave(&mm->hydra_deferred_lock, flags);
	page = mm->hydra_deferred_pages;
	mm->hydra_deferred_pages = NULL;
	spin_unlock_irqrestore(&mm->hydra_deferred_lock, flags);

	while (page) {
		int nid = page_to_nid(page);
		bool from_cache = PageHydraFromCache(page);
		next = page->next_replica;
		page->next_replica = NULL;
		ClearPageHydraFromCache(page);
		if (!from_cache || !hydra_cache_push(page, nid, HYDRA_CACHE_PTE))
			__free_page(page);
		page = next;
	}
}

#endif /* _LINUX_HYDRA_UTIL_H */
