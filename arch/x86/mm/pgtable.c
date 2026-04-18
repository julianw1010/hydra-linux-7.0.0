// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hugetlb.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/fixmap.h>
#include <asm/mtrr.h>
#include <linux/swapops.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/hydra_util.h>
#include <linux/mempolicy.h>
#include <linux/slab.h>
#include <asm/hydra_pti.h>

#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
phys_addr_t physical_mask __ro_after_init = (1ULL << __PHYSICAL_MASK_SHIFT) - 1;
EXPORT_SYMBOL(physical_mask);
SYM_PIC_ALIAS(physical_mask);
#endif

pgtable_t pte_alloc_one(struct mm_struct *mm)
{
	return __pte_alloc_one(mm, GFP_PGTABLE_USER);
}

void ___pte_free_tlb(struct mmu_gather *tlb, struct page *pte)
{
	int nid = page_to_nid(pte);
	bool from_cache = PageHydraFromCache(pte);

	if (pte->pt_owner_mm)
		hydra_pt_dec(&pte->pt_owner_mm->hydra_nr_pte[nid]);

	hydra_break_chain(pte);

	paravirt_release_pte(page_to_pfn(pte));
	pagetable_dtor(page_ptdesc(pte));

	if (from_cache) {
		ClearPageHydraFromCache(pte);
		pte->next_replica = NULL;
		if (hydra_cache_push(pte, nid, HYDRA_CACHE_PTE))
			return;
	}

	ClearPageHydraFromCache(pte);
	tlb_remove_ptdesc(tlb, page_ptdesc(pte));
}

#if CONFIG_PGTABLE_LEVELS > 2
void ___pmd_free_tlb(struct mmu_gather *tlb, pmd_t *pmd)
{
	struct page *page = virt_to_page(pmd);
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);

	if (page->pt_owner_mm)
		hydra_pt_dec(&page->pt_owner_mm->hydra_nr_pmd[nid]);

	hydra_break_chain(page);

	paravirt_release_pmd(__pa(pmd) >> PAGE_SHIFT);
#ifdef CONFIG_X86_PAE
	tlb->need_flush_all = 1;
#endif
	pagetable_dtor(virt_to_ptdesc(pmd));

	if (from_cache) {
		ClearPageHydraFromCache(page);
		page->next_replica = NULL;
		if (hydra_cache_push(page, nid, HYDRA_CACHE_PMD))
			return;
	}

	ClearPageHydraFromCache(page);
	tlb_remove_ptdesc(tlb, virt_to_ptdesc(pmd));
}

#if CONFIG_PGTABLE_LEVELS > 3
void ___pud_free_tlb(struct mmu_gather *tlb, pud_t *pud)
{
	struct page *page = virt_to_page(pud);
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);

	if (page->pt_owner_mm)
		hydra_pt_dec(&page->pt_owner_mm->hydra_nr_pud[nid]);

	paravirt_release_pud(__pa(pud) >> PAGE_SHIFT);

	if (from_cache) {
		ClearPageHydraFromCache(page);
		page->next_replica = NULL;
		if (hydra_cache_push(page, nid, HYDRA_CACHE_PUD))
			return;
	}

	ClearPageHydraFromCache(page);
	tlb_remove_ptdesc(tlb, virt_to_ptdesc(pud));
}

#if CONFIG_PGTABLE_LEVELS > 4
void ___p4d_free_tlb(struct mmu_gather *tlb, p4d_t *p4d)
{
	struct page *page = virt_to_page(p4d);
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);

	if (page->pt_owner_mm)
		hydra_pt_dec(&page->pt_owner_mm->hydra_nr_p4d[nid]);

	paravirt_release_p4d(__pa(p4d) >> PAGE_SHIFT);

	if (from_cache) {
		ClearPageHydraFromCache(page);
		page->next_replica = NULL;
		if (hydra_cache_push(page, nid, HYDRA_CACHE_P4D))
			return;
	}

	ClearPageHydraFromCache(page);
	tlb_remove_ptdesc(tlb, virt_to_ptdesc(p4d));
}
#endif	/* CONFIG_PGTABLE_LEVELS > 4 */
#endif	/* CONFIG_PGTABLE_LEVELS > 3 */
#endif	/* CONFIG_PGTABLE_LEVELS > 2 */

static inline void pgd_list_add(pgd_t *pgd)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pgd);

	list_add(&ptdesc->pt_list, &pgd_list);
}

static inline void pgd_list_del(pgd_t *pgd)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pgd);

	list_del(&ptdesc->pt_list);
}

static void pgd_set_mm(pgd_t *pgd, struct mm_struct *mm)
{
	virt_to_ptdesc(pgd)->pt_mm = mm;
}

struct mm_struct *pgd_page_get_mm(struct page *page)
{
	return page_ptdesc(page)->pt_mm;
}

static void pgd_ctor(struct mm_struct *mm, pgd_t *pgd)
{
	/* PAE preallocates all its PMDs.  No cloning needed. */
	if (!IS_ENABLED(CONFIG_X86_PAE))
		clone_pgd_range(pgd + KERNEL_PGD_BOUNDARY,
				swapper_pg_dir + KERNEL_PGD_BOUNDARY,
				KERNEL_PGD_PTRS);

	/* List used to sync kernel mapping updates */
	pgd_set_mm(pgd, mm);
	pgd_list_add(pgd);
}

void pgd_dtor(pgd_t *pgd)
{
	spin_lock(&pgd_lock);
	pgd_list_del(pgd);
	spin_unlock(&pgd_lock);
}

/*
 * List of all pgd's needed for non-PAE so it can invalidate entries
 * in both cached and uncached pgd's; not needed for PAE since the
 * kernel pmd is shared. If PAE were not to share the pmd a similar
 * tactic would be needed. This is essentially codepath-based locking
 * against pageattr.c; it is the unique case in which a valid change
 * of kernel pagetables can't be lazily synchronized by vmalloc faults.
 * vmalloc faults work because attached pagetables are never freed.
 * -- nyc
 */

#ifdef CONFIG_X86_PAE
/*
 * In PAE mode, we need to do a cr3 reload (=tlb flush) when
 * updating the top-level pagetable entries to guarantee the
 * processor notices the update.  Since this is expensive, and
 * all 4 top-level entries are used almost immediately in a
 * new process's life, we just pre-populate them here.
 */
#define PREALLOCATED_PMDS	PTRS_PER_PGD

/*
 * "USER_PMDS" are the PMDs for the user copy of the page tables when
 * PTI is enabled. They do not exist when PTI is disabled.  Note that
 * this is distinct from the user _portion_ of the kernel page tables
 * which always exists.
 *
 * We allocate separate PMDs for the kernel part of the user page-table
 * when PTI is enabled. We need them to map the per-process LDT into the
 * user-space page-table.
 */
#define PREALLOCATED_USER_PMDS	 (boot_cpu_has(X86_FEATURE_PTI) ? \
					KERNEL_PGD_PTRS : 0)
#define MAX_PREALLOCATED_USER_PMDS KERNEL_PGD_PTRS

void pud_populate(struct mm_struct *mm, pud_t *pudp, pmd_t *pmd)
{
	paravirt_alloc_pmd(mm, __pa(pmd) >> PAGE_SHIFT);

	/* Note: almost everything apart from _PAGE_PRESENT is
	   reserved at the pmd (PDPT) level. */
	set_pud(pudp, __pud(__pa(pmd) | _PAGE_PRESENT));

	/*
	 * According to Intel App note "TLBs, Paging-Structure Caches,
	 * and Their Invalidation", April 2007, document 317080-001,
	 * section 8.1: in PAE mode we explicitly have to flush the
	 * TLB via cr3 if the top-level pgd is changed...
	 */
	flush_tlb_mm(mm);
}
#else  /* !CONFIG_X86_PAE */

/* No need to prepopulate any pagetable entries in non-PAE modes. */
#define PREALLOCATED_PMDS	0
#define PREALLOCATED_USER_PMDS	 0
#define MAX_PREALLOCATED_USER_PMDS 0
#endif	/* CONFIG_X86_PAE */

static void free_pmds(struct mm_struct *mm, pmd_t *pmds[], int count)
{
	int i;
	struct ptdesc *ptdesc;

	for (i = 0; i < count; i++)
		if (pmds[i]) {
			ptdesc = virt_to_ptdesc(pmds[i]);

			pagetable_dtor(ptdesc);
			pagetable_free(ptdesc);
			mm_dec_nr_pmds(mm);
		}
}

static int preallocate_pmds(struct mm_struct *mm, pmd_t *pmds[], int count)
{
	int i;
	bool failed = false;
	gfp_t gfp = GFP_PGTABLE_USER;

	if (mm == &init_mm)
		gfp &= ~__GFP_ACCOUNT;
	gfp &= ~__GFP_HIGHMEM;

	for (i = 0; i < count; i++) {
		pmd_t *pmd = NULL;
		struct ptdesc *ptdesc = pagetable_alloc(gfp, 0);

		if (!ptdesc)
			failed = true;
		if (ptdesc && !pagetable_pmd_ctor(mm, ptdesc)) {
			pagetable_free(ptdesc);
			ptdesc = NULL;
			failed = true;
		}
		if (ptdesc) {
			mm_inc_nr_pmds(mm);
			pmd = ptdesc_address(ptdesc);
		}

		pmds[i] = pmd;
	}

	if (failed) {
		free_pmds(mm, pmds, count);
		return -ENOMEM;
	}

	return 0;
}

/*
 * Mop up any pmd pages which may still be attached to the pgd.
 * Normally they will be freed by munmap/exit_mmap, but any pmd we
 * preallocate which never got a corresponding vma will need to be
 * freed manually.
 */
static void mop_up_one_pmd(struct mm_struct *mm, pgd_t *pgdp)
{
	pgd_t pgd = *pgdp;

	if (pgd_val(pgd) != 0) {
		pmd_t *pmd = (pmd_t *)pgd_page_vaddr(pgd);

		pgd_clear(pgdp);

		paravirt_release_pmd(pgd_val(pgd) >> PAGE_SHIFT);
		pmd_free(mm, pmd);
		mm_dec_nr_pmds(mm);
	}
}

static void pgd_mop_up_pmds(struct mm_struct *mm, pgd_t *pgdp)
{
	int i;

	for (i = 0; i < PREALLOCATED_PMDS; i++)
		mop_up_one_pmd(mm, &pgdp[i]);

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION

	if (!boot_cpu_has(X86_FEATURE_PTI))
		return;

	pgdp = kernel_to_user_pgdp(pgdp);

	for (i = 0; i < PREALLOCATED_USER_PMDS; i++)
		mop_up_one_pmd(mm, &pgdp[i + KERNEL_PGD_BOUNDARY]);
#endif
}

static void pgd_prepopulate_pmd(struct mm_struct *mm, pgd_t *pgd, pmd_t *pmds[])
{
	p4d_t *p4d;
	pud_t *pud;
	int i;

	p4d = p4d_offset(pgd, 0);
	pud = pud_offset(p4d, 0);

	for (i = 0; i < PREALLOCATED_PMDS; i++, pud++) {
		pmd_t *pmd = pmds[i];

		if (i >= KERNEL_PGD_BOUNDARY)
			memcpy(pmd, (pmd_t *)pgd_page_vaddr(swapper_pg_dir[i]),
			       sizeof(pmd_t) * PTRS_PER_PMD);

		pud_populate(mm, pud, pmd);
	}
}

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
static void pgd_prepopulate_user_pmd(struct mm_struct *mm,
				     pgd_t *k_pgd, pmd_t *pmds[])
{
	pgd_t *s_pgd = kernel_to_user_pgdp(swapper_pg_dir);
	pgd_t *u_pgd = kernel_to_user_pgdp(k_pgd);
	p4d_t *u_p4d;
	pud_t *u_pud;
	int i;

	u_p4d = p4d_offset(u_pgd, 0);
	u_pud = pud_offset(u_p4d, 0);

	s_pgd += KERNEL_PGD_BOUNDARY;
	u_pud += KERNEL_PGD_BOUNDARY;

	for (i = 0; i < PREALLOCATED_USER_PMDS; i++, u_pud++, s_pgd++) {
		pmd_t *pmd = pmds[i];

		memcpy(pmd, (pmd_t *)pgd_page_vaddr(*s_pgd),
		       sizeof(pmd_t) * PTRS_PER_PMD);

		pud_populate(mm, u_pud, pmd);
	}

}
#else
static void pgd_prepopulate_user_pmd(struct mm_struct *mm,
				     pgd_t *k_pgd, pmd_t *pmds[])
{
}
#endif

static inline pgd_t *_pgd_alloc(struct mm_struct *mm)
{
	struct page *page;
	int node;
	int order = hydra_pgd_alloc_order();
	gfp_t gfp = GFP_PGTABLE_USER;

	if (mm->lazy_repl_enabled) {
		node = hydra_alloc_node(mm);
		gfp |= __GFP_THISNODE;

		if (order == 0) {
			page = hydra_cache_pop(node, HYDRA_CACHE_PGD);
			if (page) {
				page->pt_owner_mm = mm;
				hydra_pt_inc(&mm->hydra_nr_pgd[page_to_nid(page)],
					     &mm->hydra_max_pgd[page_to_nid(page)]);
				return (pgd_t *)page_address(page);
			}
		}

		page = alloc_pages_node(node, gfp, order);
	} else {
		pgd_t *pgd = __pgd_alloc(mm, pgd_allocation_order());
		if (pgd) {
			struct page *p = virt_to_page(pgd);
			p->pt_owner_mm = mm;
			hydra_pt_inc(&mm->hydra_nr_pgd[page_to_nid(p)],
				     &mm->hydra_max_pgd[page_to_nid(p)]);
		}
		return pgd;
	}

	if (!page)
		return NULL;

	page->pt_owner_mm = mm;
	hydra_pt_inc(&mm->hydra_nr_pgd[page_to_nid(page)],
		     &mm->hydra_max_pgd[page_to_nid(page)]);
	return (pgd_t *)page_address(page);
}

static inline void _pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);
	int order = hydra_pgd_alloc_order();
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);

	if (page->pt_owner_mm)
		hydra_pt_dec(&page->pt_owner_mm->hydra_nr_pgd[nid]);

	page->pt_owner_mm = NULL;

	if (order == 0 && from_cache) {
		ClearPageHydraFromCache(page);
		page->next_replica = NULL;
		if (hydra_cache_push(page, nid, HYDRA_CACHE_PGD))
			return;
	}

	ClearPageHydraFromCache(page);
	__pgd_free(mm, pgd);
}

pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd;
	pmd_t *u_pmds[MAX_PREALLOCATED_USER_PMDS];
	pmd_t *pmds[PREALLOCATED_PMDS];

	pgd = _pgd_alloc(mm);

	if (pgd == NULL)
		goto out;

	mm->pgd = pgd;

	if (sizeof(pmds) != 0 &&
			preallocate_pmds(mm, pmds, PREALLOCATED_PMDS) != 0)
		goto out_free_pgd;

	if (sizeof(u_pmds) != 0 &&
			preallocate_pmds(mm, u_pmds, PREALLOCATED_USER_PMDS) != 0)
		goto out_free_pmds;

	if (paravirt_pgd_alloc(mm) != 0)
		goto out_free_user_pmds;

	/*
	 * Make sure that pre-populating the pmds is atomic with
	 * respect to anything walking the pgd_list, so that they
	 * never see a partially populated pgd.
	 */
	spin_lock(&pgd_lock);

	pgd_ctor(mm, pgd);
	if (sizeof(pmds) != 0)
		pgd_prepopulate_pmd(mm, pgd, pmds);

	if (sizeof(u_pmds) != 0)
		pgd_prepopulate_user_pmd(mm, pgd, u_pmds);

	spin_unlock(&pgd_lock);

	return pgd;

out_free_user_pmds:
	if (sizeof(u_pmds) != 0)
		free_pmds(mm, u_pmds, PREALLOCATED_USER_PMDS);
out_free_pmds:
	if (sizeof(pmds) != 0)
		free_pmds(mm, pmds, PREALLOCATED_PMDS);
out_free_pgd:
	_pgd_free(mm, pgd);
out:
	return NULL;
}

void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	pgd_mop_up_pmds(mm, pgd);
	pgd_dtor(pgd);
	paravirt_pgd_free(mm, pgd);
	_pgd_free(mm, pgd);
}

/*
 * Used to set accessed or dirty bits in the page table entries
 * on other architectures. On x86, the accessed and dirty bits
 * are tracked by hardware. However, do_wp_page calls this function
 * to also make the pte writeable at the same time the dirty bit is
 * set. In that case we do actually need to write the PTE.
 */
int ptep_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pte_t *ptep,
			  pte_t entry, int dirty)
{
	int changed = !pte_same(*ptep, entry);

	if (changed && dirty)
		set_pte(ptep, entry);

	return changed;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pmd_t *pmdp,
			  pmd_t entry, int dirty)
{
	int changed = !pmd_same(*pmdp, entry);

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);

	if (changed && dirty) {
		set_pmd(pmdp, entry);
		/*
		 * We had a write-protection fault here and changed the pmd
		 * to to more permissive. No need to flush the TLB for that,
		 * #PF is architecturally guaranteed to do that and in the
		 * worst-case we'll generate a spurious fault.
		 */
	}

	return changed;
}

int pudp_set_access_flags(struct vm_area_struct *vma, unsigned long address,
			  pud_t *pudp, pud_t entry, int dirty)
{
	int changed = !pud_same(*pudp, entry);

	VM_BUG_ON(address & ~HPAGE_PUD_MASK);

	if (changed && dirty) {
		set_pud(pudp, entry);
		/*
		 * We had a write-protection fault here and changed the pud
		 * to to more permissive. No need to flush the TLB for that,
		 * #PF is architecturally guaranteed to do that and in the
		 * worst-case we'll generate a spurious fault.
		 */
	}

	return changed;
}
#endif

int ptep_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pte_t *ptep)
{
	struct page *start_pte_page;
	struct page *cur;
	struct mm_struct *mm = vma->vm_mm;
	long offset;
	int ret = 0;
	long touched = 1;

	start_pte_page = virt_to_page(ptep);

	if (pte_young(*ptep))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *) &ptep->pte);

	if (!READ_ONCE(start_pte_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_ptep_test_clear_young_calls);
			atomic_long_add(touched, &mm->hydra_fn_ptep_test_clear_young_pages);
		}
		return ret;
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	for_each_replica(start_pte_page, cur) {
		pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
		if (pte_present(*rp) && pte_young(*rp)) {
			ret |= test_and_clear_bit(_PAGE_BIT_ACCESSED,
						  (unsigned long *) &rp->pte);
		}
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_ptep_test_clear_young_calls);
		atomic_long_add(touched, &mm->hydra_fn_ptep_test_clear_young_pages);
	}

	return ret;
}

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG)
int pmdp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long offset;
	int ret = 0;
	long touched = 1;

	if (pmd_young(*pmdp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pmdp);

	if (!virt_addr_valid(pmdp)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_test_clear_young_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_test_clear_young_pages);
		}
		return ret;
	}

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_test_clear_young_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_test_clear_young_pages);
		}
		return ret;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);

		if (pmd_present(*replica_entry) && pmd_trans_huge(*replica_entry)) {
			if (pmd_young(*replica_entry)) {
				if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
						       (unsigned long *)replica_entry))
					ret = 1;
			}
		}
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_pmdp_test_clear_young_calls);
		atomic_long_add(touched, &mm->hydra_fn_pmdp_test_clear_young_pages);
	}

	return ret;
}
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pudp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pud_t *pudp)
{
	int ret = 0;

	if (pud_young(*pudp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pudp);

	return ret;
}
#endif

int ptep_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep)
{
	/*
	 * On x86 CPUs, clearing the accessed bit without a TLB flush
	 * doesn't cause data corruption. [ It could cause incorrect
	 * page aging and the (mistaken) reclaim of hot pages, but the
	 * chance of that should be relatively low. ]
	 *
	 * So as a performance optimization don't flush the TLB when
	 * clearing the accessed bit, it will eventually be flushed by
	 * a context switch or a VM operation anyway. [ In the rare
	 * event of it not getting flushed for a long time the delay
	 * shouldn't really matter because there's no real memory
	 * pressure for swapout to react to. ]
	 */
	return ptep_test_and_clear_young(vma, address, ptep);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pmd_t *pmdp)
{
	int young;

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);

	young = pmdp_test_and_clear_young(vma, address, pmdp);
	if (young)
		flush_tlb_range(vma, address, address + HPAGE_PMD_SIZE);

	return young;
}

pmd_t pmdp_invalidate_ad(struct vm_area_struct *vma, unsigned long address,
			 pmd_t *pmdp)
{
	VM_WARN_ON_ONCE(!pmd_present(*pmdp));

	/*
	 * No flush is necessary. Once an invalid PTE is established, the PTE's
	 * access and dirty bits cannot be updated.
	 */
	return pmdp_establish(vma, address, pmdp, pmd_mkinvalid(*pmdp));
}
#endif

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && \
	defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
pud_t pudp_invalidate(struct vm_area_struct *vma, unsigned long address,
		     pud_t *pudp)
{
	VM_WARN_ON_ONCE(!pud_present(*pudp));
	pud_t old = pudp_establish(vma, address, pudp, pud_mkinvalid(*pudp));
	flush_pud_tlb_range(vma, address, address + HPAGE_PUD_SIZE);
	return old;
}
#endif

/**
 * reserve_top_address - Reserve a hole in the top of the kernel address space
 * @reserve: Size of hole to reserve
 *
 * Can be used to relocate the fixmap area and poke a hole in the top
 * of the kernel address space to make room for a hypervisor.
 */
void __init reserve_top_address(unsigned long reserve)
{
#ifdef CONFIG_X86_32
	BUG_ON(fixmaps_set > 0);
	__FIXADDR_TOP = round_down(-reserve, 1 << PMD_SHIFT) - PAGE_SIZE;
	printk(KERN_INFO "Reserving virtual address space above 0x%08lx (rounded to 0x%08lx)\n",
	       -reserve, __FIXADDR_TOP + PAGE_SIZE);
#endif
}

int fixmaps_set;

void __native_set_fixmap(enum fixed_addresses idx, pte_t pte)
{
	unsigned long address = __fix_to_virt(idx);

#ifdef CONFIG_X86_64
       /*
	* Ensure that the static initial page tables are covering the
	* fixmap completely.
	*/
	BUILD_BUG_ON(__end_of_permanent_fixed_addresses >
		     (FIXMAP_PMD_NUM * PTRS_PER_PTE));
#endif

	if (idx >= __end_of_fixed_addresses) {
		BUG();
		return;
	}
	set_pte_vaddr(address, pte);
	fixmaps_set++;
}

void native_set_fixmap(unsigned /* enum fixed_addresses */ idx,
		       phys_addr_t phys, pgprot_t flags)
{
	/* Sanitize 'prot' against any unsupported bits: */
	pgprot_val(flags) &= __default_kernel_pte_mask;

	__native_set_fixmap(idx, pfn_pte(phys >> PAGE_SHIFT, flags));
}

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
#if CONFIG_PGTABLE_LEVELS > 4
/**
 * p4d_set_huge - Set up kernel P4D mapping
 * @p4d: Pointer to the P4D entry
 * @addr: Virtual address associated with the P4D entry
 * @prot: Protection bits to use
 *
 * No 512GB pages yet -- always return 0
 */
int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}

/**
 * p4d_clear_huge - Clear kernel P4D mapping when it is set
 * @p4d: Pointer to the P4D entry to clear
 *
 * No 512GB pages yet -- do nothing
 */
void p4d_clear_huge(p4d_t *p4d)
{
}
#endif

/**
 * pud_set_huge - Set up kernel PUD mapping
 * @pud: Pointer to the PUD entry
 * @addr: Virtual address associated with the PUD entry
 * @prot: Protection bits to use
 *
 * MTRRs can override PAT memory types with 4KiB granularity. Therefore, this
 * function sets up a huge page only if the complete range has the same MTRR
 * caching mode.
 *
 * Callers should try to decrease page size (1GB -> 2MB -> 4K) if the bigger
 * page mapping attempt fails.
 *
 * Returns 1 on success and 0 on failure.
 */
int pud_set_huge(pud_t *pud, phys_addr_t addr, pgprot_t prot)
{
	u8 uniform;

	mtrr_type_lookup(addr, addr + PUD_SIZE, &uniform);
	if (!uniform)
		return 0;

	/* Bail out if we are we on a populated non-leaf entry: */
	if (pud_present(*pud) && !pud_leaf(*pud))
		return 0;

	set_pte((pte_t *)pud, pfn_pte(
		(u64)addr >> PAGE_SHIFT,
		__pgprot(protval_4k_2_large(pgprot_val(prot)) | _PAGE_PSE)));

	return 1;
}

/**
 * pmd_set_huge - Set up kernel PMD mapping
 * @pmd: Pointer to the PMD entry
 * @addr: Virtual address associated with the PMD entry
 * @prot: Protection bits to use
 *
 * See text over pud_set_huge() above.
 *
 * Returns 1 on success and 0 on failure.
 */
int pmd_set_huge(pmd_t *pmd, phys_addr_t addr, pgprot_t prot)
{
	u8 uniform;

	mtrr_type_lookup(addr, addr + PMD_SIZE, &uniform);
	if (!uniform) {
		pr_warn_once("%s: Cannot satisfy [mem %#010llx-%#010llx] with a huge-page mapping due to MTRR override.\n",
			     __func__, addr, addr + PMD_SIZE);
		return 0;
	}

	/* Bail out if we are we on a populated non-leaf entry: */
	if (pmd_present(*pmd) && !pmd_leaf(*pmd))
		return 0;

	set_pte((pte_t *)pmd, pfn_pte(
		(u64)addr >> PAGE_SHIFT,
		__pgprot(protval_4k_2_large(pgprot_val(prot)) | _PAGE_PSE)));

	return 1;
}

/**
 * pud_clear_huge - Clear kernel PUD mapping when it is set
 * @pud: Pointer to the PUD entry to clear.
 *
 * Returns 1 on success and 0 on failure (no PUD map is found).
 */
int pud_clear_huge(pud_t *pud)
{
	if (pud_leaf(*pud)) {
		pud_clear(pud);
		return 1;
	}

	return 0;
}

/**
 * pmd_clear_huge - Clear kernel PMD mapping when it is set
 * @pmd: Pointer to the PMD entry to clear.
 *
 * Returns 1 on success and 0 on failure (no PMD map is found).
 */
int pmd_clear_huge(pmd_t *pmd)
{
	if (pmd_leaf(*pmd)) {
		pmd_clear(pmd);
		return 1;
	}

	return 0;
}

#ifdef CONFIG_X86_64
/**
 * pud_free_pmd_page - Clear PUD entry and free PMD page
 * @pud: Pointer to a PUD
 * @addr: Virtual address associated with PUD
 *
 * Context: The PUD range has been unmapped and TLB purged.
 * Return: 1 if clearing the entry succeeded. 0 otherwise.
 *
 * NOTE: Callers must allow a single page allocation.
 */
int pud_free_pmd_page(pud_t *pud, unsigned long addr)
{
	pmd_t *pmd, *pmd_sv;
	struct ptdesc *pt;
	int i;

	pmd = pud_pgtable(*pud);
	pmd_sv = (pmd_t *)__get_free_page(GFP_KERNEL);
	if (!pmd_sv)
		return 0;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd_sv[i] = pmd[i];
		if (!pmd_none(pmd[i]))
			pmd_clear(&pmd[i]);
	}

	pud_clear(pud);

	/* INVLPG to clear all paging-structure caches */
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE-1);

	for (i = 0; i < PTRS_PER_PMD; i++) {
		if (!pmd_none(pmd_sv[i])) {
			pt = page_ptdesc(pmd_page(pmd_sv[i]));
			pagetable_dtor_free(pt);
		}
	}

	free_page((unsigned long)pmd_sv);

	pmd_free(&init_mm, pmd);

	return 1;
}

/**
 * pmd_free_pte_page - Clear PMD entry and free PTE page.
 * @pmd: Pointer to the PMD
 * @addr: Virtual address associated with PMD
 *
 * Context: The PMD range has been unmapped and TLB purged.
 * Return: 1 if clearing the entry succeeded. 0 otherwise.
 */
int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	struct ptdesc *pt;

	pt = page_ptdesc(pmd_page(*pmd));
	pmd_clear(pmd);

	/* INVLPG to clear all paging-structure caches */
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE-1);

	pagetable_dtor_free(pt);

	return 1;
}

#else /* !CONFIG_X86_64 */

/*
 * Disable free page handling on x86-PAE. This assures that ioremap()
 * does not update sync'd PMD entries. See vmalloc_sync_one().
 */
int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	return pmd_none(*pmd);
}

#endif /* CONFIG_X86_64 */
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMAP */

pte_t pte_mkwrite(pte_t pte, struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_SHADOW_STACK)
		return pte_mkwrite_shstk(pte);

	pte = pte_mkwrite_novma(pte);

	return pte_clear_saveddirty(pte);
}

pmd_t pmd_mkwrite(pmd_t pmd, struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_SHADOW_STACK)
		return pmd_mkwrite_shstk(pmd);

	pmd = pmd_mkwrite_novma(pmd);

	return pmd_clear_saveddirty(pmd);
}

void arch_check_zapped_pte(struct vm_area_struct *vma, pte_t pte)
{
	/*
	 * Hardware before shadow stack can (rarely) set Dirty=1
	 * on a Write=0 PTE. So the below condition
	 * only indicates a software bug when shadow stack is
	 * supported by the HW. This checking is covered in
	 * pte_shstk().
	 */
	VM_WARN_ON_ONCE(!(vma->vm_flags & VM_SHADOW_STACK) &&
			pte_shstk(pte));
}

void arch_check_zapped_pmd(struct vm_area_struct *vma, pmd_t pmd)
{
	/* See note in arch_check_zapped_pte() */
	VM_WARN_ON_ONCE(!(vma->vm_flags & VM_SHADOW_STACK) &&
			pmd_shstk(pmd));
}

void arch_check_zapped_pud(struct vm_area_struct *vma, pud_t pud)
{
	/* See note in arch_check_zapped_pte() */
	VM_WARN_ON_ONCE(!(vma->vm_flags & VM_SHADOW_STACK) && pud_shstk(pud));
}

pgtable_t repl_pte_alloc_one(struct mm_struct *mm, unsigned long address, size_t nid, size_t owner_node)
{
	struct page *pte;
	struct ptdesc *ptdesc;

	pte = hydra_cache_pop(nid, HYDRA_CACHE_PTE);
	if (pte) {
		ptdesc = page_ptdesc(pte);
		if (!pagetable_pte_ctor(mm, ptdesc)) {
			if (!hydra_cache_push(pte, nid, HYDRA_CACHE_PTE)) {
				__free_page(pte);
			}
			return NULL;
		}

		pte->pt_owner_mm = mm;
		pte->mitosis_tracking = NULL;
		hydra_pt_inc(&mm->hydra_nr_pte[page_to_nid(pte)],
			     &mm->hydra_max_pte[page_to_nid(pte)]);
		return pte;
	}

	pte = repl_alloc_page_on_node(nid, 0);
	if (!pte)
		return NULL;

	ptdesc = page_ptdesc(pte);
	if (!pagetable_pte_ctor(mm, ptdesc)) {
		__free_page(pte);
		return NULL;
	}
	pte->pt_owner_mm = mm;
	pte->mitosis_tracking = NULL;
	hydra_pt_inc(&mm->hydra_nr_pte[page_to_nid(pte)],
		     &mm->hydra_max_pte[page_to_nid(pte)]);
	return pte;
}

pgd_t *repl_pgd_alloc(struct mm_struct *mm, size_t nid)
{
	pgd_t *pgd;
	pmd_t *pmds[PREALLOCATED_PMDS];
	struct page *page;
	int order = hydra_pgd_alloc_order();
	nodemask_t nm = NODE_MASK_NONE;
	bool from_cache = false;

	if (order == 0) {
		page = hydra_cache_pop(nid, HYDRA_CACHE_PGD);
		if (page) {
			from_cache = true;
			goto got_page;
		}
	}

	node_set(nid, nm);
	page = __alloc_pages(GFP_PGTABLE_USER | __GFP_THISNODE, order, nid, &nm);
	if (!page)
		goto out;

	page->next_replica = NULL;

got_page:
	page->pt_owner_mm = mm;
	hydra_pt_inc(&mm->hydra_nr_pgd[page_to_nid(page)],
		     &mm->hydra_max_pgd[page_to_nid(page)]);
	pgd = (pgd_t *)page_address(page);

	if (PREALLOCATED_PMDS > 0) {
		if (preallocate_pmds(mm, pmds, PREALLOCATED_PMDS) != 0)
			goto out_free_page;
	}

	spin_lock(&pgd_lock);
	pgd_ctor(mm, pgd);
	pgd_prepopulate_pmd(mm, pgd, pmds);
	spin_unlock(&pgd_lock);

	return pgd;

out_free_page:
	hydra_pt_dec(&mm->hydra_nr_pgd[page_to_nid(page)]);
	page->pt_owner_mm = NULL;
	if (order == 0 && from_cache) {
		ClearPageHydraFromCache(page);
		page->next_replica = NULL;
		if (hydra_cache_push(page, nid, HYDRA_CACHE_PGD))
			goto out;
	}

	ClearPageHydraFromCache(page);
	free_pages((unsigned long)pgd, order);
out:
	return NULL;
}

void pgtable_track_set_pgd(pgd_t *pgdp, pgd_t pgd)
{
	pgd_t *user_entry;
	struct mm_struct *mm = NULL;
	long touched = 1;

	native_set_pgd(pgdp, pgd);

	user_entry = hydra_get_user_pgd_entry(pgdp);
	if (user_entry) {
		WRITE_ONCE(*user_entry, __pgd(pgd_val(pgd)));
		touched++;
	}

	if (virt_addr_valid(pgdp))
		mm = virt_to_page(pgdp)->pt_owner_mm;
	if (mm) {
		atomic_long_inc(&mm->hydra_fn_track_set_pgd_calls);
		atomic_long_add(touched, &mm->hydra_fn_track_set_pgd_pages);
	}
}

void pgtable_track_set_p4d(p4d_t *p4dp, p4d_t p4d)
{
	pgd_t *user_entry;
	struct mm_struct *mm = NULL;
	long touched = 1;

	native_set_p4d(p4dp, p4d);

	if (!pgtable_l5_enabled()) {
		user_entry = hydra_get_user_pgd_entry((pgd_t *)p4dp);
		if (user_entry) {
			WRITE_ONCE(*user_entry, __pgd(p4d_val(p4d)));
			touched++;
		}
	}

	if (virt_addr_valid(p4dp))
		mm = virt_to_page(p4dp)->pt_owner_mm;
	if (mm) {
		atomic_long_inc(&mm->hydra_fn_track_set_p4d_calls);
		atomic_long_add(touched, &mm->hydra_fn_track_set_p4d_pages);
	}
}

void pgtable_track_set_pud(pud_t *pudp, pud_t pud)
{
	struct mm_struct *mm = NULL;

	native_set_pud(pudp, pud);

	if (virt_addr_valid(pudp))
		mm = virt_to_page(pudp)->pt_owner_mm;
	if (mm) {
		atomic_long_inc(&mm->hydra_fn_track_set_pud_calls);
		atomic_long_inc(&mm->hydra_fn_track_set_pud_pages);
	}
}

void pgtable_track_set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	struct page *page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;
	long touched = 1;

	native_set_pmd(pmdp, pmd);

	if (!virt_addr_valid(pmdp))
		return;

	page = virt_to_page(pmdp);
	mm = page->pt_owner_mm;

	if (!READ_ONCE(page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_track_set_pmd_calls);
			atomic_long_add(touched, &mm->hydra_fn_track_set_pmd_pages);
		}
		return;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if (!pmd_present(pmd)) {
		for_each_replica(page, cur) {
			pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
			pmd_t old_repl = *replica_entry;

			if (pmd_present(old_repl) &&
			    !pmd_trans_huge(old_repl) &&
			    !pmd_leaf(old_repl) &&
			    !pmd_bad(old_repl)) {
				pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
				struct page *pte_page = virt_to_page(pte_base);
				struct mm_struct *owner_mm = pte_page->pt_owner_mm;

				native_set_pmd(replica_entry, __pmd(0));

				if (owner_mm)
					mm_dec_nr_ptes(owner_mm);
				hydra_defer_pte_page_free(owner_mm, pte_page);
			} else {
				native_set_pmd(replica_entry, __pmd(0));
			}
			touched++;
		}
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_track_set_pmd_calls);
			atomic_long_add(touched, &mm->hydra_fn_track_set_pmd_pages);
		}
		return;
	}

	if (pmd_trans_huge(pmd) || pmd_leaf(pmd)) {
		for_each_replica(page, cur) {
			pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
			pmd_t old_repl = *replica_entry;

			if (pmd_present(old_repl) &&
			    !pmd_trans_huge(old_repl) &&
			    !pmd_leaf(old_repl) &&
			    !pmd_bad(old_repl)) {
				pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
				struct page *pte_page = virt_to_page(pte_base);
				struct mm_struct *owner_mm = pte_page->pt_owner_mm;

				native_set_pmd(replica_entry, __pmd(0));

				if (owner_mm)
					mm_dec_nr_ptes(owner_mm);
				hydra_defer_pte_page_free(owner_mm, pte_page);
			} else {
				pmd_t new_repl = pmd_mkold(pmd);
				native_set_pmd(replica_entry, new_repl);
			}
			touched++;
		}
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_track_set_pmd_calls);
			atomic_long_add(touched, &mm->hydra_fn_track_set_pmd_pages);
		}
		return;
	}

	for_each_replica(page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_repl = *replica_entry;

		if (pmd_present(old_repl) &&
		    (pmd_trans_huge(old_repl) || pmd_leaf(old_repl))) {
			native_set_pmd(replica_entry, __pmd(0));
		} else if (pmd_present(old_repl) &&
			   !pmd_trans_huge(old_repl) &&
			   !pmd_leaf(old_repl) &&
			   !pmd_bad(old_repl)) {
			pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
			struct page *pte_page = virt_to_page(pte_base);
			struct mm_struct *owner_mm = pte_page->pt_owner_mm;

			native_set_pmd(replica_entry, __pmd(0));

			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);
			hydra_defer_pte_page_free(owner_mm, pte_page);
		}
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_track_set_pmd_calls);
		atomic_long_add(touched, &mm->hydra_fn_track_set_pmd_pages);
	}
}

void pgtable_repl_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *start_pte_page;
	struct page *cur;
	struct mm_struct *mm;
	long offset;
	long touched = 1;

	native_set_pte(ptep, pteval);

	if (!virt_addr_valid(ptep))
		return;

	start_pte_page = virt_to_page(ptep);
	mm = start_pte_page->pt_owner_mm;

	if (!READ_ONCE(start_pte_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_set_pte_calls);
			atomic_long_add(touched, &mm->hydra_fn_set_pte_pages);
		}
		return;
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	if (!pte_present(pteval)) {
		for_each_replica(start_pte_page, cur) {
			pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
			native_set_pte(rp, __pte(0));
			touched++;
		}
	} else {
		for_each_replica(start_pte_page, cur) {
			pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
			native_set_pte(rp, pte_mkold(pteval));
			touched++;
		}
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_set_pte_calls);
		atomic_long_add(touched, &mm->hydra_fn_set_pte_pages);
	}
}

pte_t pgtable_repl_get_pte(pte_t *ptep)
{
	struct page *pte_page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;
	pte_t master_pte;
	pteval_t extra_flags;
	long touched = 1;

	if (!ptep)
		return __pte(0);

	pte_page = virt_to_page(ptep);
	master_pte = *ptep;
	mm = pte_page->pt_owner_mm;

	if (!READ_ONCE(pte_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_get_pte_calls);
			atomic_long_add(touched, &mm->hydra_fn_get_pte_pages);
		}
		return master_pte;
	}

	if (!pte_present(master_pte)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_get_pte_calls);
			atomic_long_add(touched, &mm->hydra_fn_get_pte_pages);
		}
		return master_pte;
	}

	extra_flags = 0;
	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	for_each_replica(pte_page, cur) {
		pte_t *replica_pte = (pte_t *)(page_address(cur) + offset);
		pte_t replica_val = *replica_pte;

		if (pte_present(replica_val))
			extra_flags |= pte_flags(replica_val);
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_get_pte_calls);
		atomic_long_add(touched, &mm->hydra_fn_get_pte_pages);
	}

	return pte_set_flags(master_pte, extra_flags);
}

void pgtable_repl_set_pte_at(struct mm_struct *mm, unsigned long addr, pte_t *ptep, pte_t pteval)
{
	pgtable_repl_set_pte(ptep, pteval);
}

pte_t ptep_get_and_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	struct page *start_pte_page;
	struct page *cur;
	long offset;
	pteval_t pteval;
	pteval_t flags;
	long touched = 1;

	start_pte_page = virt_to_page(ptep);

	flags = pte_flags(*ptep);
	pteval = pte_val(native_ptep_get_and_clear(ptep));

	if (!mm || !mm->lazy_repl_enabled) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_ptep_get_and_clear_calls);
			atomic_long_add(touched, &mm->hydra_fn_ptep_get_and_clear_pages);
		}
		page_table_check_pte_clear(mm, addr, native_make_pte(pteval));
		return native_make_pte(pteval);
	}

	if (!READ_ONCE(start_pte_page->next_replica)) {
		atomic_long_inc(&mm->hydra_fn_ptep_get_and_clear_calls);
		atomic_long_add(touched, &mm->hydra_fn_ptep_get_and_clear_pages);
		page_table_check_pte_clear(mm, addr, native_make_pte(pteval));
		return native_make_pte(pteval);
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	for_each_replica(start_pte_page, cur) {
		pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
		pte_t repl_pte = native_ptep_get_and_clear(rp);

		if (pte_present(repl_pte))
			flags |= pte_flags(repl_pte);
		touched++;
	}

	atomic_long_inc(&mm->hydra_fn_ptep_get_and_clear_calls);
	atomic_long_add(touched, &mm->hydra_fn_ptep_get_and_clear_pages);

	page_table_check_pte_clear(mm, addr, native_make_pte(pteval));
	return pte_set_flags(native_make_pte(pteval), flags);
}

void ptep_set_wrprotect(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	struct page *start_pte_page;
	struct page *cur;
	long offset;
	long touched = 1;

	start_pte_page = virt_to_page(ptep);

	clear_bit(_PAGE_BIT_RW, (unsigned long *)&ptep->pte);

	if (!mm || !mm->lazy_repl_enabled) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_ptep_set_wrprotect_calls);
			atomic_long_add(touched, &mm->hydra_fn_ptep_set_wrprotect_pages);
		}
		return;
	}

	if (!READ_ONCE(start_pte_page->next_replica)) {
		atomic_long_inc(&mm->hydra_fn_ptep_set_wrprotect_calls);
		atomic_long_add(touched, &mm->hydra_fn_ptep_set_wrprotect_pages);
		return;
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	for_each_replica(start_pte_page, cur) {
		pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
		if (pte_present(*rp))
			clear_bit(_PAGE_BIT_RW, (unsigned long *)&rp->pte);
		touched++;
	}

	atomic_long_inc(&mm->hydra_fn_ptep_set_wrprotect_calls);
	atomic_long_add(touched, &mm->hydra_fn_ptep_set_wrprotect_pages);
}

pmd_t pmdp_huge_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	unsigned long offset;
	pmdval_t val;
	pmdval_t flags;
	long touched = 1;

	val = pmd_val(native_pmdp_get_and_clear(pmdp));
	flags = pmd_flags(__pmd(val));

	if (!virt_addr_valid(pmdp)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_huge_get_and_clear_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_huge_get_and_clear_pages);
		}
		page_table_check_pmd_clear(mm, addr, __pmd(val));
		return __pmd(val);
	}

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_huge_get_and_clear_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_huge_get_and_clear_pages);
		}
		page_table_check_pmd_clear(mm, addr, __pmd(val));
		return __pmd(val);
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_entry = native_pmdp_get_and_clear(replica_entry);

		if (pmd_present(old_entry) &&
		    !pmd_trans_huge(old_entry) &&
		    !pmd_leaf(old_entry) &&
		    !pmd_bad(old_entry)) {
			pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_entry);
			struct page *pte_page = virt_to_page(pte_base);
			struct mm_struct *owner_mm = pte_page->pt_owner_mm;

			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);
			hydra_defer_pte_page_free(owner_mm, pte_page);
		} else if (pmd_trans_huge(old_entry) || pmd_leaf(old_entry)) {
			flags |= pmd_flags(old_entry);
		}
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_pmdp_huge_get_and_clear_calls);
		atomic_long_add(touched, &mm->hydra_fn_pmdp_huge_get_and_clear_pages);
	}

	page_table_check_pmd_clear(mm, addr, __pmd(val));
	return pmd_set_flags(__pmd(val), flags);
}

void pmdp_set_wrprotect(struct mm_struct *mm,
			unsigned long addr, pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	unsigned long offset;
	long touched = 1;

	clear_bit(_PAGE_BIT_RW, (unsigned long *)pmdp);

	if (!virt_addr_valid(pmdp)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_set_wrprotect_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_set_wrprotect_pages);
		}
		return;
	}

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_set_wrprotect_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_set_wrprotect_pages);
		}
		return;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);

		if (pmd_present(*replica_entry) && pmd_trans_huge(*replica_entry))
			clear_bit(_PAGE_BIT_RW, (unsigned long *)replica_entry);
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_pmdp_set_wrprotect_calls);
		atomic_long_add(touched, &mm->hydra_fn_pmdp_set_wrprotect_pages);
	}
}

pmd_t hydra_pmdp_establish(pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;
	pmdval_t val;
	pmdval_t flags;
	bool propagate;
	pmd_t repl_val;
	long touched = 1;

	if (IS_ENABLED(CONFIG_SMP))
		val = pmd_val(xchg(pmdp, pmd));
	else {
		val = pmd_val(*pmdp);
		WRITE_ONCE(*pmdp, pmd);
	}

	flags = pmd_flags(__pmd(val));

	if (!virt_addr_valid(pmdp))
		return __pmd(val);

	pmd_page = virt_to_page(pmdp);
	mm = pmd_page->pt_owner_mm;

	if (!READ_ONCE(pmd_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_establish_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_establish_pages);
		}
		return __pmd(val);
	}

	propagate = !pmd_present(pmd) ||
		    pmd_trans_huge(pmd) ||
		    pmd_leaf(pmd);

	if (!propagate) {
		offset = ((unsigned long)pmdp) & ~PAGE_MASK;

		for_each_replica(pmd_page, cur) {
			pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
			pmd_t old_repl = *replica_entry;

			if (pmd_present(old_repl) &&
			    (pmd_trans_huge(old_repl) || pmd_leaf(old_repl))) {
				native_set_pmd(replica_entry, __pmd(0));
			}
			touched++;
		}

		if (mm) {
			atomic_long_inc(&mm->hydra_fn_pmdp_establish_calls);
			atomic_long_add(touched, &mm->hydra_fn_pmdp_establish_pages);
		}

		return __pmd(val);
	}

	if (pmd_val(pmd) & _PAGE_PRESENT)
		repl_val = pmd_mkold(pmd);
	else
		repl_val = __pmd(0);

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_repl;

		if (IS_ENABLED(CONFIG_SMP))
			old_repl = __pmd(pmd_val(xchg(replica_entry, repl_val)));
		else {
			old_repl = *replica_entry;
			WRITE_ONCE(*replica_entry, repl_val);
		}

		if (pmd_present(old_repl) &&
		    !pmd_trans_huge(old_repl) &&
		    !pmd_leaf(old_repl) &&
		    !pmd_bad(old_repl)) {
			pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
			struct page *pte_page = virt_to_page(pte_base);
			struct mm_struct *owner_mm = pte_page->pt_owner_mm;

			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);
			hydra_defer_pte_page_free(owner_mm, pte_page);
		} else if (pmd_trans_huge(old_repl) || pmd_leaf(old_repl)) {
			flags |= pmd_flags(old_repl);
		}
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_pmdp_establish_calls);
		atomic_long_add(touched, &mm->hydra_fn_pmdp_establish_pages);
	}

	return pmd_set_flags(__pmd(val), flags);
}

pmd_t hydra_get_pmd(pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;
	pmdval_t val;
	long touched = 1;

	if (!pmdp)
		return __pmd(0);

	if (!virt_addr_valid(pmdp))
		return *pmdp;

	pmd_page = virt_to_page(pmdp);
	mm = pmd_page->pt_owner_mm;

	if (!READ_ONCE(pmd_page->next_replica)) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_get_pmd_calls);
			atomic_long_add(touched, &mm->hydra_fn_get_pmd_pages);
		}
		return *pmdp;
	}

	val = pmd_val(*pmdp);

	if (!pmd_present(__pmd(val)) || !pmd_trans_huge(__pmd(val))) {
		if (mm) {
			atomic_long_inc(&mm->hydra_fn_get_pmd_calls);
			atomic_long_add(touched, &mm->hydra_fn_get_pmd_pages);
		}
		return __pmd(val);
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t replica_val = *replica_entry;

		if (pmd_present(replica_val) && pmd_trans_huge(replica_val))
			val |= pmd_flags(replica_val);
		touched++;
	}

	if (mm) {
		atomic_long_inc(&mm->hydra_fn_get_pmd_calls);
		atomic_long_add(touched, &mm->hydra_fn_get_pmd_pages);
	}

	return __pmd(val);
}

EXPORT_SYMBOL(hydra_get_pmd);
