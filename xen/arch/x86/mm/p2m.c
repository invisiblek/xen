/******************************************************************************
 * arch/x86/mm/p2m.c
 *
 * physical-to-machine mappings for automatically-translated domains.
 *
 * Parts of this code are Copyright (c) 2009 by Citrix Systems, Inc. (Patrick Colp)
 * Parts of this code are Copyright (c) 2007 by Advanced Micro Devices.
 * Parts of this code are Copyright (c) 2006-2007 by XenSource Inc.
 * Parts of this code are Copyright (c) 2006 by Michael A Fetterman
 * Parts based on earlier work by Michael A Fetterman, Ian Pratt et al.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/iommu.h>
#include <xen/mem_access.h>
#include <xen/vm_event.h>
#include <xen/event.h>
#include <xen/grant_table.h>
#include <xen/ioreq.h>
#include <xen/param.h>
#include <public/vm_event.h>
#include <asm/domain.h>
#include <asm/page.h>
#include <asm/paging.h>
#include <asm/p2m.h>
#include <asm/hvm/vmx/vmx.h> /* ept_p2m_init() */
#include <asm/mem_sharing.h>
#include <asm/hvm/nestedhvm.h>
#include <asm/altp2m.h>
#include <asm/vm_event.h>
#include <xsm/xsm.h>

#include "mm-locks.h"

/* Override macro from asm/page.h to make work with mfn_t */
#undef virt_to_mfn
#define virt_to_mfn(v) _mfn(__virt_to_mfn(v))

/* Turn on/off host superpage page table support for hap, default on. */
bool_t __initdata opt_hap_1gb = 1, __initdata opt_hap_2mb = 1;
boolean_param("hap_1gb", opt_hap_1gb);
boolean_param("hap_2mb", opt_hap_2mb);

DEFINE_PERCPU_RWLOCK_GLOBAL(p2m_percpu_rwlock);

static void p2m_nestedp2m_init(struct p2m_domain *p2m)
{
#ifdef CONFIG_HVM
    INIT_LIST_HEAD(&p2m->np2m_list);

    p2m->np2m_base = P2M_BASE_EADDR;
    p2m->np2m_generation = 0;
#endif
}

static int p2m_init_logdirty(struct p2m_domain *p2m)
{
    if ( p2m->logdirty_ranges )
        return 0;

    p2m->logdirty_ranges = rangeset_new(p2m->domain, "log-dirty",
                                        RANGESETF_prettyprint_hex);
    if ( !p2m->logdirty_ranges )
        return -ENOMEM;

    return 0;
}

static void p2m_free_logdirty(struct p2m_domain *p2m)
{
    if ( !p2m->logdirty_ranges )
        return;

    rangeset_destroy(p2m->logdirty_ranges);
    p2m->logdirty_ranges = NULL;
}

/* Init the datastructures for later use by the p2m code */
static int p2m_initialise(struct domain *d, struct p2m_domain *p2m)
{
    int ret = 0;

    mm_rwlock_init(&p2m->lock);
    INIT_PAGE_LIST_HEAD(&p2m->pages);

    p2m->domain = d;
    p2m->default_access = p2m_access_rwx;
    p2m->p2m_class = p2m_host;

    p2m_pod_init(p2m);
    p2m_nestedp2m_init(p2m);

    if ( hap_enabled(d) && cpu_has_vmx )
        ret = ept_p2m_init(p2m);
    else
        p2m_pt_init(p2m);

    spin_lock_init(&p2m->ioreq.lock);

    return ret;
}

static struct p2m_domain *p2m_init_one(struct domain *d)
{
    struct p2m_domain *p2m = xzalloc(struct p2m_domain);

    if ( !p2m )
        return NULL;

    if ( !zalloc_cpumask_var(&p2m->dirty_cpumask) )
        goto free_p2m;

    if ( p2m_initialise(d, p2m) )
        goto free_cpumask;
    return p2m;

free_cpumask:
    free_cpumask_var(p2m->dirty_cpumask);
free_p2m:
    xfree(p2m);
    return NULL;
}

static void p2m_free_one(struct p2m_domain *p2m)
{
    p2m_free_logdirty(p2m);
    if ( hap_enabled(p2m->domain) && cpu_has_vmx )
        ept_p2m_uninit(p2m);
    free_cpumask_var(p2m->dirty_cpumask);
    xfree(p2m);
}

static int p2m_init_hostp2m(struct domain *d)
{
    struct p2m_domain *p2m = p2m_init_one(d);
    int rc;

    if ( !p2m )
        return -ENOMEM;

    rc = p2m_init_logdirty(p2m);

    if ( !rc )
        d->arch.p2m = p2m;
    else
        p2m_free_one(p2m);

    return rc;
}

static void p2m_teardown_hostp2m(struct domain *d)
{
    /* Iterate over all p2m tables per domain */
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( p2m )
    {
        p2m_free_one(p2m);
        d->arch.p2m = NULL;
    }
}

#ifdef CONFIG_HVM
static void p2m_teardown_nestedp2m(struct domain *d)
{
    unsigned int i;
    struct p2m_domain *p2m;

    for ( i = 0; i < MAX_NESTEDP2M; i++ )
    {
        if ( !d->arch.nested_p2m[i] )
            continue;
        p2m = d->arch.nested_p2m[i];
        list_del(&p2m->np2m_list);
        p2m_free_one(p2m);
        d->arch.nested_p2m[i] = NULL;
    }
}

static int p2m_init_nestedp2m(struct domain *d)
{
    unsigned int i;
    struct p2m_domain *p2m;

    mm_lock_init(&d->arch.nested_p2m_lock);
    for ( i = 0; i < MAX_NESTEDP2M; i++ )
    {
        d->arch.nested_p2m[i] = p2m = p2m_init_one(d);
        if ( p2m == NULL )
        {
            p2m_teardown_nestedp2m(d);
            return -ENOMEM;
        }
        p2m->p2m_class = p2m_nested;
        p2m->write_p2m_entry_pre = NULL;
        p2m->write_p2m_entry_post = nestedp2m_write_p2m_entry_post;
        list_add(&p2m->np2m_list, &p2m_get_hostp2m(d)->np2m_list);
    }

    return 0;
}

static void p2m_teardown_altp2m(struct domain *d)
{
    unsigned int i;
    struct p2m_domain *p2m;

    for ( i = 0; i < MAX_ALTP2M; i++ )
    {
        if ( !d->arch.altp2m_p2m[i] )
            continue;
        p2m = d->arch.altp2m_p2m[i];
        d->arch.altp2m_p2m[i] = NULL;
        p2m_free_one(p2m);
    }
}

static int p2m_init_altp2m(struct domain *d)
{
    unsigned int i;
    struct p2m_domain *p2m;
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);

    mm_lock_init(&d->arch.altp2m_list_lock);
    for ( i = 0; i < MAX_ALTP2M; i++ )
    {
        d->arch.altp2m_p2m[i] = p2m = p2m_init_one(d);
        if ( p2m == NULL )
        {
            p2m_teardown_altp2m(d);
            return -ENOMEM;
        }
        p2m->p2m_class = p2m_alternate;
        p2m->access_required = hostp2m->access_required;
        _atomic_set(&p2m->active_vcpus, 0);
    }

    return 0;
}
#endif

int p2m_init(struct domain *d)
{
    int rc;

    rc = p2m_init_hostp2m(d);
    if ( rc )
        return rc;

#ifdef CONFIG_HVM
    /* Must initialise nestedp2m unconditionally
     * since nestedhvm_enabled(d) returns false here.
     * (p2m_init runs too early for HVM_PARAM_* options) */
    rc = p2m_init_nestedp2m(d);
    if ( rc )
    {
        p2m_teardown_hostp2m(d);
        return rc;
    }

    rc = p2m_init_altp2m(d);
    if ( rc )
    {
        p2m_teardown_hostp2m(d);
        p2m_teardown_nestedp2m(d);
    }
#endif

    return rc;
}

int p2m_is_logdirty_range(struct p2m_domain *p2m, unsigned long start,
                          unsigned long end)
{
    if ( p2m->global_logdirty ||
         rangeset_contains_range(p2m->logdirty_ranges, start, end) )
        return 1;
    if ( rangeset_overlaps_range(p2m->logdirty_ranges, start, end) )
        return -1;
    return 0;
}

#ifdef CONFIG_HVM

static void change_entry_type_global(struct p2m_domain *p2m,
                                     p2m_type_t ot, p2m_type_t nt)
{
    p2m->change_entry_type_global(p2m, ot, nt);
    /* Don't allow 'recalculate' operations to change the logdirty state. */
    if ( ot != nt )
        p2m->global_logdirty = (nt == p2m_ram_logdirty);
}

/*
 * May be called with ot = nt = p2m_ram_rw for its side effect of
 * recalculating all PTEs in the p2m.
 */
void p2m_change_entry_type_global(struct domain *d,
                                  p2m_type_t ot, p2m_type_t nt)
{
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);

    ASSERT(p2m_is_changeable(ot) && p2m_is_changeable(nt));

    p2m_lock(hostp2m);

    change_entry_type_global(hostp2m, ot, nt);

    if ( unlikely(altp2m_active(d)) )
    {
        unsigned int i;

        for ( i = 0; i < MAX_ALTP2M; i++ )
            if ( d->arch.altp2m_eptp[i] != mfn_x(INVALID_MFN) )
            {
                struct p2m_domain *altp2m = d->arch.altp2m_p2m[i];

                p2m_lock(altp2m);
                change_entry_type_global(altp2m, ot, nt);
                p2m_unlock(altp2m);
            }
    }

    p2m_unlock(hostp2m);
}

/* There's already a memory_type_changed() in asm/mtrr.h. */
static void _memory_type_changed(struct p2m_domain *p2m)
{
    if ( p2m->memory_type_changed )
        p2m->memory_type_changed(p2m);
}

void p2m_memory_type_changed(struct domain *d)
{
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);

    p2m_lock(hostp2m);

    _memory_type_changed(hostp2m);

    if ( unlikely(altp2m_active(d)) )
    {
        unsigned int i;

        for ( i = 0; i < MAX_ALTP2M; i++ )
            if ( d->arch.altp2m_eptp[i] != mfn_x(INVALID_MFN) )
            {
                struct p2m_domain *altp2m = d->arch.altp2m_p2m[i];

                p2m_lock(altp2m);
                _memory_type_changed(altp2m);
                p2m_unlock(altp2m);
            }
    }

    p2m_unlock(hostp2m);
}

int p2m_set_ioreq_server(struct domain *d,
                         unsigned int flags,
                         struct ioreq_server *s)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int rc;

    /*
     * Use lock to prevent concurrent setting attempts
     * from multiple ioreq servers.
     */
    spin_lock(&p2m->ioreq.lock);

    /* Unmap ioreq server from p2m type by passing flags with 0. */
    if ( flags == 0 )
    {
        rc = -EINVAL;
        if ( p2m->ioreq.server != s )
            goto out;

        p2m->ioreq.server = NULL;
        p2m->ioreq.flags = 0;
    }
    else
    {
        rc = -EBUSY;
        if ( p2m->ioreq.server != NULL )
            goto out;

        /*
         * It is possible that an ioreq server has just been unmapped,
         * released the spin lock, with some p2m_ioreq_server entries
         * in p2m table remained. We shall refuse another ioreq server
         * mapping request in such case.
         */
        if ( read_atomic(&p2m->ioreq.entry_count) )
            goto out;

        p2m->ioreq.server = s;
        p2m->ioreq.flags = flags;
    }

    rc = 0;

 out:
    spin_unlock(&p2m->ioreq.lock);

    return rc;
}

struct ioreq_server *p2m_get_ioreq_server(struct domain *d,
                                          unsigned int *flags)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    struct ioreq_server *s;

    spin_lock(&p2m->ioreq.lock);

    s = p2m->ioreq.server;
    *flags = p2m->ioreq.flags;

    spin_unlock(&p2m->ioreq.lock);
    return s;
}

void p2m_enable_hardware_log_dirty(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( p2m->enable_hardware_log_dirty )
        p2m->enable_hardware_log_dirty(p2m);
}

void p2m_disable_hardware_log_dirty(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( p2m->disable_hardware_log_dirty )
        p2m->disable_hardware_log_dirty(p2m);
}

void p2m_flush_hardware_cached_dirty(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( p2m->flush_hardware_cached_dirty )
    {
        p2m_lock(p2m);
        p2m->flush_hardware_cached_dirty(p2m);
        p2m_unlock(p2m);
    }
}

#endif /* CONFIG_HVM */

/*
 * Force a synchronous P2M TLB flush if a deferred flush is pending.
 *
 * Must be called with the p2m lock held.
 */
void p2m_tlb_flush_sync(struct p2m_domain *p2m)
{
    if ( p2m->need_flush ) {
        p2m->need_flush = 0;
        p2m->tlb_flush(p2m);
    }
}

/*
 * Unlock the p2m lock and do a P2M TLB flush if needed.
 */
void p2m_unlock_and_tlb_flush(struct p2m_domain *p2m)
{
    if ( p2m->need_flush ) {
        p2m->need_flush = 0;
        mm_write_unlock(&p2m->lock);
        p2m->tlb_flush(p2m);
    } else
        mm_write_unlock(&p2m->lock);
}

mfn_t __get_gfn_type_access(struct p2m_domain *p2m, unsigned long gfn_l,
                    p2m_type_t *t, p2m_access_t *a, p2m_query_t q,
                    unsigned int *page_order, bool_t locked)
{
#ifdef CONFIG_HVM
    mfn_t mfn;
    gfn_t gfn = _gfn(gfn_l);

    if ( !p2m || !paging_mode_translate(p2m->domain) )
    {
#endif
        /*
         * Not necessarily true, but for non-translated guests we claim
         * it's the most generic kind of memory.
         */
        *t = p2m_ram_rw;
        return _mfn(gfn_l);
#ifdef CONFIG_HVM
    }

    /* Unshare makes no sense without populate. */
    if ( q & P2M_UNSHARE )
        q |= P2M_ALLOC;

    if ( locked )
        /* Grab the lock here, don't release until put_gfn */
        gfn_lock(p2m, gfn, 0);

    mfn = p2m->get_entry(p2m, gfn, t, a, q, page_order, NULL);

    /* Check if we need to fork the page */
    if ( (q & P2M_ALLOC) && p2m_is_hole(*t) &&
         !mem_sharing_fork_page(p2m->domain, gfn, q & P2M_UNSHARE) )
        mfn = p2m->get_entry(p2m, gfn, t, a, q, page_order, NULL);

    /* Check if we need to unshare the page */
    if ( (q & P2M_UNSHARE) && p2m_is_shared(*t) )
    {
        ASSERT(p2m_is_hostp2m(p2m));
        /*
         * Try to unshare. If we fail, communicate ENOMEM without
         * sleeping.
         */
        if ( mem_sharing_unshare_page(p2m->domain, gfn_l) < 0 )
            mem_sharing_notify_enomem(p2m->domain, gfn_l, false);
        mfn = p2m->get_entry(p2m, gfn, t, a, q, page_order, NULL);
    }

    if (unlikely((p2m_is_broken(*t))))
    {
        /* Return invalid_mfn to avoid caller's access */
        mfn = INVALID_MFN;
        if ( q & P2M_ALLOC )
            domain_crash(p2m->domain);
    }

    return mfn;
#endif
}

void __put_gfn(struct p2m_domain *p2m, unsigned long gfn)
{
    if ( !p2m || !paging_mode_translate(p2m->domain) )
        /* Nothing to do in this case */
        return;

    ASSERT(gfn_locked_by_me(p2m, gfn));

    gfn_unlock(p2m, gfn, 0);
}

/* Atomically look up a GFN and take a reference count on the backing page. */
struct page_info *p2m_get_page_from_gfn(
    struct p2m_domain *p2m, gfn_t gfn,
    p2m_type_t *t, p2m_access_t *a, p2m_query_t q)
{
    struct page_info *page = NULL;
    p2m_access_t _a;
    p2m_type_t _t;
    mfn_t mfn;

    /* Allow t or a to be NULL */
    t = t ?: &_t;
    a = a ?: &_a;

    if ( likely(!p2m_locked_by_me(p2m)) )
    {
        /* Fast path: look up and get out */
        p2m_read_lock(p2m);
        mfn = __get_gfn_type_access(p2m, gfn_x(gfn), t, a, 0, NULL, 0);
        if ( p2m_is_any_ram(*t) && mfn_valid(mfn)
             && !((q & P2M_UNSHARE) && p2m_is_shared(*t)) )
        {
            page = mfn_to_page(mfn);
            if ( unlikely(p2m_is_foreign(*t)) )
            {
                struct domain *fdom = page_get_owner_and_reference(page);

                ASSERT(fdom != p2m->domain);
                if ( fdom == NULL )
                    page = NULL;
            }
            else
            {
                struct domain *d = !p2m_is_shared(*t) ? p2m->domain : dom_cow;

                if ( !get_page(page, d) )
                    page = NULL;
            }
        }
        p2m_read_unlock(p2m);

        if ( page )
            return page;

        /* Error path: not a suitable GFN at all */
        if ( !p2m_is_ram(*t) && !p2m_is_paging(*t) && !p2m_is_pod(*t) &&
             !mem_sharing_is_fork(p2m->domain) )
            return NULL;
    }

    /* Slow path: take the write lock and do fixups */
    mfn = get_gfn_type_access(p2m, gfn_x(gfn), t, a, q, NULL);
    if ( p2m_is_ram(*t) && mfn_valid(mfn) )
    {
        struct domain *d = !p2m_is_shared(*t) ? p2m->domain : dom_cow;

        page = mfn_to_page(mfn);
        if ( !get_page(page, d) )
            page = NULL;
    }
    put_gfn(p2m->domain, gfn_x(gfn));

    return page;
}

#ifdef CONFIG_HVM
/* Returns: 0 for success, -errno for failure */
int p2m_set_entry(struct p2m_domain *p2m, gfn_t gfn, mfn_t mfn,
                  unsigned int page_order, p2m_type_t p2mt, p2m_access_t p2ma)
{
    struct domain *d = p2m->domain;
    unsigned long todo = 1ul << page_order;
    unsigned int order;
    int set_rc, rc = 0;

    ASSERT(gfn_locked_by_me(p2m, gfn));

    while ( todo )
    {
        if ( hap_enabled(d) )
        {
            unsigned long fn_mask = !mfn_eq(mfn, INVALID_MFN) ? mfn_x(mfn) : 0;

            fn_mask |= gfn_x(gfn) | todo;

            order = (!(fn_mask & ((1ul << PAGE_ORDER_1G) - 1)) &&
                     hap_has_1gb) ? PAGE_ORDER_1G :
                    (!(fn_mask & ((1ul << PAGE_ORDER_2M) - 1)) &&
                     hap_has_2mb) ? PAGE_ORDER_2M : PAGE_ORDER_4K;
        }
        else
            order = 0;

        set_rc = p2m->set_entry(p2m, gfn, mfn, order, p2mt, p2ma, -1);
        if ( set_rc )
            rc = set_rc;

        gfn = gfn_add(gfn, 1ul << order);
        if ( !mfn_eq(mfn, INVALID_MFN) )
            mfn = mfn_add(mfn, 1ul << order);
        todo -= 1ul << order;
    }

    return rc;
}
#endif

mfn_t p2m_alloc_ptp(struct p2m_domain *p2m, unsigned int level)
{
    struct page_info *pg;

    ASSERT(p2m);
    ASSERT(p2m->domain);
    ASSERT(p2m->domain->arch.paging.alloc_page);
    pg = p2m->domain->arch.paging.alloc_page(p2m->domain);
    if ( !pg )
        return INVALID_MFN;

    page_list_add_tail(pg, &p2m->pages);
    BUILD_BUG_ON(PGT_l1_page_table * 2 != PGT_l2_page_table);
    BUILD_BUG_ON(PGT_l1_page_table * 3 != PGT_l3_page_table);
    BUILD_BUG_ON(PGT_l1_page_table * 4 != PGT_l4_page_table);
    pg->u.inuse.type_info = (PGT_l1_page_table * level) | 1 | PGT_validated;

    return page_to_mfn(pg);
}

void p2m_free_ptp(struct p2m_domain *p2m, struct page_info *pg)
{
    ASSERT(pg);
    ASSERT(p2m);
    ASSERT(p2m->domain);
    ASSERT(p2m->domain->arch.paging.free_page);

    page_list_del(pg, &p2m->pages);
    p2m->domain->arch.paging.free_page(p2m->domain, pg);

    return;
}

/*
 * Allocate a new p2m table for a domain.
 *
 * The structure of the p2m table is that of a pagetable for xen (i.e. it is
 * controlled by CONFIG_PAGING_LEVELS).
 *
 * Returns 0 for success, -errno for failure.
 */
int p2m_alloc_table(struct p2m_domain *p2m)
{
    mfn_t top_mfn;
    struct domain *d = p2m->domain;

    p2m_lock(p2m);

    if ( p2m_is_hostp2m(p2m) && domain_tot_pages(d) )
    {
        P2M_ERROR("dom %d already has memory allocated\n", d->domain_id);
        p2m_unlock(p2m);
        return -EINVAL;
    }

    if ( pagetable_get_pfn(p2m_get_pagetable(p2m)) != 0 )
    {
        P2M_ERROR("p2m already allocated for this domain\n");
        p2m_unlock(p2m);
        return -EINVAL;
    }

    P2M_PRINTK("allocating p2m table\n");

    top_mfn = p2m_alloc_ptp(p2m, 4);
    if ( mfn_eq(top_mfn, INVALID_MFN) )
    {
        p2m_unlock(p2m);
        return -ENOMEM;
    }

    p2m->phys_table = pagetable_from_mfn(top_mfn);

    p2m_unlock(p2m);
    return 0;
}

/*
 * hvm fixme: when adding support for pvh non-hardware domains, this path must
 * cleanup any foreign p2m types (release refcnts on them).
 */
void p2m_teardown(struct p2m_domain *p2m)
/* Return all the p2m pages to Xen.
 * We know we don't have any extra mappings to these pages */
{
    struct page_info *pg;
    struct domain *d;

    if (p2m == NULL)
        return;

    d = p2m->domain;

    p2m_lock(p2m);
    ASSERT(atomic_read(&d->shr_pages) == 0);
    p2m->phys_table = pagetable_null();

    while ( (pg = page_list_remove_head(&p2m->pages)) )
        d->arch.paging.free_page(d, pg);
    p2m_unlock(p2m);
}

void p2m_final_teardown(struct domain *d)
{
#ifdef CONFIG_HVM
    /*
     * We must teardown both of them unconditionally because
     * we initialise them unconditionally.
     */
    p2m_teardown_altp2m(d);
    p2m_teardown_nestedp2m(d);
#endif

    /* Iterate over all p2m tables per domain */
    p2m_teardown_hostp2m(d);
}

#ifdef CONFIG_HVM

static int __must_check
p2m_remove_page(struct p2m_domain *p2m, gfn_t gfn, mfn_t mfn,
                unsigned int page_order)
{
    unsigned long i;
    p2m_type_t t;
    p2m_access_t a;

    ASSERT(gfn_locked_by_me(p2m, gfn));
    P2M_DEBUG("removing gfn=%#lx mfn=%#lx\n", gfn_x(gfn), mfn_x(mfn));

    for ( i = 0; i < (1UL << page_order); )
    {
        unsigned int cur_order;
        mfn_t mfn_return = p2m->get_entry(p2m, gfn_add(gfn, i), &t, &a, 0,
                                          &cur_order, NULL);

        if ( p2m_is_valid(t) &&
             (!mfn_valid(mfn) || t == p2m_mmio_direct ||
              !mfn_eq(mfn_add(mfn, i), mfn_return)) )
            return -EILSEQ;

        i += (1UL << cur_order) -
             ((gfn_x(gfn) + i) & ((1UL << cur_order) - 1));
    }

    if ( mfn_valid(mfn) )
    {
        for ( i = 0; i < (1UL << page_order); i++ )
        {
            p2m->get_entry(p2m, gfn_add(gfn, i), &t, &a, 0, NULL, NULL);
            if ( !p2m_is_special(t) && !p2m_is_shared(t) )
                set_gpfn_from_mfn(mfn_x(mfn) + i, INVALID_M2P_ENTRY);
        }
    }

    ioreq_request_mapcache_invalidate(p2m->domain);

    return p2m_set_entry(p2m, gfn, INVALID_MFN, page_order, p2m_invalid,
                         p2m->default_access);
}

int
guest_physmap_remove_page(struct domain *d, gfn_t gfn,
                          mfn_t mfn, unsigned int page_order)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int rc;

    /* IOMMU for PV guests is handled in get_page_type() and put_page(). */
    if ( !paging_mode_translate(d) )
        return 0;

    gfn_lock(p2m, gfn, page_order);
    rc = p2m_remove_page(p2m, gfn, mfn, page_order);
    gfn_unlock(p2m, gfn, page_order);

    return rc;
}

#endif /* CONFIG_HVM */

int
guest_physmap_add_page(struct domain *d, gfn_t gfn, mfn_t mfn,
                       unsigned int page_order)
{
    /* IOMMU for PV guests is handled in get_page_type() and put_page(). */
    if ( !paging_mode_translate(d) )
    {
        struct page_info *page = mfn_to_page(mfn);
        unsigned long i;

        /*
         * Our interface for PV guests wrt IOMMU entries hasn't been very
         * clear; but historically, pages have started out with IOMMU mappings,
         * and only lose them when changed to a different page type.
         *
         * Retain this property by grabbing a writable type ref and then
         * dropping it immediately.  The result will be pages that have a
         * writable type (and an IOMMU entry), but a count of 0 (such that
         * any guest-requested type changes succeed and remove the IOMMU
         * entry).
         */
        for ( i = 0; i < (1UL << page_order); ++i, ++page )
        {
            if ( !need_iommu_pt_sync(d) )
                /* nothing */;
            else if ( get_page_and_type(page, d, PGT_writable_page) )
                put_page_and_type(page);
            else
                return -EINVAL;

            set_gpfn_from_mfn(mfn_x(mfn) + i, gfn_x(gfn) + i);
        }

        return 0;
    }

    return guest_physmap_add_entry(d, gfn, mfn, page_order, p2m_ram_rw);
}

#ifdef CONFIG_HVM

int
guest_physmap_add_entry(struct domain *d, gfn_t gfn, mfn_t mfn,
                        unsigned int page_order, p2m_type_t t)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    unsigned long i;
    gfn_t ogfn;
    p2m_type_t ot;
    p2m_access_t a;
    mfn_t omfn;
    int pod_count = 0;
    int rc = 0;

    if ( !paging_mode_translate(d) )
    {
        ASSERT_UNREACHABLE();
        return -EPERM;
    }

    /* foreign pages are added thru p2m_add_foreign */
    if ( p2m_is_foreign(t) )
        return -EINVAL;

    if ( !mfn_valid(mfn) || t == p2m_mmio_direct )
    {
        ASSERT_UNREACHABLE();
        return -EINVAL;
    }

    p2m_lock(p2m);

    P2M_DEBUG("adding gfn=%#lx mfn=%#lx\n", gfn_x(gfn), mfn_x(mfn));

    /* First, remove m->p mappings for existing p->m mappings */
    for ( i = 0; i < (1UL << page_order); i++ )
    {
        omfn = p2m->get_entry(p2m, gfn_add(gfn, i), &ot,
                              &a, 0, NULL, NULL);
        if ( p2m_is_shared(ot) )
        {
            /* Do an unshare to cleanly take care of all corner cases. */
            rc = mem_sharing_unshare_page(d, gfn_x(gfn) + i);
            if ( rc )
            {
                p2m_unlock(p2m);
                /*
                 * NOTE: Should a guest domain bring this upon itself,
                 * there is not a whole lot we can do. We are buried
                 * deep in locks from most code paths by now. So, fail
                 * the call and don't try to sleep on a wait queue
                 * while placing the mem event.
                 *
                 * However, all current (changeset 3432abcf9380) code
                 * paths avoid this unsavoury situation. For now.
                 *
                 * Foreign domains are okay to place an event as they
                 * won't go to sleep.
                 */
                mem_sharing_notify_enomem(d, gfn_x(gfn) + i, false);
                return rc;
            }
            omfn = p2m->get_entry(p2m, gfn_add(gfn, i),
                                  &ot, &a, 0, NULL, NULL);
            ASSERT(!p2m_is_shared(ot));
        }
        if ( p2m_is_special(ot) )
        {
            /* Don't permit unmapping grant/foreign/direct-MMIO this way. */
            p2m_unlock(p2m);
            printk(XENLOG_G_ERR
                   "%pd: GFN %#lx (%#lx,%u,%u) -> (%#lx,%u,%u) not permitted\n",
                   d, gfn_x(gfn) + i,
                   mfn_x(omfn), ot, a,
                   mfn_x(mfn) + i, t, p2m->default_access);
            domain_crash(d);
            return -EPERM;
        }
        else if ( p2m_is_ram(ot) && !p2m_is_paged(ot) )
        {
            ASSERT(mfn_valid(omfn));
            set_gpfn_from_mfn(mfn_x(omfn), INVALID_M2P_ENTRY);
        }
        else if ( ot == p2m_populate_on_demand )
        {
            /* Count how man PoD entries we'll be replacing if successful */
            pod_count++;
        }
        else if ( p2m_is_paging(ot) && (ot != p2m_ram_paging_out) )
        {
            /* We're plugging a hole in the physmap where a paged out page was */
            atomic_dec(&d->paged_pages);
        }
    }

    /* Then, look for m->p mappings for this range and deal with them */
    for ( i = 0; i < (1UL << page_order); i++ )
    {
        if ( dom_cow &&
             page_get_owner(mfn_to_page(mfn_add(mfn, i))) == dom_cow )
        {
            /* This is no way to add a shared page to your physmap! */
            gdprintk(XENLOG_ERR, "Adding shared mfn %lx directly to dom%d physmap not allowed.\n",
                     mfn_x(mfn_add(mfn, i)), d->domain_id);
            p2m_unlock(p2m);
            return -EINVAL;
        }
        if ( page_get_owner(mfn_to_page(mfn_add(mfn, i))) != d )
            continue;
        ogfn = mfn_to_gfn(d, mfn_add(mfn, i));
        if ( !gfn_eq(ogfn, _gfn(INVALID_M2P_ENTRY)) &&
             !gfn_eq(ogfn, gfn_add(gfn, i)) )
        {
            /* This machine frame is already mapped at another physical
             * address */
            P2M_DEBUG("aliased! mfn=%#lx, old gfn=%#lx, new gfn=%#lx\n",
                      mfn_x(mfn_add(mfn, i)), gfn_x(ogfn),
                      gfn_x(gfn_add(gfn, i)));
            omfn = p2m->get_entry(p2m, ogfn, &ot, &a, 0, NULL, NULL);
            if ( p2m_is_ram(ot) && !p2m_is_paged(ot) )
            {
                ASSERT(mfn_valid(omfn));
                P2M_DEBUG("old gfn=%#lx -> mfn %#lx\n",
                          gfn_x(ogfn) , mfn_x(omfn));
                if ( mfn_eq(omfn, mfn_add(mfn, i)) &&
                     (rc = p2m_remove_page(p2m, ogfn, omfn, 0)) )
                    goto out;
            }
        }
    }

    /* Now, actually do the two-way mapping */
    rc = p2m_set_entry(p2m, gfn, mfn, page_order, t, p2m->default_access);
    if ( rc == 0 )
    {
        pod_lock(p2m);
        p2m->pod.entry_count -= pod_count;
        BUG_ON(p2m->pod.entry_count < 0);
        pod_unlock(p2m);

        if ( !p2m_is_grant(t) )
        {
            for ( i = 0; i < (1UL << page_order); i++ )
                set_gpfn_from_mfn(mfn_x(mfn_add(mfn, i)),
                                  gfn_x(gfn_add(gfn, i)));
        }
    }

out:
    p2m_unlock(p2m);

    return rc;
}

/*
 * Modify the p2m type of a single gfn from ot to nt.
 * Returns: 0 for success, -errno for failure.
 * Resets the access permissions.
 */
int p2m_change_type_one(struct domain *d, unsigned long gfn_l,
                       p2m_type_t ot, p2m_type_t nt)
{
    p2m_access_t a;
    p2m_type_t pt;
    gfn_t gfn = _gfn(gfn_l);
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int rc;

    BUG_ON(p2m_is_special(ot) || p2m_is_special(nt));

    gfn_lock(p2m, gfn, 0);

    mfn = p2m->get_entry(p2m, gfn, &pt, &a, 0, NULL, NULL);
    rc = likely(pt == ot)
         ? p2m_set_entry(p2m, gfn, mfn, PAGE_ORDER_4K, nt,
                         p2m->default_access)
         : -EBUSY;

    gfn_unlock(p2m, gfn, 0);

    return rc;
}

/* Modify the p2m type of [start, end_exclusive) from ot to nt. */
static void change_type_range(struct p2m_domain *p2m,
                              unsigned long start, unsigned long end_exclusive,
                              p2m_type_t ot, p2m_type_t nt)
{
    unsigned long invalidate_start, invalidate_end;
    struct domain *d = p2m->domain;
    const unsigned long host_max_pfn = p2m_get_hostp2m(d)->max_mapped_pfn;
    unsigned long end = end_exclusive - 1;
    const unsigned long max_pfn = p2m->max_mapped_pfn;
    int rc = 0;

    /*
     * If we have an altp2m, the logdirty rangeset range needs to
     * match that of the hostp2m, but for efficiency, we want to clip
     * down the the invalidation range according to the mapped values
     * in the altp2m. Keep track of and clip the ranges separately.
     */
    invalidate_start = start;
    invalidate_end   = end;

    /*
     * Clip down to the host p2m. This is probably not the right behavior.
     * This should be revisited later, but for now post a warning.
     */
    if ( unlikely(end > host_max_pfn) )
    {
        printk(XENLOG_G_WARNING "Dom%d logdirty rangeset clipped to max_mapped_pfn\n",
               d->domain_id);
        end = invalidate_end = host_max_pfn;
    }

    /* If the requested range is out of scope, return doing nothing. */
    if ( start > end )
        return;

    if ( p2m_is_altp2m(p2m) )
        invalidate_end = min(invalidate_end, max_pfn);

    /*
     * If the p2m is empty, or the range is outside the currently
     * mapped range, no need to do the invalidation; just update the
     * rangeset.
     */
    if ( invalidate_start < invalidate_end )
    {
        /*
         * If all valid gfns are in the invalidation range, just do a
         * global type change. Otherwise, invalidate only the range
         * we need.
         *
         * NB that invalidate_end can't logically be >max_pfn at this
         * point. If this changes, the == will need to be changed to
         * >=.
         */
        ASSERT(invalidate_end <= max_pfn);
        if ( !invalidate_start && invalidate_end == max_pfn)
            p2m->change_entry_type_global(p2m, ot, nt);
        else
            rc = p2m->change_entry_type_range(p2m, ot, nt,
                                              invalidate_start, invalidate_end);
        if ( rc )
        {
            printk(XENLOG_G_ERR "Error %d changing Dom%d GFNs [%lx,%lx] from %d to %d\n",
                   rc, d->domain_id, invalidate_start, invalidate_end, ot, nt);
            domain_crash(d);
        }
    }

    switch ( nt )
    {
    case p2m_ram_rw:
        if ( ot == p2m_ram_logdirty )
            rc = rangeset_remove_range(p2m->logdirty_ranges, start, end);
        break;
    case p2m_ram_logdirty:
        if ( ot == p2m_ram_rw )
            rc = rangeset_add_range(p2m->logdirty_ranges, start, end);
        break;
    default:
        break;
    }
    if ( rc )
    {
        printk(XENLOG_G_ERR "Error %d manipulating Dom%d's log-dirty ranges\n",
               rc, d->domain_id);
        domain_crash(d);
    }
}

void p2m_change_type_range(struct domain *d,
                           unsigned long start, unsigned long end,
                           p2m_type_t ot, p2m_type_t nt)
{
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);

    ASSERT(ot != nt);
    ASSERT(p2m_is_changeable(ot) && p2m_is_changeable(nt));

    p2m_lock(hostp2m);
    hostp2m->defer_nested_flush = true;

    change_type_range(hostp2m, start, end, ot, nt);

    if ( unlikely(altp2m_active(d)) )
    {
        unsigned int i;

        for ( i = 0; i < MAX_ALTP2M; i++ )
            if ( d->arch.altp2m_eptp[i] != mfn_x(INVALID_MFN) )
            {
                struct p2m_domain *altp2m = d->arch.altp2m_p2m[i];

                p2m_lock(altp2m);
                change_type_range(altp2m, start, end, ot, nt);
                p2m_unlock(altp2m);
            }
    }
    hostp2m->defer_nested_flush = false;
    if ( nestedhvm_enabled(d) )
        p2m_flush_nestedp2m(d);

    p2m_unlock(hostp2m);
}

/*
 * Finish p2m type change for gfns which are marked as need_recalc in a range.
 * Uses the current p2m's max_mapped_pfn to further clip the invalidation
 * range for alternate p2ms.
 * Returns: 0 for success, negative for failure
 */
static int finish_type_change(struct p2m_domain *p2m,
                              gfn_t first_gfn, unsigned long max_nr)
{
    unsigned long gfn = gfn_x(first_gfn);
    unsigned long last_gfn = gfn + max_nr - 1;
    int rc = 0;

    last_gfn = min(last_gfn, p2m->max_mapped_pfn);
    while ( gfn <= last_gfn )
    {
        rc = p2m->recalc(p2m, gfn);
        /*
         * ept->recalc could return 0/1/-ENOMEM. pt->recalc could return
         * 0/1/-ENOMEM/-ENOENT, -ENOENT isn't an error as we are looping
         * gfn here. If rc is 1 we need to have it 0 for success.
         */
        if ( rc == -ENOENT || rc > 0 )
            rc = 0;
        else if ( rc < 0 )
        {
            gdprintk(XENLOG_ERR, "p2m->recalc failed! Dom%d gfn=%lx\n",
                     p2m->domain->domain_id, gfn);
            break;
        }

        gfn++;
    }

    return rc;
}

int p2m_finish_type_change(struct domain *d,
                           gfn_t first_gfn, unsigned long max_nr)
{
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);
    int rc;

    p2m_lock(hostp2m);

    rc = finish_type_change(hostp2m, first_gfn, max_nr);

    if ( rc < 0 )
        goto out;

    if ( unlikely(altp2m_active(d)) )
    {
        unsigned int i;

        for ( i = 0; i < MAX_ALTP2M; i++ )
            if ( d->arch.altp2m_eptp[i] != mfn_x(INVALID_MFN) )
            {
                struct p2m_domain *altp2m = d->arch.altp2m_p2m[i];

                p2m_lock(altp2m);
                rc = finish_type_change(altp2m, first_gfn, max_nr);
                p2m_unlock(altp2m);

                if ( rc < 0 )
                    goto out;
            }
    }

 out:
    p2m_unlock(hostp2m);

    return rc;
}

/*
 * Returns:
 *    0              for success
 *    -errno         for failure
 *    1 + new order  for caller to retry with smaller order (guaranteed
 *                   to be smaller than order passed in)
 */
static int set_typed_p2m_entry(struct domain *d, unsigned long gfn_l,
                               mfn_t mfn, unsigned int order,
                               p2m_type_t gfn_p2mt, p2m_access_t access)
{
    int rc = 0;
    p2m_access_t a;
    p2m_type_t ot;
    mfn_t omfn;
    gfn_t gfn = _gfn(gfn_l);
    unsigned int cur_order = 0;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( !paging_mode_translate(d) )
    {
        ASSERT_UNREACHABLE();
        return -EIO;
    }

    gfn_lock(p2m, gfn, order);
    omfn = p2m->get_entry(p2m, gfn, &ot, &a, 0, &cur_order, NULL);
    if ( cur_order < order )
    {
        gfn_unlock(p2m, gfn, order);
        return cur_order + 1;
    }
    if ( p2m_is_special(ot) )
    {
        /* Special-case (almost) identical mappings. */
        if ( !mfn_eq(mfn, omfn) || gfn_p2mt != ot )
        {
            gfn_unlock(p2m, gfn, order);
            printk(XENLOG_G_ERR
                   "%pd: GFN %#lx (%#lx,%u,%u,%u) -> (%#lx,%u,%u,%u) not permitted\n",
                   d, gfn_l,
                   mfn_x(omfn), cur_order, ot, a,
                   mfn_x(mfn), order, gfn_p2mt, access);
            domain_crash(d);
            return -EPERM;
        }

        if ( access == a )
        {
            gfn_unlock(p2m, gfn, order);
            return 0;
        }
    }
    else if ( p2m_is_ram(ot) )
    {
        unsigned long i;

        for ( i = 0; i < (1UL << order); ++i )
        {
            ASSERT(mfn_valid(mfn_add(omfn, i)));
            set_gpfn_from_mfn(mfn_x(omfn) + i, INVALID_M2P_ENTRY);
        }

        ioreq_request_mapcache_invalidate(d);
    }

    P2M_DEBUG("set %d %lx %lx\n", gfn_p2mt, gfn_l, mfn_x(mfn));
    rc = p2m_set_entry(p2m, gfn, mfn, order, gfn_p2mt, access);
    if ( rc )
        gdprintk(XENLOG_ERR, "p2m_set_entry: %#lx:%u -> %d (0x%"PRI_mfn")\n",
                 gfn_l, order, rc, mfn_x(mfn));
    else if ( p2m_is_pod(ot) )
    {
        pod_lock(p2m);
        p2m->pod.entry_count -= 1UL << order;
        BUG_ON(p2m->pod.entry_count < 0);
        pod_unlock(p2m);
    }
    gfn_unlock(p2m, gfn, order);

    return rc;
}

/* Set foreign mfn in the given guest's p2m table. */
int set_foreign_p2m_entry(struct domain *d, const struct domain *fd,
                          unsigned long gfn, mfn_t mfn)
{
    ASSERT(arch_acquire_resource_check(d));

    return set_typed_p2m_entry(d, gfn, mfn, PAGE_ORDER_4K, p2m_map_foreign,
                               p2m_get_hostp2m(d)->default_access);
}

int set_mmio_p2m_entry(struct domain *d, gfn_t gfn, mfn_t mfn,
                       unsigned int order)
{
    if ( order > PAGE_ORDER_4K &&
         rangeset_overlaps_range(mmio_ro_ranges, mfn_x(mfn),
                                 mfn_x(mfn) + (1UL << order) - 1) )
        return PAGE_ORDER_4K + 1;

    return set_typed_p2m_entry(d, gfn_x(gfn), mfn, order, p2m_mmio_direct,
                               p2m_get_hostp2m(d)->default_access);
}

/*
 * Returns:
 *    0        for success
 *    -errno   for failure
 *    order+1  for caller to retry with order (guaranteed smaller than
 *             the order value passed in)
 */
static int clear_mmio_p2m_entry(struct domain *d, unsigned long gfn_l,
                                mfn_t mfn, unsigned int order)
{
    int rc = -EINVAL;
    gfn_t gfn = _gfn(gfn_l);
    mfn_t actual_mfn;
    p2m_access_t a;
    p2m_type_t t;
    unsigned int cur_order = 0;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( !paging_mode_translate(d) )
        return -EIO;

    gfn_lock(p2m, gfn, order);
    actual_mfn = p2m->get_entry(p2m, gfn, &t, &a, 0, &cur_order, NULL);
    if ( cur_order < order )
    {
        rc = cur_order + 1;
        goto out;
    }

    /* Do not use mfn_valid() here as it will usually fail for MMIO pages. */
    if ( mfn_eq(actual_mfn, INVALID_MFN) || (t != p2m_mmio_direct) )
    {
        gdprintk(XENLOG_ERR,
                 "gfn_to_mfn failed! gfn=%08lx type:%d\n", gfn_l, t);
        goto out;
    }
    if ( !mfn_eq(mfn, actual_mfn) )
        gdprintk(XENLOG_WARNING,
                 "no mapping between mfn %08lx and gfn %08lx\n",
                 mfn_x(mfn), gfn_l);
    rc = p2m_set_entry(p2m, gfn, INVALID_MFN, order, p2m_invalid,
                       p2m->default_access);

 out:
    gfn_unlock(p2m, gfn, order);

    return rc;
}

#endif /* CONFIG_HVM */

int set_identity_p2m_entry(struct domain *d, unsigned long gfn_l,
                           p2m_access_t p2ma, unsigned int flag)
{
#ifdef CONFIG_HVM
    p2m_type_t p2mt;
    p2m_access_t a;
    gfn_t gfn = _gfn(gfn_l);
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int ret;

    if ( !paging_mode_translate(d) )
    {
#endif
        if ( !is_iommu_enabled(d) )
            return 0;
        return iommu_legacy_map(d, _dfn(gfn_l), _mfn(gfn_l),
                                1ul << PAGE_ORDER_4K,
                                p2m_access_to_iommu_flags(p2ma));
#ifdef CONFIG_HVM
    }

    gfn_lock(p2m, gfn, 0);

    mfn = p2m->get_entry(p2m, gfn, &p2mt, &a, 0, NULL, NULL);

    if ( p2mt == p2m_invalid || p2mt == p2m_mmio_dm )
        ret = p2m_set_entry(p2m, gfn, _mfn(gfn_l), PAGE_ORDER_4K,
                            p2m_mmio_direct, p2ma);
    else if ( mfn_x(mfn) == gfn_l && p2mt == p2m_mmio_direct && a == p2ma )
        ret = 0;
    else
    {
        if ( flag & XEN_DOMCTL_DEV_RDM_RELAXED )
            ret = 0;
        else
            ret = -EBUSY;
        printk(XENLOG_G_WARNING
               "Cannot setup identity map d%d:%lx,"
               " gfn already mapped to %lx.\n",
               d->domain_id, gfn_l, mfn_x(mfn));
    }

    gfn_unlock(p2m, gfn, 0);
    return ret;
#endif
}

int clear_identity_p2m_entry(struct domain *d, unsigned long gfn_l)
{
#ifdef CONFIG_HVM
    p2m_type_t p2mt;
    p2m_access_t a;
    gfn_t gfn = _gfn(gfn_l);
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int ret;

    if ( !paging_mode_translate(d) )
    {
#endif
        if ( !is_iommu_enabled(d) )
            return 0;
        return iommu_legacy_unmap(d, _dfn(gfn_l), 1ul << PAGE_ORDER_4K);
#ifdef CONFIG_HVM
    }

    gfn_lock(p2m, gfn, 0);

    mfn = p2m->get_entry(p2m, gfn, &p2mt, &a, 0, NULL, NULL);
    if ( p2mt == p2m_mmio_direct && mfn_x(mfn) == gfn_l )
    {
        ret = p2m_set_entry(p2m, gfn, INVALID_MFN, PAGE_ORDER_4K,
                            p2m_invalid, p2m->default_access);
        gfn_unlock(p2m, gfn, 0);
    }
    else
    {
        gfn_unlock(p2m, gfn, 0);
        printk(XENLOG_G_WARNING
               "non-identity map d%d:%lx not cleared (mapped to %lx)\n",
               d->domain_id, gfn_l, mfn_x(mfn));
        ret = 0;
    }

    return ret;
#endif
}

#ifdef CONFIG_MEM_SHARING

/* Returns: 0 for success, -errno for failure */
int set_shared_p2m_entry(struct domain *d, unsigned long gfn_l, mfn_t mfn)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int rc = 0;
    gfn_t gfn = _gfn(gfn_l);
    p2m_access_t a;
    p2m_type_t ot;
    mfn_t omfn;
    unsigned long pg_type;

    if ( !paging_mode_translate(p2m->domain) )
        return -EIO;

    gfn_lock(p2m, gfn, 0);
    omfn = p2m->get_entry(p2m, gfn, &ot, &a, 0, NULL, NULL);
    /* At the moment we only allow p2m change if gfn has already been made
     * sharable first */
    ASSERT(p2m_is_shared(ot));
    ASSERT(mfn_valid(omfn));
    /* Set the m2p entry to invalid only if there are no further type
     * refs to this page as shared */
    pg_type = read_atomic(&(mfn_to_page(omfn)->u.inuse.type_info));
    if ( (pg_type & PGT_count_mask) == 0
         || (pg_type & PGT_type_mask) != PGT_shared_page )
        set_gpfn_from_mfn(mfn_x(omfn), INVALID_M2P_ENTRY);

    P2M_DEBUG("set shared %lx %lx\n", gfn_l, mfn_x(mfn));
    rc = p2m_set_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2m_ram_shared,
                       p2m->default_access);
    gfn_unlock(p2m, gfn, 0);
    if ( rc )
        gdprintk(XENLOG_ERR,
                 "p2m_set_entry failed! mfn=%08lx rc:%d\n",
                 mfn_x(get_gfn_query_unlocked(p2m->domain, gfn_l, &ot)), rc);
    return rc;
}

#endif /* CONFIG_MEM_SHARING */

#ifdef CONFIG_HVM

static struct p2m_domain *
p2m_getlru_nestedp2m(struct domain *d, struct p2m_domain *p2m)
{
    struct list_head *lru_list = &p2m_get_hostp2m(d)->np2m_list;
    
    ASSERT(!list_empty(lru_list));

    if ( p2m == NULL )
        p2m = list_entry(lru_list->prev, struct p2m_domain, np2m_list);

    list_move(&p2m->np2m_list, lru_list);

    return p2m;
}

static void
p2m_flush_table_locked(struct p2m_domain *p2m)
{
    struct page_info *top, *pg;
    struct domain *d = p2m->domain;
    mfn_t mfn;

    ASSERT(p2m_locked_by_me(p2m));

    /*
     * "Host" p2m tables can have shared entries &c that need a bit more care
     * when discarding them.
     */
    ASSERT(!p2m_is_hostp2m(p2m));
#ifdef CONFIG_HVM
    /* Nested p2m's do not do pod, hence the asserts (and no pod lock)*/
    ASSERT(page_list_empty(&p2m->pod.super));
    ASSERT(page_list_empty(&p2m->pod.single));
#endif

    /* No need to flush if it's already empty */
    if ( p2m_is_nestedp2m(p2m) && p2m->np2m_base == P2M_BASE_EADDR )
        return;

    /* This is no longer a valid nested p2m for any address space */
    p2m->np2m_base = P2M_BASE_EADDR;
    p2m->np2m_generation++;

    /* Make sure nobody else is using this p2m table */
    if ( nestedhvm_enabled(d) )
        nestedhvm_vmcx_flushtlb(p2m);

    /* Zap the top level of the trie */
    mfn = pagetable_get_mfn(p2m_get_pagetable(p2m));
    clear_domain_page(mfn);

    /* Free the rest of the trie pages back to the paging pool */
    top = mfn_to_page(mfn);
    while ( (pg = page_list_remove_head(&p2m->pages)) )
    {
        if ( pg != top )
            d->arch.paging.free_page(d, pg);
    }
    page_list_add(top, &p2m->pages);
}

/* Reset this p2m table to be empty */
static void
p2m_flush_table(struct p2m_domain *p2m)
{
    p2m_lock(p2m);
    p2m_flush_table_locked(p2m);
    p2m_unlock(p2m);
}

void
p2m_flush(struct vcpu *v, struct p2m_domain *p2m)
{
    ASSERT(v->domain == p2m->domain);
    vcpu_nestedhvm(v).nv_p2m = NULL;
    p2m_flush_table(p2m);
    hvm_asid_flush_vcpu(v);
}

void
p2m_flush_nestedp2m(struct domain *d)
{
    unsigned int i;

    for ( i = 0; i < MAX_NESTEDP2M; i++ )
    {
        struct p2m_domain *p2m = d->arch.nested_p2m[i];

        if ( p2m_locked_by_me(p2m) )
            p2m_flush_table_locked(p2m);
        else
            p2m_flush_table(p2m);
    }
}

void np2m_flush_base(struct vcpu *v, unsigned long np2m_base)
{
    struct domain *d = v->domain;
    struct p2m_domain *p2m;
    unsigned int i;

    np2m_base &= ~(0xfffull);

    nestedp2m_lock(d);
    for ( i = 0; i < MAX_NESTEDP2M; i++ )
    {
        p2m = d->arch.nested_p2m[i];
        p2m_lock(p2m);
        if ( p2m->np2m_base == np2m_base )
        {
            p2m_flush_table_locked(p2m);
            p2m_unlock(p2m);
            break;
        }
        p2m_unlock(p2m);
    }
    nestedp2m_unlock(d);
}

static void assign_np2m(struct vcpu *v, struct p2m_domain *p2m)
{
    struct nestedvcpu *nv = &vcpu_nestedhvm(v);
    struct domain *d = v->domain;

    /* Bring this np2m to the top of the LRU list */
    p2m_getlru_nestedp2m(d, p2m);

    nv->nv_flushp2m = 0;
    nv->nv_p2m = p2m;
    nv->np2m_generation = p2m->np2m_generation;
    cpumask_set_cpu(v->processor, p2m->dirty_cpumask);
}

static void nvcpu_flush(struct vcpu *v)
{
    hvm_asid_flush_vcpu(v);
    vcpu_nestedhvm(v).stale_np2m = true;
}

struct p2m_domain *
p2m_get_nestedp2m_locked(struct vcpu *v)
{
    struct nestedvcpu *nv = &vcpu_nestedhvm(v);
    struct domain *d = v->domain;
    struct p2m_domain *p2m;
    uint64_t np2m_base = nhvm_vcpu_p2m_base(v);
    unsigned int i;
    bool needs_flush = true;

    /* Mask out low bits; this avoids collisions with P2M_BASE_EADDR */
    np2m_base &= ~(0xfffull);

    if (nv->nv_flushp2m && nv->nv_p2m) {
        nv->nv_p2m = NULL;
    }

    nestedp2m_lock(d);
    p2m = nv->nv_p2m;
    if ( p2m ) 
    {
        p2m_lock(p2m);
        if ( p2m->np2m_base == np2m_base )
        {
            /* Check if np2m was flushed just before the lock */
            if ( nv->np2m_generation == p2m->np2m_generation )
                needs_flush = false;
            /* np2m is up-to-date */
            goto found;
        }
        else if ( p2m->np2m_base != P2M_BASE_EADDR )
        {
            /* vCPU is switching from some other valid np2m */
            cpumask_clear_cpu(v->processor, p2m->dirty_cpumask);
        }
        p2m_unlock(p2m);
    }

    /* Share a np2m if possible */
    for ( i = 0; i < MAX_NESTEDP2M; i++ )
    {
        p2m = d->arch.nested_p2m[i];
        p2m_lock(p2m);

        if ( p2m->np2m_base == np2m_base )
            goto found;

        p2m_unlock(p2m);
    }

    /* All p2m's are or were in use. Take the least recent used one,
     * flush it and reuse. */
    p2m = p2m_getlru_nestedp2m(d, NULL);
    p2m_flush_table(p2m);
    p2m_lock(p2m);

 found:
    if ( needs_flush )
        nvcpu_flush(v);
    p2m->np2m_base = np2m_base;
    assign_np2m(v, p2m);
    nestedp2m_unlock(d);

    return p2m;
}

struct p2m_domain *p2m_get_nestedp2m(struct vcpu *v)
{
    struct p2m_domain *p2m = p2m_get_nestedp2m_locked(v);
    p2m_unlock(p2m);

    return p2m;
}

struct p2m_domain *
p2m_get_p2m(struct vcpu *v)
{
    if (!nestedhvm_is_n2(v))
        return p2m_get_hostp2m(v->domain);

    return p2m_get_nestedp2m(v);
}

void np2m_schedule(int dir)
{
    struct vcpu *curr = current;
    struct nestedvcpu *nv = &vcpu_nestedhvm(curr);
    struct p2m_domain *p2m;

    ASSERT(dir == NP2M_SCHEDLE_IN || dir == NP2M_SCHEDLE_OUT);

    if ( !nestedhvm_enabled(curr->domain) ||
         !nestedhvm_vcpu_in_guestmode(curr) ||
         !nestedhvm_paging_mode_hap(curr) )
        return;

    p2m = nv->nv_p2m;
    if ( p2m )
    {
        bool np2m_valid;

        p2m_lock(p2m);
        np2m_valid = p2m->np2m_base == nhvm_vcpu_p2m_base(curr) &&
                     nv->np2m_generation == p2m->np2m_generation;
        if ( dir == NP2M_SCHEDLE_OUT && np2m_valid )
        {
            /*
             * The np2m is up to date but this vCPU will no longer use it,
             * which means there are no reasons to send a flush IPI.
             */
            cpumask_clear_cpu(curr->processor, p2m->dirty_cpumask);
        }
        else if ( dir == NP2M_SCHEDLE_IN )
        {
            if ( !np2m_valid )
            {
                /* This vCPU's np2m was flushed while it was not runnable */
                hvm_asid_flush_core();
                vcpu_nestedhvm(curr).nv_p2m = NULL;
            }
            else
                cpumask_set_cpu(curr->processor, p2m->dirty_cpumask);
        }
        p2m_unlock(p2m);
    }
}

unsigned long paging_gva_to_gfn(struct vcpu *v,
                                unsigned long va,
                                uint32_t *pfec)
{
    struct p2m_domain *hostp2m = p2m_get_hostp2m(v->domain);
    const struct paging_mode *hostmode = paging_get_hostmode(v);

    if ( is_hvm_vcpu(v) && paging_mode_hap(v->domain) && nestedhvm_is_n2(v) )
    {
        unsigned long l2_gfn, l1_gfn;
        paddr_t l1_gpa;
        struct p2m_domain *p2m;
        const struct paging_mode *mode;
        uint8_t l1_p2ma;
        unsigned int l1_page_order;
        int rv;

        /* translate l2 guest va into l2 guest gfn */
        p2m = p2m_get_nestedp2m(v);
        mode = paging_get_nestedmode(v);
        l2_gfn = mode->gva_to_gfn(v, p2m, va, pfec);

        if ( l2_gfn == gfn_x(INVALID_GFN) )
            return gfn_x(INVALID_GFN);

        rv = nestedhap_walk_L1_p2m(v, pfn_to_paddr(l2_gfn), &l1_gpa,
                                   &l1_page_order, &l1_p2ma,
                                   1,
                                   !!(*pfec & PFEC_write_access),
                                   !!(*pfec & PFEC_insn_fetch));

        if ( rv != NESTEDHVM_PAGEFAULT_DONE )
            return gfn_x(INVALID_GFN);

        l1_gfn = paddr_to_pfn(l1_gpa);

        /*
         * Sanity check that l1_gfn can be used properly as a 4K mapping, even
         * if it mapped by a nested superpage.
         */
        ASSERT((l2_gfn & ((1ul << l1_page_order) - 1)) ==
               (l1_gfn & ((1ul << l1_page_order) - 1)));

        return l1_gfn;
    }

    return hostmode->gva_to_gfn(v, hostp2m, va, pfec);
}

#endif /* CONFIG_HVM */

/*
 * If the map is non-NULL, we leave this function having acquired an extra ref
 * on mfn_to_page(*mfn).  In all cases, *pfec contains appropriate
 * synthetic/structure PFEC_* bits.
 */
void *map_domain_gfn(struct p2m_domain *p2m, gfn_t gfn, mfn_t *mfn,
                     p2m_query_t q, uint32_t *pfec)
{
    p2m_type_t p2mt;
    struct page_info *page;

    if ( !gfn_valid(p2m->domain, gfn) )
    {
        *pfec = PFEC_reserved_bit | PFEC_page_present;
        return NULL;
    }

    /* Translate the gfn, unsharing if shared. */
    page = p2m_get_page_from_gfn(p2m, gfn, &p2mt, NULL, q);
    if ( p2m_is_paging(p2mt) )
    {
        ASSERT(p2m_is_hostp2m(p2m));
        if ( page )
            put_page(page);
        p2m_mem_paging_populate(p2m->domain, gfn);
        *pfec = PFEC_page_paged;
        return NULL;
    }
    if ( p2m_is_shared(p2mt) )
    {
        if ( page )
            put_page(page);
        *pfec = PFEC_page_shared;
        return NULL;
    }
    if ( !page )
    {
        *pfec = 0;
        return NULL;
    }

    *pfec = PFEC_page_present;
    *mfn = page_to_mfn(page);
    ASSERT(mfn_valid(*mfn));

    return map_domain_page(*mfn);
}

#ifdef CONFIG_HVM

static unsigned int mmio_order(const struct domain *d,
                               unsigned long start_fn, unsigned long nr)
{
    /*
     * Note that the !hap_enabled() here has two effects:
     * - exclude shadow mode (which doesn't support large MMIO mappings),
     * - exclude PV guests, should execution reach this code for such.
     * So be careful when altering this.
     */
    if ( !hap_enabled(d) ||
         (start_fn & ((1UL << PAGE_ORDER_2M) - 1)) || !(nr >> PAGE_ORDER_2M) )
        return PAGE_ORDER_4K;

    if ( 0 /*
            * Don't use 1Gb pages, to limit the iteration count in
            * set_typed_p2m_entry() when it needs to zap M2P entries
            * for a RAM range.
            */ &&
         !(start_fn & ((1UL << PAGE_ORDER_1G) - 1)) && (nr >> PAGE_ORDER_1G) &&
         hap_has_1gb )
        return PAGE_ORDER_1G;

    if ( hap_has_2mb )
        return PAGE_ORDER_2M;

    return PAGE_ORDER_4K;
}

#define MAP_MMIO_MAX_ITER 64 /* pretty arbitrary */

int map_mmio_regions(struct domain *d,
                     gfn_t start_gfn,
                     unsigned long nr,
                     mfn_t mfn)
{
    int ret = 0;
    unsigned long i;
    unsigned int iter, order;

    if ( !paging_mode_translate(d) )
    {
        ASSERT_UNREACHABLE();
        return -EOPNOTSUPP;
    }

    for ( iter = i = 0; i < nr && iter < MAP_MMIO_MAX_ITER;
          i += 1UL << order, ++iter )
    {
        /* OR'ing gfn and mfn values will return an order suitable to both. */
        for ( order = mmio_order(d, (gfn_x(start_gfn) + i) | (mfn_x(mfn) + i), nr - i); ;
              order = ret - 1 )
        {
            ret = set_mmio_p2m_entry(d, gfn_add(start_gfn, i),
                                     mfn_add(mfn, i), order);
            if ( ret <= 0 )
                break;
            ASSERT(ret <= order);
        }
        if ( ret < 0 )
            break;
    }

    return i == nr ? 0 : i ?: ret;
}

int unmap_mmio_regions(struct domain *d,
                       gfn_t start_gfn,
                       unsigned long nr,
                       mfn_t mfn)
{
    int ret = 0;
    unsigned long i;
    unsigned int iter, order;

    if ( !paging_mode_translate(d) )
    {
        ASSERT_UNREACHABLE();
        return -EOPNOTSUPP;
    }

    for ( iter = i = 0; i < nr && iter < MAP_MMIO_MAX_ITER;
          i += 1UL << order, ++iter )
    {
        /* OR'ing gfn and mfn values will return an order suitable to both. */
        for ( order = mmio_order(d, (gfn_x(start_gfn) + i) | (mfn_x(mfn) + i), nr - i); ;
              order = ret - 1 )
        {
            ret = clear_mmio_p2m_entry(d, gfn_x(start_gfn) + i,
                                       mfn_add(mfn, i), order);
            if ( ret <= 0 )
                break;
            ASSERT(ret <= order);
        }
        if ( ret < 0 )
            break;
    }

    return i == nr ? 0 : i ?: ret;
}

int altp2m_get_effective_entry(struct p2m_domain *ap2m, gfn_t gfn, mfn_t *mfn,
                               p2m_type_t *t, p2m_access_t *a,
                               bool prepopulate)
{
    *mfn = ap2m->get_entry(ap2m, gfn, t, a, 0, NULL, NULL);

    /* Check host p2m if no valid entry in alternate */
    if ( !mfn_valid(*mfn) && !p2m_is_hostp2m(ap2m) )
    {
        struct p2m_domain *hp2m = p2m_get_hostp2m(ap2m->domain);
        unsigned int page_order;
        int rc;

        *mfn = __get_gfn_type_access(hp2m, gfn_x(gfn), t, a,
                                     P2M_ALLOC | P2M_UNSHARE, &page_order, 0);

        rc = -ESRCH;
        if ( !mfn_valid(*mfn) || *t != p2m_ram_rw )
            return rc;

        /* If this is a superpage, copy that first */
        if ( prepopulate && page_order != PAGE_ORDER_4K )
        {
            unsigned long mask = ~((1UL << page_order) - 1);
            gfn_t gfn_aligned = _gfn(gfn_x(gfn) & mask);
            mfn_t mfn_aligned = _mfn(mfn_x(*mfn) & mask);

            rc = ap2m->set_entry(ap2m, gfn_aligned, mfn_aligned, page_order, *t, *a, 1);
            if ( rc )
                return rc;
        }
    }

    return 0;
}

void p2m_altp2m_check(struct vcpu *v, uint16_t idx)
{
    if ( altp2m_active(v->domain) )
        p2m_switch_vcpu_altp2m_by_id(v, idx);
}

bool_t p2m_switch_vcpu_altp2m_by_id(struct vcpu *v, unsigned int idx)
{
    struct domain *d = v->domain;
    bool_t rc = 0;

    if ( idx >= MAX_ALTP2M )
        return rc;

    altp2m_list_lock(d);

    if ( d->arch.altp2m_eptp[idx] != mfn_x(INVALID_MFN) )
    {
        if ( idx != vcpu_altp2m(v).p2midx )
        {
            atomic_dec(&p2m_get_altp2m(v)->active_vcpus);
            vcpu_altp2m(v).p2midx = idx;
            atomic_inc(&p2m_get_altp2m(v)->active_vcpus);
            altp2m_vcpu_update_p2m(v);
        }
        rc = 1;
    }

    altp2m_list_unlock(d);
    return rc;
}

/*
 * Read info about the gfn in an altp2m, locking the gfn.
 *
 * If the entry is valid, pass the results back to the caller.
 *
 * If the entry was invalid, and the host's entry is also invalid,
 * return to the caller without any changes.
 *
 * If the entry is invalid, and the host entry was valid, propagate
 * the host's entry to the altp2m (retaining page order), and indicate
 * that the caller should re-try the faulting instruction.
 */
bool p2m_altp2m_get_or_propagate(struct p2m_domain *ap2m, unsigned long gfn_l,
                                 mfn_t *mfn, p2m_type_t *p2mt,
                                 p2m_access_t *p2ma, unsigned int page_order)
{
    p2m_type_t ap2mt;
    p2m_access_t ap2ma;
    unsigned long mask;
    gfn_t gfn;
    mfn_t amfn;
    int rc;

    /*
     * NB we must get the full lock on the altp2m here, in addition to
     * the lock on the individual gfn, since we may change a range of
     * gfns below.
     */
    p2m_lock(ap2m);

    amfn = get_gfn_type_access(ap2m, gfn_l, &ap2mt, &ap2ma, 0, NULL);

    if ( !mfn_eq(amfn, INVALID_MFN) )
    {
        p2m_unlock(ap2m);
        *mfn  = amfn;
        *p2mt = ap2mt;
        *p2ma = ap2ma;
        return false;
    }

    /* Host entry is also invalid; don't bother setting the altp2m entry. */
    if ( mfn_eq(*mfn, INVALID_MFN) )
    {
        p2m_unlock(ap2m);
        return false;
    }

    /*
     * If this is a superpage mapping, round down both frame numbers
     * to the start of the superpage.  NB that we repupose `amfn`
     * here.
     */
    mask = ~((1UL << page_order) - 1);
    amfn = _mfn(mfn_x(*mfn) & mask);
    gfn = _gfn(gfn_l & mask);

    rc = p2m_set_entry(ap2m, gfn, amfn, page_order, *p2mt, *p2ma);
    p2m_unlock(ap2m);

    if ( rc )
    {
        gprintk(XENLOG_ERR,
                "failed to set entry for %"PRI_gfn" -> %"PRI_mfn" altp2m %u, rc %d\n",
                gfn_l, mfn_x(amfn), vcpu_altp2m(current).p2midx, rc);
        domain_crash(ap2m->domain);
    }

    return true;
}

enum altp2m_reset_type {
    ALTP2M_RESET,
    ALTP2M_DEACTIVATE
};

static void p2m_reset_altp2m(struct domain *d, unsigned int idx,
                             enum altp2m_reset_type reset_type)
{
    struct p2m_domain *p2m;

    ASSERT(idx < MAX_ALTP2M);
    p2m = array_access_nospec(d->arch.altp2m_p2m, idx);

    p2m_lock(p2m);

    p2m_flush_table_locked(p2m);

    if ( reset_type == ALTP2M_DEACTIVATE )
        p2m_free_logdirty(p2m);

    /* Uninit and reinit ept to force TLB shootdown */
    ept_p2m_uninit(p2m);
    ept_p2m_init(p2m);

    p2m->min_remapped_gfn = gfn_x(INVALID_GFN);
    p2m->max_remapped_gfn = 0;

    p2m_unlock(p2m);
}

void p2m_flush_altp2m(struct domain *d)
{
    unsigned int i;

    altp2m_list_lock(d);

    for ( i = 0; i < MAX_ALTP2M; i++ )
    {
        p2m_reset_altp2m(d, i, ALTP2M_DEACTIVATE);
        d->arch.altp2m_eptp[i] = mfn_x(INVALID_MFN);
        d->arch.altp2m_visible_eptp[i] = mfn_x(INVALID_MFN);
    }

    altp2m_list_unlock(d);
}

static int p2m_activate_altp2m(struct domain *d, unsigned int idx,
                               p2m_access_t hvmmem_default_access)
{
    struct p2m_domain *hostp2m, *p2m;
    int rc;

    ASSERT(idx < MAX_ALTP2M);

    p2m = array_access_nospec(d->arch.altp2m_p2m, idx);
    hostp2m = p2m_get_hostp2m(d);

    p2m_lock(p2m);

    rc = p2m_init_logdirty(p2m);

    if ( rc )
        goto out;

    /* The following is really just a rangeset copy. */
    rc = rangeset_merge(p2m->logdirty_ranges, hostp2m->logdirty_ranges);

    if ( rc )
    {
        p2m_free_logdirty(p2m);
        goto out;
    }

    p2m->default_access = hvmmem_default_access;
    p2m->domain = hostp2m->domain;
    p2m->global_logdirty = hostp2m->global_logdirty;
    p2m->min_remapped_gfn = gfn_x(INVALID_GFN);
    p2m->max_mapped_pfn = p2m->max_remapped_gfn = 0;

    p2m_init_altp2m_ept(d, idx);

 out:
    p2m_unlock(p2m);

    return rc;
}

int p2m_init_altp2m_by_id(struct domain *d, unsigned int idx)
{
    int rc = -EINVAL;
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);

    if ( idx >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) )
        return rc;

    altp2m_list_lock(d);

    if ( d->arch.altp2m_eptp[array_index_nospec(idx, MAX_EPTP)] ==
         mfn_x(INVALID_MFN) )
        rc = p2m_activate_altp2m(d, idx, hostp2m->default_access);

    altp2m_list_unlock(d);
    return rc;
}

int p2m_init_next_altp2m(struct domain *d, uint16_t *idx,
                         xenmem_access_t hvmmem_default_access)
{
    int rc = -EINVAL;
    unsigned int i;
    p2m_access_t a;
    struct p2m_domain *hostp2m = p2m_get_hostp2m(d);

    if ( hvmmem_default_access > XENMEM_access_default ||
         !xenmem_access_to_p2m_access(hostp2m, hvmmem_default_access, &a) )
        return rc;

    altp2m_list_lock(d);

    for ( i = 0; i < MAX_ALTP2M; i++ )
    {
        if ( d->arch.altp2m_eptp[i] != mfn_x(INVALID_MFN) )
            continue;

        rc = p2m_activate_altp2m(d, i, a);

        if ( !rc )
            *idx = i;

        break;
    }

    altp2m_list_unlock(d);
    return rc;
}

int p2m_destroy_altp2m_by_id(struct domain *d, unsigned int idx)
{
    struct p2m_domain *p2m;
    int rc = -EBUSY;

    if ( !idx || idx >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) )
        return rc;

    rc = domain_pause_except_self(d);
    if ( rc )
        return rc;

    rc = -EBUSY;
    altp2m_list_lock(d);

    if ( d->arch.altp2m_eptp[array_index_nospec(idx, MAX_EPTP)] !=
         mfn_x(INVALID_MFN) )
    {
        p2m = array_access_nospec(d->arch.altp2m_p2m, idx);

        if ( !_atomic_read(p2m->active_vcpus) )
        {
            p2m_reset_altp2m(d, idx, ALTP2M_DEACTIVATE);
            d->arch.altp2m_eptp[array_index_nospec(idx, MAX_EPTP)] =
                mfn_x(INVALID_MFN);
            d->arch.altp2m_visible_eptp[array_index_nospec(idx, MAX_EPTP)] =
                mfn_x(INVALID_MFN);
            rc = 0;
        }
    }

    altp2m_list_unlock(d);

    domain_unpause_except_self(d);

    return rc;
}

int p2m_switch_domain_altp2m_by_id(struct domain *d, unsigned int idx)
{
    struct vcpu *v;
    int rc = -EINVAL;

    if ( idx >= MAX_ALTP2M )
        return rc;

    rc = domain_pause_except_self(d);
    if ( rc )
        return rc;

    rc = -EINVAL;
    altp2m_list_lock(d);

    if ( d->arch.altp2m_visible_eptp[idx] != mfn_x(INVALID_MFN) )
    {
        for_each_vcpu( d, v )
            if ( idx != vcpu_altp2m(v).p2midx )
            {
                atomic_dec(&p2m_get_altp2m(v)->active_vcpus);
                vcpu_altp2m(v).p2midx = idx;
                atomic_inc(&p2m_get_altp2m(v)->active_vcpus);
                altp2m_vcpu_update_p2m(v);
            }

        rc = 0;
    }

    altp2m_list_unlock(d);

    domain_unpause_except_self(d);

    return rc;
}

int p2m_change_altp2m_gfn(struct domain *d, unsigned int idx,
                          gfn_t old_gfn, gfn_t new_gfn)
{
    struct p2m_domain *hp2m, *ap2m;
    p2m_access_t a;
    p2m_type_t t;
    mfn_t mfn;
    int rc = -EINVAL;

    if ( idx >=  min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) ||
         d->arch.altp2m_eptp[array_index_nospec(idx, MAX_EPTP)] ==
         mfn_x(INVALID_MFN) )
        return rc;

    hp2m = p2m_get_hostp2m(d);
    ap2m = array_access_nospec(d->arch.altp2m_p2m, idx);

    p2m_lock(hp2m);
    p2m_lock(ap2m);

    if ( gfn_eq(new_gfn, INVALID_GFN) )
    {
        mfn = ap2m->get_entry(ap2m, old_gfn, &t, &a, 0, NULL, NULL);
        rc = mfn_valid(mfn)
             ? p2m_remove_page(ap2m, old_gfn, mfn, PAGE_ORDER_4K)
             : 0;
        goto out;
    }

    rc = altp2m_get_effective_entry(ap2m, old_gfn, &mfn, &t, &a,
                                    AP2MGET_prepopulate);
    if ( rc )
        goto out;

    rc = altp2m_get_effective_entry(ap2m, new_gfn, &mfn, &t, &a,
                                    AP2MGET_query);
    if ( rc )
        goto out;

    if ( !ap2m->set_entry(ap2m, old_gfn, mfn, PAGE_ORDER_4K, t, a,
                          (current->domain != d)) )
    {
        rc = 0;

        if ( gfn_x(new_gfn) < ap2m->min_remapped_gfn )
            ap2m->min_remapped_gfn = gfn_x(new_gfn);
        if ( gfn_x(new_gfn) > ap2m->max_remapped_gfn )
            ap2m->max_remapped_gfn = gfn_x(new_gfn);
    }

 out:
    p2m_unlock(ap2m);
    p2m_unlock(hp2m);
    return rc;
}

int p2m_altp2m_propagate_change(struct domain *d, gfn_t gfn,
                                mfn_t mfn, unsigned int page_order,
                                p2m_type_t p2mt, p2m_access_t p2ma)
{
    struct p2m_domain *p2m;
    p2m_access_t a;
    p2m_type_t t;
    mfn_t m;
    unsigned int i;
    unsigned int reset_count = 0;
    unsigned int last_reset_idx = ~0;
    int ret = 0;

    if ( !altp2m_active(d) )
        return 0;

    altp2m_list_lock(d);

    for ( i = 0; i < MAX_ALTP2M; i++ )
    {
        if ( d->arch.altp2m_eptp[i] == mfn_x(INVALID_MFN) )
            continue;

        p2m = d->arch.altp2m_p2m[i];
        m = get_gfn_type_access(p2m, gfn_x(gfn), &t, &a, 0, NULL);

        /* Check for a dropped page that may impact this altp2m */
        if ( mfn_eq(mfn, INVALID_MFN) &&
             gfn_x(gfn) >= p2m->min_remapped_gfn &&
             gfn_x(gfn) <= p2m->max_remapped_gfn )
        {
            if ( !reset_count++ )
            {
                p2m_reset_altp2m(d, i, ALTP2M_RESET);
                last_reset_idx = i;
            }
            else
            {
                /* At least 2 altp2m's impacted, so reset everything */
                __put_gfn(p2m, gfn_x(gfn));

                for ( i = 0; i < MAX_ALTP2M; i++ )
                {
                    if ( i == last_reset_idx ||
                         d->arch.altp2m_eptp[i] == mfn_x(INVALID_MFN) )
                        continue;

                    p2m_reset_altp2m(d, i, ALTP2M_RESET);
                }

                ret = 0;
                break;
            }
        }
        else if ( !mfn_eq(m, INVALID_MFN) )
        {
            int rc = p2m_set_entry(p2m, gfn, mfn, page_order, p2mt, p2ma);

            /* Best effort: Don't bail on error. */
            if ( !ret )
                ret = rc;
        }

        __put_gfn(p2m, gfn_x(gfn));
    }

    altp2m_list_unlock(d);

    return ret;
}
#endif /* CONFIG_HVM */

/*** Audit ***/

#if P2M_AUDIT
void audit_p2m(struct domain *d,
               uint64_t *orphans,
                uint64_t *m2p_bad,
                uint64_t *p2m_bad)
{
    struct page_info *page;
    struct domain *od;
    unsigned long mfn, gfn;
    mfn_t p2mfn;
    unsigned long orphans_count = 0, mpbad = 0, pmbad = 0;
    p2m_access_t p2ma;
    p2m_type_t type;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( !paging_mode_translate(d) )
        goto out_p2m_audit;

    P2M_PRINTK("p2m audit starts\n");

    p2m_lock(p2m);
    pod_lock(p2m);

    if (p2m->audit_p2m)
        pmbad = p2m->audit_p2m(p2m);

    /* Audit part two: walk the domain's page allocation list, checking
     * the m2p entries. */
    spin_lock(&d->page_alloc_lock);
    page_list_for_each ( page, &d->page_list )
    {
        mfn = mfn_x(page_to_mfn(page));

        P2M_PRINTK("auditing guest page, mfn=%#lx\n", mfn);

        od = page_get_owner(page);

        if ( od != d )
        {
            P2M_PRINTK("wrong owner %#lx -> %p(%u) != %p(%u)\n",
                       mfn, od, (od?od->domain_id:-1), d, d->domain_id);
            continue;
        }

        gfn = get_gpfn_from_mfn(mfn);
        if ( gfn == INVALID_M2P_ENTRY )
        {
            orphans_count++;
            P2M_PRINTK("orphaned guest page: mfn=%#lx has invalid gfn\n",
                           mfn);
            continue;
        }

        if ( SHARED_M2P(gfn) )
        {
            P2M_PRINTK("shared mfn (%lx) on domain page list!\n",
                    mfn);
            continue;
        }

        p2mfn = get_gfn_type_access(p2m, gfn, &type, &p2ma, 0, NULL);
        if ( mfn_x(p2mfn) != mfn )
        {
            mpbad++;
            P2M_PRINTK("map mismatch mfn %#lx -> gfn %#lx -> mfn %#lx"
                       " (-> gfn %#lx)\n",
                       mfn, gfn, mfn_x(p2mfn),
                       (mfn_valid(p2mfn)
                        ? get_gpfn_from_mfn(mfn_x(p2mfn))
                        : -1u));
            /* This m2p entry is stale: the domain has another frame in
             * this physical slot.  No great disaster, but for neatness,
             * blow away the m2p entry. */
            set_gpfn_from_mfn(mfn, INVALID_M2P_ENTRY);
        }
        __put_gfn(p2m, gfn);

        P2M_PRINTK("OK: mfn=%#lx, gfn=%#lx, p2mfn=%#lx\n",
                       mfn, gfn, mfn_x(p2mfn));
    }
    spin_unlock(&d->page_alloc_lock);

    pod_unlock(p2m);
    p2m_unlock(p2m);
 
    P2M_PRINTK("p2m audit complete\n");
    if ( orphans_count | mpbad | pmbad )
        P2M_PRINTK("p2m audit found %lu orphans\n", orphans_count);
    if ( mpbad | pmbad )
    {
        P2M_PRINTK("p2m audit found %lu odd p2m, %lu bad m2p entries\n",
                   pmbad, mpbad);
        WARN();
    }

out_p2m_audit:
    *orphans = (uint64_t) orphans_count;
    *m2p_bad = (uint64_t) mpbad;
    *p2m_bad = (uint64_t) pmbad;
}
#endif /* P2M_AUDIT */

#ifdef CONFIG_HVM

/*
 * Add frame from foreign domain to target domain's physmap. Similar to
 * XENMAPSPACE_gmfn but the frame is foreign being mapped into current,
 * and is not removed from foreign domain.
 *
 * Usage: - libxl on pvh dom0 creating a guest and doing privcmd_ioctl_mmap.
 *        - xentrace running on dom0 mapping xenheap pages. foreigndom would
 *          be DOMID_XEN in such a case.
 *        etc..
 *
 * Side Effect: the mfn for fgfn will be refcounted in lower level routines
 *              so it is not lost while mapped here. The refcnt is released
 *              via the XENMEM_remove_from_physmap path.
 *
 * Returns: 0 ==> success
 */
static int p2m_add_foreign(struct domain *tdom, unsigned long fgfn,
                           unsigned long gpfn, domid_t foreigndom)
{
    p2m_type_t p2mt, p2mt_prev;
    mfn_t prev_mfn, mfn;
    struct page_info *page;
    int rc;
    struct domain *fdom;

    /*
     * hvm fixme: until support is added to p2m teardown code to cleanup any
     * foreign entries, limit this to hardware domain only.
     */
    if ( !arch_acquire_resource_check(tdom) )
        return -EPERM;

    if ( foreigndom == DOMID_XEN )
        fdom = rcu_lock_domain(dom_xen);
    else
    {
        rc = rcu_lock_remote_domain_by_id(foreigndom, &fdom);
        if ( rc )
            return rc;

        rc = -EINVAL;
        if ( tdom == fdom )
            goto out;
    }

    rc = xsm_map_gmfn_foreign(XSM_TARGET, tdom, fdom);
    if ( rc )
        goto out;

    /*
     * Take a refcnt on the mfn. NB: following supported for foreign mapping:
     *     ram_rw | ram_logdirty | ram_ro | paging_out.
     */
    page = get_page_from_gfn(fdom, fgfn, &p2mt, P2M_ALLOC);
    if ( !page )
    {
        rc = -EINVAL;
        goto out;
    }

    if ( !p2m_is_ram(p2mt) || p2m_is_shared(p2mt) || p2m_is_hole(p2mt) )
    {
        rc = -EINVAL;
        goto put_one;
    }
    mfn = page_to_mfn(page);

    /* Remove previously mapped page if it is present. */
    prev_mfn = get_gfn(tdom, gpfn, &p2mt_prev);
    if ( mfn_valid(prev_mfn) )
    {
        if ( is_special_page(mfn_to_page(prev_mfn)) )
            /* Special pages are simply unhooked from this phys slot */
            rc = guest_physmap_remove_page(tdom, _gfn(gpfn), prev_mfn, 0);
        else
            /* Normal domain memory is freed, to avoid leaking memory. */
            rc = guest_remove_page(tdom, gpfn);
        if ( rc )
            goto put_both;
    }
    /*
     * Create the new mapping. Can't use guest_physmap_add_page() because it
     * will update the m2p table which will result in  mfn -> gpfn of dom0
     * and not fgfn of domU.
     */
    rc = set_foreign_p2m_entry(tdom, fdom, gpfn, mfn);
    if ( rc )
        gdprintk(XENLOG_WARNING, "set_foreign_p2m_entry failed. "
                 "gpfn:%lx mfn:%lx fgfn:%lx td:%d fd:%d\n",
                 gpfn, mfn_x(mfn), fgfn, tdom->domain_id, fdom->domain_id);

 put_both:
    /*
     * This put_gfn for the above get_gfn for prev_mfn.  We must do this
     * after set_foreign_p2m_entry so another cpu doesn't populate the gpfn
     * before us.
     */
    put_gfn(tdom, gpfn);

 put_one:
    put_page(page);

 out:
    if ( fdom )
        rcu_unlock_domain(fdom);

    return rc;
}

int xenmem_add_to_physmap_one(
    struct domain *d,
    unsigned int space,
    union add_to_physmap_extra extra,
    unsigned long idx,
    gfn_t gpfn)
{
    struct page_info *page = NULL;
    unsigned long gfn = 0 /* gcc ... */, old_gpfn;
    mfn_t prev_mfn;
    int rc = 0;
    mfn_t mfn = INVALID_MFN;
    p2m_type_t p2mt;

    switch ( space )
    {
    case XENMAPSPACE_shared_info:
        if ( idx == 0 )
            mfn = virt_to_mfn(d->shared_info);
        break;

    case XENMAPSPACE_grant_table:
        rc = gnttab_map_frame(d, idx, gpfn, &mfn);
        if ( rc )
            return rc;
        /* Need to take care of the reference obtained in gnttab_map_frame(). */
        page = mfn_to_page(mfn);
        break;

    case XENMAPSPACE_gmfn:
    {
        p2m_type_t p2mt;

        gfn = idx;
        mfn = get_gfn_unshare(d, gfn, &p2mt);
        /* If the page is still shared, exit early */
        if ( p2m_is_shared(p2mt) )
        {
            put_gfn(d, gfn);
            return -ENOMEM;
        }
        page = get_page_from_mfn(mfn, d);
        if ( unlikely(!page) )
            mfn = INVALID_MFN;
        break;
    }

    case XENMAPSPACE_gmfn_foreign:
        return p2m_add_foreign(d, idx, gfn_x(gpfn), extra.foreign_domid);
    }

    if ( mfn_eq(mfn, INVALID_MFN) )
    {
        rc = -EINVAL;
        goto put_both;
    }

    /*
     * Note that we're (ab)using GFN locking (to really be locking of the
     * entire P2M) here in (at least) two ways: Finer grained locking would
     * expose lock order violations in the XENMAPSPACE_gmfn case (due to the
     * earlier get_gfn_unshare() above). Plus at the very least for the grant
     * table v2 status page case we need to guarantee that the same page can
     * only appear at a single GFN. While this is a property we want in
     * general, for pages which can subsequently be freed this imperative:
     * Upon freeing we wouldn't be able to find other mappings in the P2M
     * (unless we did a brute force search).
     */
    prev_mfn = get_gfn(d, gfn_x(gpfn), &p2mt);

    /* XENMAPSPACE_gmfn: Check if the MFN is associated with another GFN. */
    old_gpfn = get_gpfn_from_mfn(mfn_x(mfn));
    ASSERT(!SHARED_M2P(old_gpfn));
    if ( space == XENMAPSPACE_gmfn && old_gpfn != gfn )
    {
        rc = -EXDEV;
        goto put_all;
    }

    /* Remove previously mapped page if it was present. */
    if ( p2mt == p2m_mmio_direct )
        rc = -EPERM;
    else if ( mfn_valid(prev_mfn) )
    {
        if ( is_special_page(mfn_to_page(prev_mfn)) )
            /* Special pages are simply unhooked from this phys slot. */
            rc = guest_physmap_remove_page(d, gpfn, prev_mfn, PAGE_ORDER_4K);
        else if ( !mfn_eq(mfn, prev_mfn) )
            /* Normal domain memory is freed, to avoid leaking memory. */
            rc = guest_remove_page(d, gfn_x(gpfn));
    }

    /* Unmap from old location, if any. */
    if ( !rc && old_gpfn != INVALID_M2P_ENTRY )
        rc = guest_physmap_remove_page(d, _gfn(old_gpfn), mfn, PAGE_ORDER_4K);

    /* Map at new location. */
    if ( !rc )
        rc = guest_physmap_add_page(d, gpfn, mfn, PAGE_ORDER_4K);

 put_all:
    put_gfn(d, gfn_x(gpfn));

 put_both:
    /*
     * In the XENMAPSPACE_gmfn case, we took a ref of the gfn at the top.
     * We also may need to transfer ownership of the page reference to our
     * caller.
     */
    if ( space == XENMAPSPACE_gmfn )
    {
        put_gfn(d, gfn);
        if ( !rc && extra.ppage )
        {
            *extra.ppage = page;
            page = NULL;
        }
    }

    if ( page )
        put_page(page);

    return rc;
}

/*
 * Set/clear the #VE suppress bit for a page.  Only available on VMX.
 */
int p2m_set_suppress_ve(struct domain *d, gfn_t gfn, bool suppress_ve,
                        unsigned int altp2m_idx)
{
    int rc;
    struct xen_hvm_altp2m_suppress_ve_multi sve = {
        altp2m_idx, suppress_ve, 0, 0, gfn_x(gfn), gfn_x(gfn), 0
    };

    if ( !(rc = p2m_set_suppress_ve_multi(d, &sve)) )
        rc = sve.first_error;

    return rc;
}

/*
 * Set/clear the #VE suppress bit for multiple pages.  Only available on VMX.
 */
int p2m_set_suppress_ve_multi(struct domain *d,
                              struct xen_hvm_altp2m_suppress_ve_multi *sve)
{
    struct p2m_domain *host_p2m = p2m_get_hostp2m(d);
    struct p2m_domain *ap2m = NULL;
    struct p2m_domain *p2m = host_p2m;
    uint64_t start = sve->first_gfn;
    int rc = 0;

    if ( sve->view > 0 )
    {
        if ( sve->view >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) ||
             d->arch.altp2m_eptp[array_index_nospec(sve->view, MAX_EPTP)] ==
             mfn_x(INVALID_MFN) )
            return -EINVAL;

        p2m = ap2m = array_access_nospec(d->arch.altp2m_p2m, sve->view);
    }

    p2m_lock(host_p2m);

    if ( ap2m )
        p2m_lock(ap2m);

    while ( sve->last_gfn >= start )
    {
        p2m_access_t a;
        p2m_type_t t;
        mfn_t mfn;
        int err = 0;

        if ( (err = altp2m_get_effective_entry(p2m, _gfn(start), &mfn, &t, &a,
                                               AP2MGET_query)) &&
             !sve->first_error )
        {
            sve->first_error_gfn = start; /* Save the gfn of the first error */
            sve->first_error = err; /* Save the first error code */
        }

        if ( !err && (err = p2m->set_entry(p2m, _gfn(start), mfn,
                                           PAGE_ORDER_4K, t, a,
                                           sve->suppress_ve)) &&
             !sve->first_error )
        {
            sve->first_error_gfn = start; /* Save the gfn of the first error */
            sve->first_error = err; /* Save the first error code */
        }

        /* Check for continuation if it's not the last iteration. */
        if ( sve->last_gfn >= ++start && hypercall_preempt_check() )
        {
            rc = -ERESTART;
            break;
        }
    }

    sve->first_gfn = start;

    if ( ap2m )
        p2m_unlock(ap2m);

    p2m_unlock(host_p2m);

    return rc;
}

int p2m_get_suppress_ve(struct domain *d, gfn_t gfn, bool *suppress_ve,
                        unsigned int altp2m_idx)
{
    struct p2m_domain *host_p2m = p2m_get_hostp2m(d);
    struct p2m_domain *ap2m = NULL;
    struct p2m_domain *p2m;
    mfn_t mfn;
    p2m_access_t a;
    p2m_type_t t;
    int rc = 0;

    if ( altp2m_idx > 0 )
    {
        if ( altp2m_idx >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) ||
             d->arch.altp2m_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)] ==
             mfn_x(INVALID_MFN) )
            return -EINVAL;

        p2m = ap2m = array_access_nospec(d->arch.altp2m_p2m, altp2m_idx);
    }
    else
        p2m = host_p2m;

    gfn_lock(host_p2m, gfn, 0);

    if ( ap2m )
        p2m_lock(ap2m);

    mfn = p2m->get_entry(p2m, gfn, &t, &a, 0, NULL, suppress_ve);
    if ( !mfn_valid(mfn) )
        rc = -ESRCH;

    if ( ap2m )
        p2m_unlock(ap2m);

    gfn_unlock(host_p2m, gfn, 0);

    return rc;
}

int p2m_set_altp2m_view_visibility(struct domain *d, unsigned int altp2m_idx,
                                   uint8_t visible)
{
    int rc = 0;

    altp2m_list_lock(d);

    /*
     * Eptp index is correlated with altp2m index and should not exceed
     * min(MAX_ALTP2M, MAX_EPTP).
     */
    if ( altp2m_idx >= min(ARRAY_SIZE(d->arch.altp2m_p2m), MAX_EPTP) ||
         d->arch.altp2m_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)] ==
         mfn_x(INVALID_MFN) )
        rc = -EINVAL;
    else if ( visible )
        d->arch.altp2m_visible_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)] =
            d->arch.altp2m_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)];
    else
        d->arch.altp2m_visible_eptp[array_index_nospec(altp2m_idx, MAX_EPTP)] =
            mfn_x(INVALID_MFN);

    altp2m_list_unlock(d);

    return rc;
}

#endif /* CONFIG_HVM */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
