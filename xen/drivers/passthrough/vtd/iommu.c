/*
 * Copyright (c) 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) Ashok Raj <ashok.raj@intel.com>
 * Copyright (C) Shaohua Li <shaohua.li@intel.com>
 * Copyright (C) Allen Kay <allen.m.kay@intel.com> - adapted to xen
 */

#include <xen/irq.h>
#include <xen/sched.h>
#include <xen/xmalloc.h>
#include <xen/domain_page.h>
#include <xen/iocap.h>
#include <xen/iommu.h>
#include <xen/numa.h>
#include <xen/softirq.h>
#include <xen/time.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include <xen/keyhandler.h>
#include <asm/msi.h>
#include <asm/nops.h>
#include <asm/irq.h>
#include <asm/hvm/vmx/vmx.h>
#include <asm/p2m.h>
#include <mach_apic.h>
#include "iommu.h"
#include "dmar.h"
#include "extern.h"
#include "vtd.h"
#include "../ats.h"

/* dom_io is used as a sentinel for quarantined devices */
#define QUARANTINE_SKIP(d) ((d) == dom_io && !dom_iommu(d)->arch.vtd.pgd_maddr)

/* Possible unfiltered LAPIC/MSI messages from untrusted sources? */
bool __read_mostly untrusted_msi;

bool __read_mostly iommu_igfx = true;
bool __read_mostly iommu_qinval = true;
#ifndef iommu_snoop
bool __read_mostly iommu_snoop = true;
#endif

static unsigned int __initdata nr_iommus;

static struct iommu_ops vtd_ops;
static struct tasklet vtd_fault_tasklet;

static int setup_hwdom_device(u8 devfn, struct pci_dev *);
static void setup_hwdom_rmrr(struct domain *d);

static int domain_iommu_domid(struct domain *d,
                              struct vtd_iommu *iommu)
{
    unsigned long nr_dom, i;

    nr_dom = cap_ndoms(iommu->cap);
    i = find_first_bit(iommu->domid_bitmap, nr_dom);
    while ( i < nr_dom )
    {
        if ( iommu->domid_map[i] == d->domain_id )
            return i;

        i = find_next_bit(iommu->domid_bitmap, nr_dom, i+1);
    }

    if ( !d->is_dying )
        dprintk(XENLOG_ERR VTDPREFIX,
                "Cannot get valid iommu %u domid: %pd\n",
                iommu->index, d);

    return -1;
}

#define DID_FIELD_WIDTH 16
#define DID_HIGH_OFFSET 8
static int context_set_domain_id(struct context_entry *context,
                                 struct domain *d,
                                 struct vtd_iommu *iommu)
{
    unsigned long nr_dom, i;
    int found = 0;

    ASSERT(spin_is_locked(&iommu->lock));

    nr_dom = cap_ndoms(iommu->cap);
    i = find_first_bit(iommu->domid_bitmap, nr_dom);
    while ( i < nr_dom )
    {
        if ( iommu->domid_map[i] == d->domain_id )
        {
            found = 1;
            break;
        }
        i = find_next_bit(iommu->domid_bitmap, nr_dom, i+1);
    }

    if ( found == 0 )
    {
        i = find_first_zero_bit(iommu->domid_bitmap, nr_dom);
        if ( i >= nr_dom )
        {
            dprintk(XENLOG_ERR VTDPREFIX, "IOMMU: no free domain ids\n");
            return -EFAULT;
        }
        iommu->domid_map[i] = d->domain_id;
    }

    set_bit(i, iommu->domid_bitmap);
    context->hi |= (i & ((1 << DID_FIELD_WIDTH) - 1)) << DID_HIGH_OFFSET;
    return 0;
}

static int context_get_domain_id(struct context_entry *context,
                                 struct vtd_iommu *iommu)
{
    unsigned long dom_index, nr_dom;
    int domid = -1;

    if (iommu && context)
    {
        nr_dom = cap_ndoms(iommu->cap);

        dom_index = context_domain_id(*context);

        if ( dom_index < nr_dom && iommu->domid_map )
            domid = iommu->domid_map[dom_index];
        else
            dprintk(XENLOG_DEBUG VTDPREFIX,
                    "dom_index %lu exceeds nr_dom %lu or iommu has no domid_map\n",
                    dom_index, nr_dom);
    }
    return domid;
}

static void cleanup_domid_map(struct domain *domain, struct vtd_iommu *iommu)
{
    int iommu_domid = domain_iommu_domid(domain, iommu);

    if ( iommu_domid >= 0 )
    {
        clear_bit(iommu_domid, iommu->domid_bitmap);
        iommu->domid_map[iommu_domid] = 0;
    }
}

static void sync_cache(const void *addr, unsigned int size)
{
    static unsigned long clflush_size = 0;
    const void *end = addr + size;

    if ( clflush_size == 0 )
        clflush_size = get_cache_line_size();

    addr -= (unsigned long)addr & (clflush_size - 1);
    for ( ; addr < end; addr += clflush_size )
/*
 * The arguments to a macro must not include preprocessor directives. Doing so
 * results in undefined behavior, so we have to create some defines here in
 * order to avoid it.
 */
#if defined(HAVE_AS_CLWB)
# define CLWB_ENCODING "clwb %[p]"
#elif defined(HAVE_AS_XSAVEOPT)
# define CLWB_ENCODING "data16 xsaveopt %[p]" /* clwb */
#else
# define CLWB_ENCODING ".byte 0x66, 0x0f, 0xae, 0x30" /* clwb (%%rax) */
#endif

#define BASE_INPUT(addr) [p] "m" (*(const char *)(addr))
#if defined(HAVE_AS_CLWB) || defined(HAVE_AS_XSAVEOPT)
# define INPUT BASE_INPUT
#else
# define INPUT(addr) "a" (addr), BASE_INPUT(addr)
#endif
        /*
         * Note regarding the use of NOP_DS_PREFIX: it's faster to do a clflush
         * + prefix than a clflush + nop, and hence the prefix is added instead
         * of letting the alternative framework fill the gap by appending nops.
         */
        alternative_io_2(".byte " __stringify(NOP_DS_PREFIX) "; clflush %[p]",
                         "data16 clflush %[p]", /* clflushopt */
                         X86_FEATURE_CLFLUSHOPT,
                         CLWB_ENCODING,
                         X86_FEATURE_CLWB, /* no outputs */,
                         INPUT(addr));
#undef INPUT
#undef BASE_INPUT
#undef CLWB_ENCODING

    alternative_2("", "sfence", X86_FEATURE_CLFLUSHOPT,
                      "sfence", X86_FEATURE_CLWB);
}

/* Allocate page table, return its machine address */
uint64_t alloc_pgtable_maddr(unsigned long npages, nodeid_t node)
{
    struct page_info *pg, *cur_pg;
    unsigned int i;

    pg = alloc_domheap_pages(NULL, get_order_from_pages(npages),
                             (node == NUMA_NO_NODE) ? 0 : MEMF_node(node));
    if ( !pg )
        return 0;

    cur_pg = pg;
    for ( i = 0; i < npages; i++ )
    {
        void *vaddr = __map_domain_page(cur_pg);

        clear_page(vaddr);

        if ( (iommu_ops.init ? &iommu_ops : &vtd_ops)->sync_cache )
            sync_cache(vaddr, PAGE_SIZE);
        unmap_domain_page(vaddr);
        cur_pg++;
    }

    return page_to_maddr(pg);
}

void free_pgtable_maddr(u64 maddr)
{
    if ( maddr != 0 )
        free_domheap_page(maddr_to_page(maddr));
}

/* context entry handling */
static u64 bus_to_context_maddr(struct vtd_iommu *iommu, u8 bus)
{
    struct root_entry *root, *root_entries;
    u64 maddr;

    ASSERT(spin_is_locked(&iommu->lock));
    root_entries = (struct root_entry *)map_vtd_domain_page(iommu->root_maddr);
    root = &root_entries[bus];
    if ( !root_present(*root) )
    {
        maddr = alloc_pgtable_maddr(1, iommu->node);
        if ( maddr == 0 )
        {
            unmap_vtd_domain_page(root_entries);
            return 0;
        }
        set_root_value(*root, maddr);
        set_root_present(*root);
        iommu_sync_cache(root, sizeof(struct root_entry));
    }
    maddr = (u64) get_context_addr(*root);
    unmap_vtd_domain_page(root_entries);
    return maddr;
}

static u64 addr_to_dma_page_maddr(struct domain *domain, u64 addr, int alloc)
{
    struct domain_iommu *hd = dom_iommu(domain);
    int addr_width = agaw_to_width(hd->arch.vtd.agaw);
    struct dma_pte *parent, *pte = NULL;
    int level = agaw_to_level(hd->arch.vtd.agaw);
    int offset;
    u64 pte_maddr = 0;

    addr &= (((u64)1) << addr_width) - 1;
    ASSERT(spin_is_locked(&hd->arch.mapping_lock));
    if ( !hd->arch.vtd.pgd_maddr )
    {
        struct page_info *pg;

        if ( !alloc || !(pg = iommu_alloc_pgtable(domain)) )
            goto out;

        hd->arch.vtd.pgd_maddr = page_to_maddr(pg);
    }

    parent = (struct dma_pte *)map_vtd_domain_page(hd->arch.vtd.pgd_maddr);
    while ( level > 1 )
    {
        offset = address_level_offset(addr, level);
        pte = &parent[offset];

        pte_maddr = dma_pte_addr(*pte);
        if ( !pte_maddr )
        {
            struct page_info *pg;

            if ( !alloc )
                break;

            pg = iommu_alloc_pgtable(domain);
            if ( !pg )
                break;

            pte_maddr = page_to_maddr(pg);
            dma_set_pte_addr(*pte, pte_maddr);

            /*
             * high level table always sets r/w, last level
             * page table control read/write
             */
            dma_set_pte_readable(*pte);
            dma_set_pte_writable(*pte);
            iommu_sync_cache(pte, sizeof(struct dma_pte));
        }

        if ( level == 2 )
            break;

        unmap_vtd_domain_page(parent);
        parent = map_vtd_domain_page(pte_maddr);
        level--;
    }

    unmap_vtd_domain_page(parent);
 out:
    return pte_maddr;
}

static uint64_t domain_pgd_maddr(struct domain *d, unsigned int nr_pt_levels)
{
    struct domain_iommu *hd = dom_iommu(d);
    uint64_t pgd_maddr;
    unsigned int agaw;

    ASSERT(spin_is_locked(&hd->arch.mapping_lock));

    if ( iommu_use_hap_pt(d) )
    {
        pagetable_t pgt = p2m_get_pagetable(p2m_get_hostp2m(d));

        return pagetable_get_paddr(pgt);
    }

    if ( !hd->arch.vtd.pgd_maddr )
    {
        /* Ensure we have pagetables allocated down to leaf PTE. */
        addr_to_dma_page_maddr(d, 0, 1);

        if ( !hd->arch.vtd.pgd_maddr )
            return 0;
    }

    pgd_maddr = hd->arch.vtd.pgd_maddr;

    /* Skip top levels of page tables for 2- and 3-level DRHDs. */
    for ( agaw = level_to_agaw(4);
          agaw != level_to_agaw(nr_pt_levels);
          agaw-- )
    {
        const struct dma_pte *p = map_vtd_domain_page(pgd_maddr);

        pgd_maddr = dma_pte_addr(*p);
        unmap_vtd_domain_page(p);
        if ( !pgd_maddr )
            return 0;
    }

    return pgd_maddr;
}

static void iommu_flush_write_buffer(struct vtd_iommu *iommu)
{
    u32 val;
    unsigned long flags;

    if ( !rwbf_quirk && !cap_rwbf(iommu->cap) )
        return;

    spin_lock_irqsave(&iommu->register_lock, flags);
    val = dmar_readl(iommu->reg, DMAR_GSTS_REG);
    dmar_writel(iommu->reg, DMAR_GCMD_REG, val | DMA_GCMD_WBF);

    /* Make sure hardware complete it */
    IOMMU_FLUSH_WAIT("write buffer", iommu, DMAR_GSTS_REG, dmar_readl,
                     !(val & DMA_GSTS_WBFS), val);

    spin_unlock_irqrestore(&iommu->register_lock, flags);
}

/* return value determine if we need a write buffer flush */
int vtd_flush_context_reg(struct vtd_iommu *iommu, uint16_t did,
                          uint16_t source_id, uint8_t function_mask,
                          uint64_t type, bool flush_non_present_entry)
{
    u64 val = 0;
    unsigned long flags;

    /*
     * In the non-present entry flush case, if hardware doesn't cache
     * non-present entry we do nothing and if hardware cache non-present
     * entry, we flush entries of domain 0 (the domain id is used to cache
     * any non-present entries)
     */
    if ( flush_non_present_entry )
    {
        if ( !cap_caching_mode(iommu->cap) )
            return 1;
        else
            did = 0;
    }

    /* use register invalidation */
    switch ( type )
    {
    case DMA_CCMD_GLOBAL_INVL:
        val = DMA_CCMD_GLOBAL_INVL;
        break;
    case DMA_CCMD_DOMAIN_INVL:
        val = DMA_CCMD_DOMAIN_INVL|DMA_CCMD_DID(did);
        break;
    case DMA_CCMD_DEVICE_INVL:
        val = DMA_CCMD_DEVICE_INVL|DMA_CCMD_DID(did)
            |DMA_CCMD_SID(source_id)|DMA_CCMD_FM(function_mask);
        break;
    default:
        BUG();
    }
    val |= DMA_CCMD_ICC;

    spin_lock_irqsave(&iommu->register_lock, flags);
    dmar_writeq(iommu->reg, DMAR_CCMD_REG, val);

    /* Make sure hardware complete it */
    IOMMU_FLUSH_WAIT("context", iommu, DMAR_CCMD_REG, dmar_readq,
                     !(val & DMA_CCMD_ICC), val);

    spin_unlock_irqrestore(&iommu->register_lock, flags);
    /* flush context entry will implicitly flush write buffer */
    return 0;
}

static int __must_check iommu_flush_context_global(struct vtd_iommu *iommu,
                                                   bool flush_non_present_entry)
{
    return iommu->flush.context(iommu, 0, 0, 0, DMA_CCMD_GLOBAL_INVL,
                                flush_non_present_entry);
}

static int __must_check iommu_flush_context_device(struct vtd_iommu *iommu,
                                                   u16 did, u16 source_id,
                                                   u8 function_mask,
                                                   bool flush_non_present_entry)
{
    return iommu->flush.context(iommu, did, source_id, function_mask,
                                DMA_CCMD_DEVICE_INVL, flush_non_present_entry);
}

/* return value determine if we need a write buffer flush */
int vtd_flush_iotlb_reg(struct vtd_iommu *iommu, uint16_t did, uint64_t addr,
                        unsigned int size_order, uint64_t type,
                        bool flush_non_present_entry, bool flush_dev_iotlb)
{
    int tlb_offset = ecap_iotlb_offset(iommu->ecap);
    u64 val = 0;
    unsigned long flags;

    /*
     * In the non-present entry flush case, if hardware doesn't cache
     * non-present entries we do nothing.
     */
    if ( flush_non_present_entry && !cap_caching_mode(iommu->cap) )
        return 1;

    /* use register invalidation */
    switch ( type )
    {
    case DMA_TLB_GLOBAL_FLUSH:
        val = DMA_TLB_GLOBAL_FLUSH|DMA_TLB_IVT;
        break;
    case DMA_TLB_DSI_FLUSH:
        val = DMA_TLB_DSI_FLUSH|DMA_TLB_IVT|DMA_TLB_DID(did);
        break;
    case DMA_TLB_PSI_FLUSH:
        val = DMA_TLB_PSI_FLUSH|DMA_TLB_IVT|DMA_TLB_DID(did);
        break;
    default:
        BUG();
    }
    /* Note: set drain read/write */
    if ( cap_read_drain(iommu->cap) )
        val |= DMA_TLB_READ_DRAIN;
    if ( cap_write_drain(iommu->cap) )
        val |= DMA_TLB_WRITE_DRAIN;

    spin_lock_irqsave(&iommu->register_lock, flags);
    /* Note: Only uses first TLB reg currently */
    if ( type == DMA_TLB_PSI_FLUSH )
    {
        /* Note: always flush non-leaf currently. */
        dmar_writeq(iommu->reg, tlb_offset, size_order | addr);
    }
    dmar_writeq(iommu->reg, tlb_offset + 8, val);

    /* Make sure hardware complete it */
    IOMMU_FLUSH_WAIT("iotlb", iommu, (tlb_offset + 8), dmar_readq,
                     !(val & DMA_TLB_IVT), val);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    /* check IOTLB invalidation granularity */
    if ( DMA_TLB_IAIG(val) == 0 )
        dprintk(XENLOG_ERR VTDPREFIX, "IOMMU: flush IOTLB failed\n");

    /* flush iotlb entry will implicitly flush write buffer */
    return 0;
}

static int __must_check iommu_flush_iotlb_global(struct vtd_iommu *iommu,
                                                 bool flush_non_present_entry,
                                                 bool flush_dev_iotlb)
{
    int status;

    /* apply platform specific errata workarounds */
    vtd_ops_preamble_quirk(iommu);

    status = iommu->flush.iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH,
                                flush_non_present_entry, flush_dev_iotlb);

    /* undo platform specific errata workarounds */
    vtd_ops_postamble_quirk(iommu);

    return status;
}

static int __must_check iommu_flush_iotlb_dsi(struct vtd_iommu *iommu, u16 did,
                                              bool_t flush_non_present_entry,
                                              bool_t flush_dev_iotlb)
{
    int status;

    /* apply platform specific errata workarounds */
    vtd_ops_preamble_quirk(iommu);

    status = iommu->flush.iotlb(iommu, did, 0, 0, DMA_TLB_DSI_FLUSH,
                                flush_non_present_entry, flush_dev_iotlb);

    /* undo platform specific errata workarounds */
    vtd_ops_postamble_quirk(iommu);

    return status;
}

static int __must_check iommu_flush_iotlb_psi(struct vtd_iommu *iommu, u16 did,
                                              u64 addr, unsigned int order,
                                              bool_t flush_non_present_entry,
                                              bool_t flush_dev_iotlb)
{
    int status;

    ASSERT(!(addr & (~PAGE_MASK_4K)));

    /* Fallback to domain selective flush if no PSI support */
    if ( !cap_pgsel_inv(iommu->cap) )
        return iommu_flush_iotlb_dsi(iommu, did, flush_non_present_entry,
                                     flush_dev_iotlb);

    /* Fallback to domain selective flush if size is too big */
    if ( order > cap_max_amask_val(iommu->cap) )
        return iommu_flush_iotlb_dsi(iommu, did, flush_non_present_entry,
                                     flush_dev_iotlb);

    addr >>= PAGE_SHIFT_4K + order;
    addr <<= PAGE_SHIFT_4K + order;

    /* apply platform specific errata workarounds */
    vtd_ops_preamble_quirk(iommu);

    status = iommu->flush.iotlb(iommu, did, addr, order, DMA_TLB_PSI_FLUSH,
                                flush_non_present_entry, flush_dev_iotlb);

    /* undo platform specific errata workarounds */
    vtd_ops_postamble_quirk(iommu);

    return status;
}

static int __must_check iommu_flush_all(void)
{
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;
    bool_t flush_dev_iotlb;
    int rc = 0;

    flush_all_cache();
    for_each_drhd_unit ( drhd )
    {
        int context_rc, iotlb_rc;

        iommu = drhd->iommu;
        context_rc = iommu_flush_context_global(iommu, 0);
        flush_dev_iotlb = !!find_ats_dev_drhd(iommu);
        iotlb_rc = iommu_flush_iotlb_global(iommu, 0, flush_dev_iotlb);

        /*
         * The current logic for returns:
         *   - positive  invoke iommu_flush_write_buffer to flush cache.
         *   - zero      on success.
         *   - negative  on failure. Continue to flush IOMMU IOTLB on a
         *               best effort basis.
         */
        if ( context_rc > 0 || iotlb_rc > 0 )
            iommu_flush_write_buffer(iommu);
        if ( rc >= 0 )
            rc = context_rc;
        if ( rc >= 0 )
            rc = iotlb_rc;
    }

    if ( rc > 0 )
        rc = 0;

    return rc;
}

static int __must_check iommu_flush_iotlb(struct domain *d, dfn_t dfn,
                                          bool_t dma_old_pte_present,
                                          unsigned long page_count)
{
    struct domain_iommu *hd = dom_iommu(d);
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;
    bool_t flush_dev_iotlb;
    int iommu_domid;
    int ret = 0;

    /*
     * No need pcideves_lock here because we have flush
     * when assign/deassign device
     */
    for_each_drhd_unit ( drhd )
    {
        int rc;

        iommu = drhd->iommu;

        if ( !test_bit(iommu->index, &hd->arch.vtd.iommu_bitmap) )
            continue;

        flush_dev_iotlb = !!find_ats_dev_drhd(iommu);
        iommu_domid= domain_iommu_domid(d, iommu);
        if ( iommu_domid == -1 )
            continue;

        if ( !page_count || (page_count & (page_count - 1)) ||
             dfn_eq(dfn, INVALID_DFN) || !IS_ALIGNED(dfn_x(dfn), page_count) )
            rc = iommu_flush_iotlb_dsi(iommu, iommu_domid,
                                       0, flush_dev_iotlb);
        else
            rc = iommu_flush_iotlb_psi(iommu, iommu_domid,
                                       dfn_to_daddr(dfn),
                                       get_order_from_pages(page_count),
                                       !dma_old_pte_present,
                                       flush_dev_iotlb);

        if ( rc > 0 )
            iommu_flush_write_buffer(iommu);
        else if ( !ret )
            ret = rc;
    }

    return ret;
}

static int __must_check iommu_flush_iotlb_pages(struct domain *d,
                                                dfn_t dfn,
                                                unsigned long page_count,
                                                unsigned int flush_flags)
{
    ASSERT(page_count && !dfn_eq(dfn, INVALID_DFN));
    ASSERT(flush_flags);

    return iommu_flush_iotlb(d, dfn, flush_flags & IOMMU_FLUSHF_modified,
                             page_count);
}

static int __must_check iommu_flush_iotlb_all(struct domain *d)
{
    return iommu_flush_iotlb(d, INVALID_DFN, 0, 0);
}

/* clear one page's page table */
static void dma_pte_clear_one(struct domain *domain, uint64_t addr,
                              unsigned int *flush_flags)
{
    struct domain_iommu *hd = dom_iommu(domain);
    struct dma_pte *page = NULL, *pte = NULL;
    u64 pg_maddr;

    spin_lock(&hd->arch.mapping_lock);
    /* get last level pte */
    pg_maddr = addr_to_dma_page_maddr(domain, addr, 0);
    if ( pg_maddr == 0 )
    {
        spin_unlock(&hd->arch.mapping_lock);
        return;
    }

    page = (struct dma_pte *)map_vtd_domain_page(pg_maddr);
    pte = page + address_level_offset(addr, 1);

    if ( !dma_pte_present(*pte) )
    {
        spin_unlock(&hd->arch.mapping_lock);
        unmap_vtd_domain_page(page);
        return;
    }

    dma_clear_pte(*pte);
    *flush_flags |= IOMMU_FLUSHF_modified;

    spin_unlock(&hd->arch.mapping_lock);
    iommu_sync_cache(pte, sizeof(struct dma_pte));

    unmap_vtd_domain_page(page);
}

static int iommu_set_root_entry(struct vtd_iommu *iommu)
{
    u32 sts;
    unsigned long flags;

    spin_lock_irqsave(&iommu->register_lock, flags);
    dmar_writeq(iommu->reg, DMAR_RTADDR_REG, iommu->root_maddr);

    sts = dmar_readl(iommu->reg, DMAR_GSTS_REG);
    dmar_writel(iommu->reg, DMAR_GCMD_REG, sts | DMA_GCMD_SRTP);

    /* Make sure hardware complete it */
    IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, dmar_readl,
                  (sts & DMA_GSTS_RTPS), sts);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    return 0;
}

static void iommu_enable_translation(struct acpi_drhd_unit *drhd)
{
    u32 sts;
    unsigned long flags;
    struct vtd_iommu *iommu = drhd->iommu;

    if ( is_igd_drhd(drhd) )
    {
        if ( !iommu_igfx )
        {
            printk(XENLOG_INFO VTDPREFIX
                   "Passed iommu=no-igfx option.  Disabling IGD VT-d engine.\n");
            return;
        }

        if ( !is_igd_vt_enabled_quirk() )
        {
            if ( force_iommu )
                panic("BIOS did not enable IGD for VT properly, crash Xen for security purpose\n");

            printk(XENLOG_WARNING VTDPREFIX
                   "BIOS did not enable IGD for VT properly.  Disabling IGD VT-d engine.\n");
            return;
        }
    }

    /* apply platform specific errata workarounds */
    vtd_ops_preamble_quirk(iommu);

    if ( iommu_verbose )
        printk(VTDPREFIX "iommu_enable_translation: iommu->reg = %p\n",
               iommu->reg);
    spin_lock_irqsave(&iommu->register_lock, flags);
    sts = dmar_readl(iommu->reg, DMAR_GSTS_REG);
    dmar_writel(iommu->reg, DMAR_GCMD_REG, sts | DMA_GCMD_TE);

    /* Make sure hardware complete it */
    IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, dmar_readl,
                  (sts & DMA_GSTS_TES), sts);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    /* undo platform specific errata workarounds */
    vtd_ops_postamble_quirk(iommu);

    /* Disable PMRs when VT-d engine takes effect per spec definition */
    disable_pmr(iommu);
}

static void iommu_disable_translation(struct vtd_iommu *iommu)
{
    u32 sts;
    unsigned long flags;

    /* apply platform specific errata workarounds */
    vtd_ops_preamble_quirk(iommu);

    spin_lock_irqsave(&iommu->register_lock, flags);
    sts = dmar_readl(iommu->reg, DMAR_GSTS_REG);
    dmar_writel(iommu->reg, DMAR_GCMD_REG, sts & (~DMA_GCMD_TE));

    /* Make sure hardware complete it */
    IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, dmar_readl,
                  !(sts & DMA_GSTS_TES), sts);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    /* undo platform specific errata workarounds */
    vtd_ops_postamble_quirk(iommu);
}

enum faulttype {
    DMA_REMAP,
    INTR_REMAP,
    UNKNOWN,
};

static const char *dma_remap_fault_reasons[] =
{
    "Software",
    "Present bit in root entry is clear",
    "Present bit in context entry is clear",
    "Invalid context entry",
    "Access beyond MGAW",
    "PTE Write access is not set",
    "PTE Read access is not set",
    "Next page table ptr is invalid",
    "Root table address invalid",
    "Context table ptr is invalid",
    "non-zero reserved fields in RTP",
    "non-zero reserved fields in CTP",
    "non-zero reserved fields in PTE",
    "Blocked a DMA translation request",
};

static const char *intr_remap_fault_reasons[] =
{
    "Detected reserved fields in the decoded interrupt-remapped request",
    "Interrupt index exceeded the interrupt-remapping table size",
    "Present field in the IRTE entry is clear",
    "Error accessing interrupt-remapping table pointed by IRTA_REG",
    "Detected reserved fields in the IRTE entry",
    "Blocked a compatibility format interrupt request",
    "Blocked an interrupt request due to source-id verification failure",
};

static const char *iommu_get_fault_reason(u8 fault_reason,
                                          enum faulttype *fault_type)
{
    if ( fault_reason >= 0x20 && ( fault_reason < 0x20 +
                ARRAY_SIZE(intr_remap_fault_reasons)) )
    {
        *fault_type = INTR_REMAP;
        return intr_remap_fault_reasons[fault_reason - 0x20];
    }
    else if ( fault_reason < ARRAY_SIZE(dma_remap_fault_reasons) )
    {
        *fault_type = DMA_REMAP;
        return dma_remap_fault_reasons[fault_reason];
    }
    else
    {
        *fault_type = UNKNOWN;
        return "Unknown";
    }
}

static int iommu_page_fault_do_one(struct vtd_iommu *iommu, int type,
                                   u8 fault_reason, u16 source_id, u64 addr)
{
    const char *reason, *kind;
    enum faulttype fault_type;
    u16 seg = iommu->drhd->segment;

    reason = iommu_get_fault_reason(fault_reason, &fault_type);
    switch ( fault_type )
    {
    case DMA_REMAP:
        printk(XENLOG_G_WARNING VTDPREFIX
               "DMAR:[%s] Request device [%pp] "
               "fault addr %"PRIx64"\n",
               (type ? "DMA Read" : "DMA Write"),
               &PCI_SBDF2(seg, source_id), addr);
        kind = "DMAR";
        break;
    case INTR_REMAP:
        printk(XENLOG_G_WARNING VTDPREFIX
               "INTR-REMAP: Request device [%pp] "
               "fault index %"PRIx64"\n",
               &PCI_SBDF2(seg, source_id), addr >> 48);
        kind = "INTR-REMAP";
        break;
    default:
        printk(XENLOG_G_WARNING VTDPREFIX
               "UNKNOWN: Request device [%pp] "
               "fault addr %"PRIx64"\n",
               &PCI_SBDF2(seg, source_id), addr);
        kind = "UNKNOWN";
        break;
    }

    printk(XENLOG_G_WARNING VTDPREFIX "%s: reason %02x - %s\n",
           kind, fault_reason, reason);

    if ( iommu_verbose && fault_type == DMA_REMAP )
        print_vtd_entries(iommu, PCI_BUS(source_id), PCI_DEVFN2(source_id),
                          addr >> PAGE_SHIFT);

    return 0;
}

static void iommu_fault_status(u32 fault_status)
{
    if ( fault_status & DMA_FSTS_PFO )
        INTEL_IOMMU_DEBUG("iommu_fault_status: Fault Overflow\n");
    if ( fault_status & DMA_FSTS_PPF )
        INTEL_IOMMU_DEBUG("iommu_fault_status: Primary Pending Fault\n");
    if ( fault_status & DMA_FSTS_AFO )
        INTEL_IOMMU_DEBUG("iommu_fault_status: Advanced Fault Overflow\n");
    if ( fault_status & DMA_FSTS_APF )
        INTEL_IOMMU_DEBUG("iommu_fault_status: Advanced Pending Fault\n");
    if ( fault_status & DMA_FSTS_IQE )
        INTEL_IOMMU_DEBUG("iommu_fault_status: Invalidation Queue Error\n");
    if ( fault_status & DMA_FSTS_ICE )
        INTEL_IOMMU_DEBUG("iommu_fault_status: Invalidation Completion Error\n");
    if ( fault_status & DMA_FSTS_ITE )
        INTEL_IOMMU_DEBUG("iommu_fault_status: Invalidation Time-out Error\n");
}

#define PRIMARY_FAULT_REG_LEN (16)
static void __do_iommu_page_fault(struct vtd_iommu *iommu)
{
    int reg, fault_index;
    u32 fault_status;
    unsigned long flags;

    fault_status = dmar_readl(iommu->reg, DMAR_FSTS_REG);

    iommu_fault_status(fault_status);

    /* FIXME: ignore advanced fault log */
    if ( !(fault_status & DMA_FSTS_PPF) )
        goto clear_overflow;

    fault_index = dma_fsts_fault_record_index(fault_status);
    reg = cap_fault_reg_offset(iommu->cap);
    while (1)
    {
        u8 fault_reason;
        u16 source_id;
        u32 data;
        u64 guest_addr;
        int type;

        /* highest 32 bits */
        spin_lock_irqsave(&iommu->register_lock, flags);
        data = dmar_readl(iommu->reg, reg +
                          fault_index * PRIMARY_FAULT_REG_LEN + 12);
        if ( !(data & DMA_FRCD_F) )
        {
            spin_unlock_irqrestore(&iommu->register_lock, flags);
            break;
        }

        fault_reason = dma_frcd_fault_reason(data);
        type = dma_frcd_type(data);

        data = dmar_readl(iommu->reg, reg +
                          fault_index * PRIMARY_FAULT_REG_LEN + 8);
        source_id = dma_frcd_source_id(data);

        guest_addr = dmar_readq(iommu->reg, reg +
                                fault_index * PRIMARY_FAULT_REG_LEN);
        guest_addr = dma_frcd_page_addr(guest_addr);
        /* clear the fault */
        dmar_writel(iommu->reg, reg +
                    fault_index * PRIMARY_FAULT_REG_LEN + 12, DMA_FRCD_F);
        spin_unlock_irqrestore(&iommu->register_lock, flags);

        iommu_page_fault_do_one(iommu, type, fault_reason,
                                source_id, guest_addr);

        pci_check_disable_device(iommu->drhd->segment,
                                 PCI_BUS(source_id), PCI_DEVFN2(source_id));

        fault_index++;
        if ( fault_index > cap_num_fault_regs(iommu->cap) )
            fault_index = 0;
    }
clear_overflow:
    /* clear primary fault overflow */
    if ( dmar_readl(iommu->reg, DMAR_FSTS_REG) & DMA_FSTS_PFO )
    {
        spin_lock_irqsave(&iommu->register_lock, flags);
        dmar_writel(iommu->reg, DMAR_FSTS_REG, DMA_FSTS_PFO);
        spin_unlock_irqrestore(&iommu->register_lock, flags);
    }
}

static void do_iommu_page_fault(void *unused)
{
    struct acpi_drhd_unit *drhd;

    if ( list_empty(&acpi_drhd_units) )
    {
       INTEL_IOMMU_DEBUG("no device found, something must be very wrong!\n");
       return;
    }

    /*
     * No matter from whom the interrupt came from, check all the
     * IOMMUs present in the system. This allows for having just one
     * tasklet (instead of one per each IOMMUs) and should be more than
     * fine, considering how rare the event of a fault should be.
     */
    for_each_drhd_unit ( drhd )
        __do_iommu_page_fault(drhd->iommu);
}

static void iommu_page_fault(int irq, void *dev_id,
                             struct cpu_user_regs *regs)
{
    /*
     * Just flag the tasklet as runnable. This is fine, according to VT-d
     * specs since a new interrupt won't be generated until we clear all
     * the faults that caused this one to happen.
     */
    tasklet_schedule(&vtd_fault_tasklet);
}

static void dma_msi_unmask(struct irq_desc *desc)
{
    struct vtd_iommu *iommu = desc->action->dev_id;
    unsigned long flags;
    u32 sts;

    /* unmask it */
    spin_lock_irqsave(&iommu->register_lock, flags);
    sts = dmar_readl(iommu->reg, DMAR_FECTL_REG);
    sts &= ~DMA_FECTL_IM;
    dmar_writel(iommu->reg, DMAR_FECTL_REG, sts);
    spin_unlock_irqrestore(&iommu->register_lock, flags);
    iommu->msi.msi_attrib.host_masked = 0;
}

static void dma_msi_mask(struct irq_desc *desc)
{
    unsigned long flags;
    struct vtd_iommu *iommu = desc->action->dev_id;
    u32 sts;

    /* mask it */
    spin_lock_irqsave(&iommu->register_lock, flags);
    sts = dmar_readl(iommu->reg, DMAR_FECTL_REG);
    sts |= DMA_FECTL_IM;
    dmar_writel(iommu->reg, DMAR_FECTL_REG, sts);
    spin_unlock_irqrestore(&iommu->register_lock, flags);
    iommu->msi.msi_attrib.host_masked = 1;
}

static unsigned int dma_msi_startup(struct irq_desc *desc)
{
    dma_msi_unmask(desc);
    return 0;
}

static void dma_msi_ack(struct irq_desc *desc)
{
    irq_complete_move(desc);
    dma_msi_mask(desc);
    move_masked_irq(desc);
}

static void dma_msi_end(struct irq_desc *desc, u8 vector)
{
    dma_msi_unmask(desc);
    end_nonmaskable_irq(desc, vector);
}

static void dma_msi_set_affinity(struct irq_desc *desc, const cpumask_t *mask)
{
    struct msi_msg msg;
    unsigned int dest;
    unsigned long flags;
    struct vtd_iommu *iommu = desc->action->dev_id;

    dest = set_desc_affinity(desc, mask);
    if (dest == BAD_APICID){
        dprintk(XENLOG_ERR VTDPREFIX, "Set iommu interrupt affinity error!\n");
        return;
    }

    msi_compose_msg(desc->arch.vector, NULL, &msg);
    msg.dest32 = dest;
    if (x2apic_enabled)
        msg.address_hi = dest & 0xFFFFFF00;
    ASSERT(!(msg.address_lo & MSI_ADDR_DEST_ID_MASK));
    msg.address_lo |= MSI_ADDR_DEST_ID(dest);
    iommu->msi.msg = msg;

    spin_lock_irqsave(&iommu->register_lock, flags);
    dmar_writel(iommu->reg, DMAR_FEDATA_REG, msg.data);
    dmar_writel(iommu->reg, DMAR_FEADDR_REG, msg.address_lo);
    /*
     * When x2APIC is not enabled, DMAR_FEUADDR_REG is reserved and
     * it's not necessary to update it.
     */
    if ( x2apic_enabled )
        dmar_writel(iommu->reg, DMAR_FEUADDR_REG, msg.address_hi);
    spin_unlock_irqrestore(&iommu->register_lock, flags);
}

static hw_irq_controller dma_msi_type = {
    .typename = "DMA_MSI",
    .startup = dma_msi_startup,
    .shutdown = dma_msi_mask,
    .enable = dma_msi_unmask,
    .disable = dma_msi_mask,
    .ack = dma_msi_ack,
    .end = dma_msi_end,
    .set_affinity = dma_msi_set_affinity,
};

static int __init iommu_set_interrupt(struct acpi_drhd_unit *drhd)
{
    int irq, ret;
    struct acpi_rhsa_unit *rhsa = drhd_to_rhsa(drhd);
    struct vtd_iommu *iommu = drhd->iommu;
    struct irq_desc *desc;

    irq = create_irq(rhsa ? pxm_to_node(rhsa->proximity_domain)
                          : NUMA_NO_NODE,
                     false);
    if ( irq <= 0 )
    {
        dprintk(XENLOG_ERR VTDPREFIX, "IOMMU: no irq available!\n");
        return -EINVAL;
    }

    desc = irq_to_desc(irq);
    desc->handler = &dma_msi_type;
    ret = request_irq(irq, 0, iommu_page_fault, "dmar", iommu);
    if ( ret )
    {
        desc->handler = &no_irq_type;
        destroy_irq(irq);
        dprintk(XENLOG_ERR VTDPREFIX, "IOMMU: can't request irq\n");
        return ret;
    }

    iommu->msi.irq = irq;
    iommu->msi.msi_attrib.pos = MSI_TYPE_IOMMU;
    iommu->msi.msi_attrib.maskbit = 1;
    iommu->msi.msi_attrib.is_64 = 1;
    desc->msi_desc = &iommu->msi;

    return 0;
}

int __init iommu_alloc(struct acpi_drhd_unit *drhd)
{
    struct vtd_iommu *iommu;
    unsigned long sagaw, nr_dom;
    int agaw;

    iommu = xzalloc(struct vtd_iommu);
    if ( iommu == NULL )
        return -ENOMEM;

    iommu->msi.irq = -1; /* No irq assigned yet. */
    iommu->node = NUMA_NO_NODE;
    INIT_LIST_HEAD(&iommu->ats_devices);
    spin_lock_init(&iommu->lock);
    spin_lock_init(&iommu->register_lock);
    spin_lock_init(&iommu->intremap.lock);

    iommu->drhd = drhd;
    drhd->iommu = iommu;

    iommu->reg = ioremap(drhd->address, PAGE_SIZE);
    if ( !iommu->reg )
        return -ENOMEM;
    iommu->index = nr_iommus++;

    iommu->cap = dmar_readq(iommu->reg, DMAR_CAP_REG);
    iommu->ecap = dmar_readq(iommu->reg, DMAR_ECAP_REG);
    iommu->version = dmar_readl(iommu->reg, DMAR_VER_REG);

    if ( !iommu_qinval && !has_register_based_invalidation(iommu) )
    {
        printk(XENLOG_WARNING VTDPREFIX "IOMMU %d: cannot disable Queued Invalidation\n",
               iommu->index);
        iommu_qinval = true;
    }

    if ( iommu_verbose )
    {
        printk(VTDPREFIX "drhd->address = %"PRIx64" iommu->reg = %p\n",
               drhd->address, iommu->reg);
        printk(VTDPREFIX "cap = %"PRIx64" ecap = %"PRIx64"\n",
               iommu->cap, iommu->ecap);
    }
    if ( !(iommu->cap + 1) || !(iommu->ecap + 1) )
        return -ENODEV;

    quirk_iommu_caps(iommu);

    if ( cap_fault_reg_offset(iommu->cap) +
         cap_num_fault_regs(iommu->cap) * PRIMARY_FAULT_REG_LEN >= PAGE_SIZE ||
         ecap_iotlb_offset(iommu->ecap) >= PAGE_SIZE )
    {
        printk(XENLOG_ERR VTDPREFIX "IOMMU: unsupported\n");
        print_iommu_regs(drhd);
        return -ENODEV;
    }

    /* Calculate number of pagetable levels: between 2 and 4. */
    sagaw = cap_sagaw(iommu->cap);
    for ( agaw = level_to_agaw(4); agaw >= 0; agaw-- )
        if ( test_bit(agaw, &sagaw) )
            break;
    if ( agaw < 0 )
    {
        printk(XENLOG_ERR VTDPREFIX "IOMMU: unsupported sagaw %lx\n", sagaw);
        print_iommu_regs(drhd);
        return -ENODEV;
    }
    iommu->nr_pt_levels = agaw_to_level(agaw);

    if ( !ecap_coherent(iommu->ecap) )
        vtd_ops.sync_cache = sync_cache;

    /* allocate domain id bitmap */
    nr_dom = cap_ndoms(iommu->cap);
    iommu->domid_bitmap = xzalloc_array(unsigned long, BITS_TO_LONGS(nr_dom));
    if ( !iommu->domid_bitmap )
        return -ENOMEM;

    /*
     * if Caching mode is set, then invalid translations are tagged with
     * domain id 0, Hence reserve bit 0 for it
     */
    if ( cap_caching_mode(iommu->cap) )
        __set_bit(0, iommu->domid_bitmap);

    iommu->domid_map = xzalloc_array(u16, nr_dom);
    if ( !iommu->domid_map )
        return -ENOMEM;

    return 0;
}

void __init iommu_free(struct acpi_drhd_unit *drhd)
{
    struct vtd_iommu *iommu = drhd->iommu;

    if ( iommu == NULL )
        return;

    drhd->iommu = NULL;

    if ( iommu->root_maddr != 0 )
    {
        free_pgtable_maddr(iommu->root_maddr);
        iommu->root_maddr = 0;
    }

    if ( iommu->reg )
        iounmap(iommu->reg);

    xfree(iommu->domid_bitmap);
    xfree(iommu->domid_map);

    if ( iommu->msi.irq >= 0 )
        destroy_irq(iommu->msi.irq);
    xfree(iommu);
}

#define guestwidth_to_adjustwidth(gaw) ({       \
    int agaw, r = (gaw - 12) % 9;               \
    agaw = (r == 0) ? gaw : (gaw + 9 - r);      \
    if ( agaw > 64 )                            \
        agaw = 64;                              \
    agaw; })

static int intel_iommu_domain_init(struct domain *d)
{
    struct domain_iommu *hd = dom_iommu(d);

    hd->arch.vtd.agaw = width_to_agaw(DEFAULT_DOMAIN_ADDRESS_WIDTH);

    return 0;
}

static void __hwdom_init intel_iommu_hwdom_init(struct domain *d)
{
    struct acpi_drhd_unit *drhd;

    setup_hwdom_pci_devices(d, setup_hwdom_device);
    setup_hwdom_rmrr(d);
    /* Make sure workarounds are applied before enabling the IOMMU(s). */
    arch_iommu_hwdom_init(d);

    if ( iommu_flush_all() )
        printk(XENLOG_WARNING VTDPREFIX
               " IOMMU flush all failed for hardware domain\n");

    for_each_drhd_unit ( drhd )
    {
        if ( iomem_deny_access(d, PFN_DOWN(drhd->address),
                               PFN_DOWN(drhd->address)) )
            BUG();
        iommu_enable_translation(drhd);
    }
}

int domain_context_mapping_one(
    struct domain *domain,
    struct vtd_iommu *iommu,
    u8 bus, u8 devfn, const struct pci_dev *pdev)
{
    struct domain_iommu *hd = dom_iommu(domain);
    struct context_entry *context, *context_entries;
    u64 maddr, pgd_maddr;
    u16 seg = iommu->drhd->segment;
    int rc, ret;
    bool_t flush_dev_iotlb;

    if ( QUARANTINE_SKIP(domain) )
        return 0;

    ASSERT(pcidevs_locked());
    spin_lock(&iommu->lock);
    maddr = bus_to_context_maddr(iommu, bus);
    context_entries = (struct context_entry *)map_vtd_domain_page(maddr);
    context = &context_entries[devfn];

    if ( context_present(*context) )
    {
        int res = 0;

        /* Try to get domain ownership from device structure.  If that's
         * not available, try to read it from the context itself. */
        if ( pdev )
        {
            if ( pdev->domain != domain )
            {
                printk(XENLOG_G_INFO VTDPREFIX "%pd: %pp owned by %pd",
                       domain, &PCI_SBDF3(seg, bus, devfn),
                       pdev->domain);
                res = -EINVAL;
            }
        }
        else
        {
            int cdomain;
            cdomain = context_get_domain_id(context, iommu);
            
            if ( cdomain < 0 )
            {
                printk(XENLOG_G_WARNING VTDPREFIX
                       "%pd: %pp mapped, but can't find owner\n",
                       domain, &PCI_SBDF3(seg, bus, devfn));
                res = -EINVAL;
            }
            else if ( cdomain != domain->domain_id )
            {
                printk(XENLOG_G_INFO VTDPREFIX
                       "%pd: %pp already mapped to d%d",
                       domain, &PCI_SBDF3(seg, bus, devfn), cdomain);
                res = -EINVAL;
            }
        }

        unmap_vtd_domain_page(context_entries);
        spin_unlock(&iommu->lock);
        return res;
    }

    if ( iommu_hwdom_passthrough && is_hardware_domain(domain) )
    {
        context_set_translation_type(*context, CONTEXT_TT_PASS_THRU);
    }
    else
    {
        spin_lock(&hd->arch.mapping_lock);

        pgd_maddr = domain_pgd_maddr(domain, iommu->nr_pt_levels);
        if ( !pgd_maddr )
        {
            spin_unlock(&hd->arch.mapping_lock);
            spin_unlock(&iommu->lock);
            unmap_vtd_domain_page(context_entries);
            return -ENOMEM;
        }

        context_set_address_root(*context, pgd_maddr);
        if ( ats_enabled && ecap_dev_iotlb(iommu->ecap) )
            context_set_translation_type(*context, CONTEXT_TT_DEV_IOTLB);
        else
            context_set_translation_type(*context, CONTEXT_TT_MULTI_LEVEL);

        spin_unlock(&hd->arch.mapping_lock);
    }

    if ( context_set_domain_id(context, domain, iommu) )
    {
        spin_unlock(&iommu->lock);
        unmap_vtd_domain_page(context_entries);
        return -EFAULT;
    }

    context_set_address_width(*context, level_to_agaw(iommu->nr_pt_levels));
    context_set_fault_enable(*context);
    context_set_present(*context);
    iommu_sync_cache(context, sizeof(struct context_entry));
    spin_unlock(&iommu->lock);

    /* Context entry was previously non-present (with domid 0). */
    rc = iommu_flush_context_device(iommu, 0, PCI_BDF2(bus, devfn),
                                    DMA_CCMD_MASK_NOBIT, 1);
    flush_dev_iotlb = !!find_ats_dev_drhd(iommu);
    ret = iommu_flush_iotlb_dsi(iommu, 0, 1, flush_dev_iotlb);

    /*
     * The current logic for returns:
     *   - positive  invoke iommu_flush_write_buffer to flush cache.
     *   - zero      on success.
     *   - negative  on failure. Continue to flush IOMMU IOTLB on a
     *               best effort basis.
     */
    if ( rc > 0 || ret > 0 )
        iommu_flush_write_buffer(iommu);
    if ( rc >= 0 )
        rc = ret;
    if ( rc > 0 )
        rc = 0;

    set_bit(iommu->index, &hd->arch.vtd.iommu_bitmap);

    unmap_vtd_domain_page(context_entries);

    if ( !seg && !rc )
        rc = me_wifi_quirk(domain, bus, devfn, MAP_ME_PHANTOM_FUNC);

    if ( rc )
        domain_context_unmap_one(domain, iommu, bus, devfn);

    return rc;
}

static int domain_context_unmap(struct domain *d, uint8_t devfn,
                                struct pci_dev *pdev);

static int domain_context_mapping(struct domain *domain, u8 devfn,
                                  struct pci_dev *pdev)
{
    struct acpi_drhd_unit *drhd;
    int ret = 0;
    u8 seg = pdev->seg, bus = pdev->bus, secbus;

    drhd = acpi_find_matched_drhd_unit(pdev);
    if ( !drhd )
        return -ENODEV;

    /*
     * Generally we assume only devices from one node to get assigned to a
     * given guest.  But even if not, by replacing the prior value here we
     * guarantee that at least some basic allocations for the device being
     * added will get done against its node.  Any further allocations for
     * this or other devices may be penalized then, but some would also be
     * if we left other than NUMA_NO_NODE untouched here.
     */
    if ( drhd->iommu->node != NUMA_NO_NODE )
        dom_iommu(domain)->node = drhd->iommu->node;

    ASSERT(pcidevs_locked());

    switch ( pdev->type )
    {
    case DEV_TYPE_PCI_HOST_BRIDGE:
        if ( iommu_debug )
            printk(VTDPREFIX "%pd:Hostbridge: skip %pp map\n",
                   domain, &PCI_SBDF3(seg, bus, devfn));
        if ( !is_hardware_domain(domain) )
            return -EPERM;
        break;

    case DEV_TYPE_PCIe_BRIDGE:
    case DEV_TYPE_PCIe2PCI_BRIDGE:
    case DEV_TYPE_LEGACY_PCI_BRIDGE:
        break;

    case DEV_TYPE_PCIe_ENDPOINT:
        if ( iommu_debug )
            printk(VTDPREFIX "%pd:PCIe: map %pp\n",
                   domain, &PCI_SBDF3(seg, bus, devfn));
        ret = domain_context_mapping_one(domain, drhd->iommu, bus, devfn,
                                         pdev);
        if ( !ret && devfn == pdev->devfn && ats_device(pdev, drhd) > 0 )
            enable_ats_device(pdev, &drhd->iommu->ats_devices);

        break;

    case DEV_TYPE_PCI:
        if ( iommu_debug )
            printk(VTDPREFIX "%pd:PCI: map %pp\n",
                   domain, &PCI_SBDF3(seg, bus, devfn));

        ret = domain_context_mapping_one(domain, drhd->iommu, bus, devfn,
                                         pdev);
        if ( ret )
            break;

        if ( (ret = find_upstream_bridge(seg, &bus, &devfn, &secbus)) < 1 )
        {
            if ( !ret )
                break;
            ret = -ENXIO;
        }

        /*
         * Mapping a bridge should, if anything, pass the struct pci_dev of
         * that bridge. Since bridges don't normally get assigned to guests,
         * their owner would be the wrong one. Pass NULL instead.
         */
        if ( ret >= 0 )
            ret = domain_context_mapping_one(domain, drhd->iommu, bus, devfn,
                                             NULL);

        /*
         * Devices behind PCIe-to-PCI/PCIx bridge may generate different
         * requester-id. It may originate from devfn=0 on the secondary bus
         * behind the bridge. Map that id as well if we didn't already.
         *
         * Somewhat similar as for bridges, we don't want to pass a struct
         * pci_dev here - there may not even exist one for this (secbus,0,0)
         * tuple. If there is one, without properly working device groups it
         * may again not have the correct owner.
         */
        if ( !ret && pdev_type(seg, bus, devfn) == DEV_TYPE_PCIe2PCI_BRIDGE &&
             (secbus != pdev->bus || pdev->devfn != 0) )
            ret = domain_context_mapping_one(domain, drhd->iommu, secbus, 0,
                                             NULL);

        if ( ret )
            domain_context_unmap(domain, devfn, pdev);

        break;

    default:
        dprintk(XENLOG_ERR VTDPREFIX, "%pd:unknown(%u): %pp\n",
                domain, pdev->type, &PCI_SBDF3(seg, bus, devfn));
        ret = -EINVAL;
        break;
    }

    if ( !ret && devfn == pdev->devfn )
        pci_vtd_quirk(pdev);

    return ret;
}

int domain_context_unmap_one(
    struct domain *domain,
    struct vtd_iommu *iommu,
    u8 bus, u8 devfn)
{
    struct context_entry *context, *context_entries;
    u64 maddr;
    int iommu_domid, rc, ret;
    bool_t flush_dev_iotlb;

    if ( QUARANTINE_SKIP(domain) )
        return 0;

    ASSERT(pcidevs_locked());
    spin_lock(&iommu->lock);

    maddr = bus_to_context_maddr(iommu, bus);
    context_entries = (struct context_entry *)map_vtd_domain_page(maddr);
    context = &context_entries[devfn];

    if ( !context_present(*context) )
    {
        spin_unlock(&iommu->lock);
        unmap_vtd_domain_page(context_entries);
        return 0;
    }

    context_clear_present(*context);
    context_clear_entry(*context);
    iommu_sync_cache(context, sizeof(struct context_entry));

    iommu_domid= domain_iommu_domid(domain, iommu);
    if ( iommu_domid == -1 )
    {
        spin_unlock(&iommu->lock);
        unmap_vtd_domain_page(context_entries);
        return -EINVAL;
    }

    rc = iommu_flush_context_device(iommu, iommu_domid,
                                    PCI_BDF2(bus, devfn),
                                    DMA_CCMD_MASK_NOBIT, 0);

    flush_dev_iotlb = !!find_ats_dev_drhd(iommu);
    ret = iommu_flush_iotlb_dsi(iommu, iommu_domid, 0, flush_dev_iotlb);

    /*
     * The current logic for returns:
     *   - positive  invoke iommu_flush_write_buffer to flush cache.
     *   - zero      on success.
     *   - negative  on failure. Continue to flush IOMMU IOTLB on a
     *               best effort basis.
     */
    if ( rc > 0 || ret > 0 )
        iommu_flush_write_buffer(iommu);
    if ( rc >= 0 )
        rc = ret;
    if ( rc > 0 )
        rc = 0;

    spin_unlock(&iommu->lock);
    unmap_vtd_domain_page(context_entries);

    if ( !iommu->drhd->segment && !rc )
        rc = me_wifi_quirk(domain, bus, devfn, UNMAP_ME_PHANTOM_FUNC);

    if ( rc && !is_hardware_domain(domain) && domain != dom_io )
    {
        if ( domain->is_dying )
        {
            printk(XENLOG_ERR "%pd: error %d unmapping %04x:%02x:%02x.%u\n",
                   domain, rc, iommu->drhd->segment, bus,
                   PCI_SLOT(devfn), PCI_FUNC(devfn));
            rc = 0; /* Make upper layers continue in a best effort manner. */
        }
        else
            domain_crash(domain);
    }

    return rc;
}

static int domain_context_unmap(struct domain *domain, u8 devfn,
                                struct pci_dev *pdev)
{
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;
    int ret;
    u8 seg = pdev->seg, bus = pdev->bus, tmp_bus, tmp_devfn, secbus;
    int found = 0;

    drhd = acpi_find_matched_drhd_unit(pdev);
    if ( !drhd )
        return -ENODEV;
    iommu = drhd->iommu;

    switch ( pdev->type )
    {
    case DEV_TYPE_PCI_HOST_BRIDGE:
        if ( iommu_debug )
            printk(VTDPREFIX "%pd:Hostbridge: skip %pp unmap\n",
                   domain, &PCI_SBDF3(seg, bus, devfn));
        return is_hardware_domain(domain) ? 0 : -EPERM;

    case DEV_TYPE_PCIe_BRIDGE:
    case DEV_TYPE_PCIe2PCI_BRIDGE:
    case DEV_TYPE_LEGACY_PCI_BRIDGE:
        return 0;

    case DEV_TYPE_PCIe_ENDPOINT:
        if ( iommu_debug )
            printk(VTDPREFIX "%pd:PCIe: unmap %pp\n",
                   domain, &PCI_SBDF3(seg, bus, devfn));
        ret = domain_context_unmap_one(domain, iommu, bus, devfn);
        if ( !ret && devfn == pdev->devfn && ats_device(pdev, drhd) > 0 )
            disable_ats_device(pdev);

        break;

    case DEV_TYPE_PCI:
        if ( iommu_debug )
            printk(VTDPREFIX "%pd:PCI: unmap %pp\n",
                   domain, &PCI_SBDF3(seg, bus, devfn));
        ret = domain_context_unmap_one(domain, iommu, bus, devfn);
        if ( ret )
            break;

        tmp_bus = bus;
        tmp_devfn = devfn;
        if ( (ret = find_upstream_bridge(seg, &tmp_bus, &tmp_devfn,
                                         &secbus)) < 1 )
        {
            if ( ret )
            {
                ret = -ENXIO;
                if ( !domain->is_dying &&
                     !is_hardware_domain(domain) && domain != dom_io )
                {
                    domain_crash(domain);
                    /* Make upper layers continue in a best effort manner. */
                    ret = 0;
                }
            }
            break;
        }

        /* PCIe to PCI/PCIx bridge */
        if ( pdev_type(seg, tmp_bus, tmp_devfn) == DEV_TYPE_PCIe2PCI_BRIDGE )
        {
            ret = domain_context_unmap_one(domain, iommu, tmp_bus, tmp_devfn);
            if ( !ret )
                ret = domain_context_unmap_one(domain, iommu, secbus, 0);
        }
        else /* Legacy PCI bridge */
            ret = domain_context_unmap_one(domain, iommu, tmp_bus, tmp_devfn);

        break;

    default:
        dprintk(XENLOG_ERR VTDPREFIX, "%pd:unknown(%u): %pp\n",
                domain, pdev->type, &PCI_SBDF3(seg, bus, devfn));
        return -EINVAL;
    }

    if ( ret || QUARANTINE_SKIP(domain) )
        return ret;

    /*
     * if no other devices under the same iommu owned by this domain,
     * clear iommu in iommu_bitmap and clear domain_id in domid_bitmp
     */
    for_each_pdev ( domain, pdev )
    {
        if ( pdev->seg == seg && pdev->bus == bus && pdev->devfn == devfn )
            continue;

        drhd = acpi_find_matched_drhd_unit(pdev);
        if ( drhd && drhd->iommu == iommu )
        {
            found = 1;
            break;
        }
    }

    if ( found == 0 )
    {
        clear_bit(iommu->index, &dom_iommu(domain)->arch.vtd.iommu_bitmap);
        cleanup_domid_map(domain, iommu);
    }

    return 0;
}

static void iommu_clear_root_pgtable(struct domain *d)
{
    struct domain_iommu *hd = dom_iommu(d);

    spin_lock(&hd->arch.mapping_lock);
    hd->arch.vtd.pgd_maddr = 0;
    spin_unlock(&hd->arch.mapping_lock);
}

static void iommu_domain_teardown(struct domain *d)
{
    struct domain_iommu *hd = dom_iommu(d);
    const struct acpi_drhd_unit *drhd;

    if ( list_empty(&acpi_drhd_units) )
        return;

    iommu_identity_map_teardown(d);

    ASSERT(!hd->arch.vtd.pgd_maddr);

    for_each_drhd_unit ( drhd )
        cleanup_domid_map(d, drhd->iommu);
}

static int __must_check intel_iommu_map_page(struct domain *d, dfn_t dfn,
                                             mfn_t mfn, unsigned int flags,
                                             unsigned int *flush_flags)
{
    struct domain_iommu *hd = dom_iommu(d);
    struct dma_pte *page, *pte, old, new = {};
    u64 pg_maddr;
    int rc = 0;

    /* Do nothing if VT-d shares EPT page table */
    if ( iommu_use_hap_pt(d) )
        return 0;

    /* Do nothing if hardware domain and iommu supports pass thru. */
    if ( iommu_hwdom_passthrough && is_hardware_domain(d) )
        return 0;

    spin_lock(&hd->arch.mapping_lock);

    /*
     * IOMMU mapping request can be safely ignored when the domain is dying.
     *
     * hd->arch.mapping_lock guarantees that d->is_dying will be observed
     * before any page tables are freed (see iommu_free_pgtables())
     */
    if ( d->is_dying )
    {
        spin_unlock(&hd->arch.mapping_lock);
        return 0;
    }

    pg_maddr = addr_to_dma_page_maddr(d, dfn_to_daddr(dfn), 1);
    if ( !pg_maddr )
    {
        spin_unlock(&hd->arch.mapping_lock);
        return -ENOMEM;
    }

    page = (struct dma_pte *)map_vtd_domain_page(pg_maddr);
    pte = &page[dfn_x(dfn) & LEVEL_MASK];
    old = *pte;

    dma_set_pte_addr(new, mfn_to_maddr(mfn));
    dma_set_pte_prot(new,
                     ((flags & IOMMUF_readable) ? DMA_PTE_READ  : 0) |
                     ((flags & IOMMUF_writable) ? DMA_PTE_WRITE : 0));

    /* Set the SNP on leaf page table if Snoop Control available */
    if ( iommu_snoop )
        dma_set_pte_snp(new);

    if ( old.val == new.val )
    {
        spin_unlock(&hd->arch.mapping_lock);
        unmap_vtd_domain_page(page);
        return 0;
    }

    *pte = new;

    iommu_sync_cache(pte, sizeof(struct dma_pte));
    spin_unlock(&hd->arch.mapping_lock);
    unmap_vtd_domain_page(page);

    *flush_flags |= IOMMU_FLUSHF_added;
    if ( dma_pte_present(old) )
        *flush_flags |= IOMMU_FLUSHF_modified;

    return rc;
}

static int __must_check intel_iommu_unmap_page(struct domain *d, dfn_t dfn,
                                               unsigned int *flush_flags)
{
    /* Do nothing if VT-d shares EPT page table */
    if ( iommu_use_hap_pt(d) )
        return 0;

    /* Do nothing if hardware domain and iommu supports pass thru. */
    if ( iommu_hwdom_passthrough && is_hardware_domain(d) )
        return 0;

    dma_pte_clear_one(d, dfn_to_daddr(dfn), flush_flags);

    return 0;
}

static int intel_iommu_lookup_page(struct domain *d, dfn_t dfn, mfn_t *mfn,
                                   unsigned int *flags)
{
    struct domain_iommu *hd = dom_iommu(d);
    struct dma_pte *page, val;
    u64 pg_maddr;

    /*
     * If VT-d shares EPT page table or if the domain is the hardware
     * domain and iommu_passthrough is set then pass back the dfn.
     */
    if ( iommu_use_hap_pt(d) ||
         (iommu_hwdom_passthrough && is_hardware_domain(d)) )
        return -EOPNOTSUPP;

    spin_lock(&hd->arch.mapping_lock);

    pg_maddr = addr_to_dma_page_maddr(d, dfn_to_daddr(dfn), 0);
    if ( !pg_maddr )
    {
        spin_unlock(&hd->arch.mapping_lock);
        return -ENOENT;
    }

    page = map_vtd_domain_page(pg_maddr);
    val = page[dfn_x(dfn) & LEVEL_MASK];

    unmap_vtd_domain_page(page);
    spin_unlock(&hd->arch.mapping_lock);

    if ( !dma_pte_present(val) )
        return -ENOENT;

    *mfn = maddr_to_mfn(dma_pte_addr(val));
    *flags = dma_pte_read(val) ? IOMMUF_readable : 0;
    *flags |= dma_pte_write(val) ? IOMMUF_writable : 0;

    return 0;
}

static int __init vtd_ept_page_compatible(struct vtd_iommu *iommu)
{
    u64 ept_cap, vtd_cap = iommu->cap;

    /* EPT is not initialised yet, so we must check the capability in
     * the MSR explicitly rather than use cpu_has_vmx_ept_*() */
    if ( rdmsr_safe(MSR_IA32_VMX_EPT_VPID_CAP, ept_cap) != 0 ) 
        return 0;

    return (ept_has_2mb(ept_cap) && opt_hap_2mb) <= cap_sps_2mb(vtd_cap) &&
           (ept_has_1gb(ept_cap) && opt_hap_1gb) <= cap_sps_1gb(vtd_cap);
}

static int intel_iommu_add_device(u8 devfn, struct pci_dev *pdev)
{
    struct acpi_rmrr_unit *rmrr;
    u16 bdf;
    int ret, i;

    ASSERT(pcidevs_locked());

    if ( !pdev->domain )
        return -EINVAL;

    ret = domain_context_mapping(pdev->domain, devfn, pdev);
    if ( ret )
    {
        dprintk(XENLOG_ERR VTDPREFIX, "d%d: context mapping failed\n",
                pdev->domain->domain_id);
        return ret;
    }

    for_each_rmrr_device ( rmrr, bdf, i )
    {
        if ( rmrr->segment == pdev->seg &&
             PCI_BUS(bdf) == pdev->bus &&
             PCI_DEVFN2(bdf) == devfn )
        {
            /*
             * iommu_add_device() is only called for the hardware
             * domain (see xen/drivers/passthrough/pci.c:pci_add_device()).
             * Since RMRRs are always reserved in the e820 map for the hardware
             * domain, there shouldn't be a conflict.
             */
            ret = iommu_identity_mapping(pdev->domain, p2m_access_rw,
                                         rmrr->base_address, rmrr->end_address,
                                         0);
            if ( ret )
                dprintk(XENLOG_ERR VTDPREFIX, "d%d: RMRR mapping failed\n",
                        pdev->domain->domain_id);
        }
    }

    return 0;
}

static int intel_iommu_enable_device(struct pci_dev *pdev)
{
    struct acpi_drhd_unit *drhd = acpi_find_matched_drhd_unit(pdev);
    int ret = drhd ? ats_device(pdev, drhd) : -ENODEV;

    pci_vtd_quirk(pdev);

    if ( ret <= 0 )
        return ret;

    ret = enable_ats_device(pdev, &drhd->iommu->ats_devices);

    return ret >= 0 ? 0 : ret;
}

static int intel_iommu_remove_device(u8 devfn, struct pci_dev *pdev)
{
    struct acpi_rmrr_unit *rmrr;
    u16 bdf;
    int i;

    if ( !pdev->domain )
        return -EINVAL;

    for_each_rmrr_device ( rmrr, bdf, i )
    {
        if ( rmrr->segment != pdev->seg ||
             PCI_BUS(bdf) != pdev->bus ||
             PCI_DEVFN2(bdf) != devfn )
            continue;

        /*
         * Any flag is nothing to clear these mappings but here
         * its always safe and strict to set 0.
         */
        iommu_identity_mapping(pdev->domain, p2m_access_x, rmrr->base_address,
                               rmrr->end_address, 0);
    }

    return domain_context_unmap(pdev->domain, devfn, pdev);
}

static int __hwdom_init setup_hwdom_device(u8 devfn, struct pci_dev *pdev)
{
    return domain_context_mapping(pdev->domain, devfn, pdev);
}

void clear_fault_bits(struct vtd_iommu *iommu)
{
    unsigned long flags;

    spin_lock_irqsave(&iommu->register_lock, flags);

    if ( dmar_readl(iommu->reg, DMAR_FSTS_REG) & DMA_FSTS_PPF )
    {
        unsigned int reg = cap_fault_reg_offset(iommu->cap);
        unsigned int end = reg + cap_num_fault_regs(iommu->cap);

        do {
           dmar_writel(iommu->reg, reg + 12, DMA_FRCD_F);
           reg += PRIMARY_FAULT_REG_LEN;
        } while ( reg < end );
    }

    dmar_writel(iommu->reg, DMAR_FSTS_REG, DMA_FSTS_FAULTS);

    spin_unlock_irqrestore(&iommu->register_lock, flags);
}

static void adjust_irq_affinity(struct acpi_drhd_unit *drhd)
{
    const struct acpi_rhsa_unit *rhsa = drhd_to_rhsa(drhd);
    unsigned int node = rhsa ? pxm_to_node(rhsa->proximity_domain)
                             : NUMA_NO_NODE;
    const cpumask_t *cpumask = NULL;
    struct irq_desc *desc;
    unsigned long flags;

    if ( node < MAX_NUMNODES && node_online(node) &&
         cpumask_intersects(&node_to_cpumask(node), &cpu_online_map) )
        cpumask = &node_to_cpumask(node);

    desc = irq_to_desc(drhd->iommu->msi.irq);
    spin_lock_irqsave(&desc->lock, flags);
    dma_msi_set_affinity(desc, cpumask);
    spin_unlock_irqrestore(&desc->lock, flags);
}

static int adjust_vtd_irq_affinities(void)
{
    struct acpi_drhd_unit *drhd;

    if ( !iommu_enabled )
        return 0;

    for_each_drhd_unit ( drhd )
        adjust_irq_affinity(drhd);

    return 0;
}
__initcall(adjust_vtd_irq_affinities);

static int __must_check init_vtd_hw(bool resume)
{
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;
    int ret;
    unsigned long flags;
    u32 sts;

    /*
     * Basic VT-d HW init: set VT-d interrupt, clear VT-d faults, etc.
     */
    for_each_drhd_unit ( drhd )
    {
        adjust_irq_affinity(drhd);

        iommu = drhd->iommu;

        clear_fault_bits(iommu);

        /*
         * Disable interrupt remapping and queued invalidation if
         * already enabled by BIOS in case we've not initialized it yet.
         */
        if ( !iommu_x2apic_enabled )
        {
            disable_intremap(iommu);
            disable_qinval(iommu);
        }

        if ( resume )
            /* FECTL write done by vtd_resume(). */
            continue;

        spin_lock_irqsave(&iommu->register_lock, flags);
        sts = dmar_readl(iommu->reg, DMAR_FECTL_REG);
        sts &= ~DMA_FECTL_IM;
        dmar_writel(iommu->reg, DMAR_FECTL_REG, sts);
        spin_unlock_irqrestore(&iommu->register_lock, flags);
    }

    /*
     * Enable queue invalidation
     */   
    for_each_drhd_unit ( drhd )
    {
        iommu = drhd->iommu;
        /*
         * If queued invalidation not enabled, use regiser based
         * invalidation
         */
        if ( enable_qinval(iommu) != 0 )
        {
            /* Ensure register-based invalidation is available */
            if ( !has_register_based_invalidation(iommu) )
                return -EIO;

            iommu->flush.context = vtd_flush_context_reg;
            iommu->flush.iotlb   = vtd_flush_iotlb_reg;
        }
    }

    /*
     * Enable interrupt remapping
     */  
    if ( iommu_intremap )
    {
        int apic;
        for ( apic = 0; apic < nr_ioapics; apic++ )
        {
            if ( ioapic_to_iommu(IO_APIC_ID(apic)) == NULL )
            {
                iommu_intremap = iommu_intremap_off;
                dprintk(XENLOG_ERR VTDPREFIX,
                    "ioapic_to_iommu: ioapic %#x (id: %#x) is NULL! "
                    "Will not try to enable Interrupt Remapping.\n",
                    apic, IO_APIC_ID(apic));
                break;
            }
        }
    }
    if ( iommu_intremap )
    {
        for_each_drhd_unit ( drhd )
        {
            iommu = drhd->iommu;
            if ( enable_intremap(iommu, 0) != 0 )
            {
                iommu_intremap = iommu_intremap_off;
                dprintk(XENLOG_WARNING VTDPREFIX,
                        "Interrupt Remapping not enabled\n");

                break;
            }
        }
        if ( !iommu_intremap )
            for_each_drhd_unit ( drhd )
                disable_intremap(drhd->iommu);
    }

    /*
     * Set root entries for each VT-d engine.  After set root entry,
     * must globally invalidate context cache, and then globally
     * invalidate IOTLB
     */
    for_each_drhd_unit ( drhd )
    {
        iommu = drhd->iommu;
        ret = iommu_set_root_entry(iommu);
        if ( ret )
        {
            dprintk(XENLOG_ERR VTDPREFIX, "IOMMU: set root entry failed\n");
            return -EIO;
        }
    }

    return iommu_flush_all();
}

static void __hwdom_init setup_hwdom_rmrr(struct domain *d)
{
    struct acpi_rmrr_unit *rmrr;
    u16 bdf;
    int ret, i;

    pcidevs_lock();
    for_each_rmrr_device ( rmrr, bdf, i )
    {
        /*
         * Here means we're add a device to the hardware domain.
         * Since RMRRs are always reserved in the e820 map for the hardware
         * domain, there shouldn't be a conflict. So its always safe and
         * strict to set 0.
         */
        ret = iommu_identity_mapping(d, p2m_access_rw, rmrr->base_address,
                                     rmrr->end_address, 0);
        if ( ret )
            dprintk(XENLOG_ERR VTDPREFIX,
                     "IOMMU: mapping reserved region failed\n");
    }
    pcidevs_unlock();
}

static struct iommu_state {
    uint32_t fectl;
} *__read_mostly iommu_state;

static int __init vtd_setup(void)
{
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;
    int ret;
    bool reg_inval_supported = true;

    if ( list_empty(&acpi_drhd_units) )
    {
        ret = -ENODEV;
        goto error;
    }

    if ( unlikely(acpi_gbl_FADT.boot_flags & ACPI_FADT_NO_MSI) )
    {
        ret = -EPERM;
        goto error;
    }

    platform_quirks_init();
    if ( !iommu_enable )
    {
        ret = -ENODEV;
        goto error;
    }

    iommu_state = xmalloc_array(struct iommu_state, nr_iommus);
    if ( !iommu_state )
    {
        ret = -ENOMEM;
        goto error;
    }

    /* We enable the following features only if they are supported by all VT-d
     * engines: Snoop Control, DMA passthrough, Register-based Invalidation,
     * Queued Invalidation, Interrupt Remapping, and Posted Interrupt.
     */
    for_each_drhd_unit ( drhd )
    {
        iommu = drhd->iommu;

        printk("Intel VT-d iommu %u supported page sizes: 4kB%s%s\n",
               iommu->index,
               cap_sps_2mb(iommu->cap) ? ", 2MB" : "",
               cap_sps_1gb(iommu->cap) ? ", 1GB" : "");

#ifndef iommu_snoop
        if ( iommu_snoop && !ecap_snp_ctl(iommu->ecap) )
            iommu_snoop = false;
#endif

        if ( iommu_hwdom_passthrough && !ecap_pass_thru(iommu->ecap) )
            iommu_hwdom_passthrough = false;

        if ( iommu_qinval && !ecap_queued_inval(iommu->ecap) )
            iommu_qinval = 0;

        if ( !has_register_based_invalidation(iommu) )
            reg_inval_supported = false;

        if ( iommu_intremap && !ecap_intr_remap(iommu->ecap) )
            iommu_intremap = iommu_intremap_off;

#ifndef iommu_intpost
        /*
         * We cannot use posted interrupt if X86_FEATURE_CX16 is
         * not supported, since we count on this feature to
         * atomically update 16-byte IRTE in posted format.
         */
        if ( !cap_intr_post(iommu->cap) || !iommu_intremap || !cpu_has_cx16 )
            iommu_intpost = false;
#endif

        if ( !vtd_ept_page_compatible(iommu) )
            clear_iommu_hap_pt_share();

        ret = iommu_set_interrupt(drhd);
        if ( ret )
        {
            dprintk(XENLOG_ERR VTDPREFIX, "IOMMU: interrupt setup failed\n");
            goto error;
        }
    }

    softirq_tasklet_init(&vtd_fault_tasklet, do_iommu_page_fault, NULL);

    if ( !iommu_qinval && !reg_inval_supported )
    {
        dprintk(XENLOG_ERR VTDPREFIX, "No available invalidation interface\n");
        ret = -ENODEV;
        goto error;
    }

    if ( !iommu_qinval && iommu_intremap )
    {
        iommu_intremap = iommu_intremap_off;
        dprintk(XENLOG_WARNING VTDPREFIX, "Interrupt Remapping disabled "
            "since Queued Invalidation isn't supported or enabled.\n");
    }

#define P(p,s) printk("Intel VT-d %s %senabled.\n", s, (p)? "" : "not ")
#ifndef iommu_snoop
    P(iommu_snoop, "Snoop Control");
#endif
    P(iommu_hwdom_passthrough, "Dom0 DMA Passthrough");
    P(iommu_qinval, "Queued Invalidation");
    P(iommu_intremap, "Interrupt Remapping");
#ifndef iommu_intpost
    P(iommu_intpost, "Posted Interrupt");
#endif
    P(iommu_hap_pt_share, "Shared EPT tables");
#undef P

    ret = init_vtd_hw(false);
    if ( ret )
        goto error;

    register_keyhandler('V', vtd_dump_iommu_info, "dump iommu info", 1);

    return 0;

 error:
    iommu_enabled = 0;
#ifndef iommu_snoop
    iommu_snoop = false;
#endif
    iommu_hwdom_passthrough = false;
    iommu_qinval = 0;
    iommu_intremap = iommu_intremap_off;
#ifndef iommu_intpost
    iommu_intpost = false;
#endif
    return ret;
}

static int reassign_device_ownership(
    struct domain *source,
    struct domain *target,
    u8 devfn, struct pci_dev *pdev)
{
    int ret;

    /*
     * Devices assigned to untrusted domains (here assumed to be any domU)
     * can attempt to send arbitrary LAPIC/MSI messages. We are unprotected
     * by the root complex unless interrupt remapping is enabled.
     */
    if ( (target != hardware_domain) && !iommu_intremap )
        untrusted_msi = true;

    /*
     * If the device belongs to the hardware domain, and it has RMRR, don't
     * remove it from the hardware domain, because BIOS may use RMRR at
     * booting time.
     */
    if ( !is_hardware_domain(source) )
    {
        const struct acpi_rmrr_unit *rmrr;
        u16 bdf;
        unsigned int i;

        for_each_rmrr_device( rmrr, bdf, i )
            if ( rmrr->segment == pdev->seg &&
                 PCI_BUS(bdf) == pdev->bus &&
                 PCI_DEVFN2(bdf) == devfn )
            {
                /*
                 * Any RMRR flag is always ignored when remove a device,
                 * but its always safe and strict to set 0.
                 */
                ret = iommu_identity_mapping(source, p2m_access_x,
                                             rmrr->base_address,
                                             rmrr->end_address, 0);
                if ( ret != -ENOENT )
                    return ret;
            }
    }

    ret = domain_context_unmap(source, devfn, pdev);
    if ( ret )
        return ret;

    if ( devfn == pdev->devfn && pdev->domain != dom_io )
    {
        list_move(&pdev->domain_list, &dom_io->pdev_list);
        pdev->domain = dom_io;
    }

    if ( !has_arch_pdevs(source) )
        vmx_pi_hooks_deassign(source);

    if ( !has_arch_pdevs(target) )
        vmx_pi_hooks_assign(target);

    ret = domain_context_mapping(target, devfn, pdev);
    if ( ret )
    {
        if ( !has_arch_pdevs(target) )
            vmx_pi_hooks_deassign(target);

        return ret;
    }

    if ( devfn == pdev->devfn && pdev->domain != target )
    {
        list_move(&pdev->domain_list, &target->pdev_list);
        pdev->domain = target;
    }

    return ret;
}

static int intel_iommu_assign_device(
    struct domain *d, u8 devfn, struct pci_dev *pdev, u32 flag)
{
    struct domain *s = pdev->domain;
    struct acpi_rmrr_unit *rmrr;
    int ret = 0, i;
    u16 bdf, seg;
    u8 bus;

    if ( list_empty(&acpi_drhd_units) )
        return -ENODEV;

    seg = pdev->seg;
    bus = pdev->bus;
    /*
     * In rare cases one given rmrr is shared by multiple devices but
     * obviously this would put the security of a system at risk. So
     * we would prevent from this sort of device assignment. But this
     * can be permitted if user set
     *      "pci = [ 'sbdf, rdm_policy=relaxed' ]"
     *
     * TODO: in the future we can introduce group device assignment
     * interface to make sure devices sharing RMRR are assigned to the
     * same domain together.
     */
    for_each_rmrr_device( rmrr, bdf, i )
    {
        if ( rmrr->segment == seg &&
             PCI_BUS(bdf) == bus &&
             PCI_DEVFN2(bdf) == devfn &&
             rmrr->scope.devices_cnt > 1 )
        {
            bool_t relaxed = !!(flag & XEN_DOMCTL_DEV_RDM_RELAXED);

            printk(XENLOG_GUEST "%s" VTDPREFIX
                   " It's %s to assign %pp"
                   " with shared RMRR at %"PRIx64" for %pd.\n",
                   relaxed ? XENLOG_WARNING : XENLOG_ERR,
                   relaxed ? "risky" : "disallowed",
                   &PCI_SBDF3(seg, bus, devfn), rmrr->base_address, d);
            if ( !relaxed )
                return -EPERM;
        }
    }

    ret = reassign_device_ownership(s, d, devfn, pdev);
    if ( ret || d == dom_io )
        return ret;

    /* Setup rmrr identity mapping */
    for_each_rmrr_device( rmrr, bdf, i )
    {
        if ( rmrr->segment == seg &&
             PCI_BUS(bdf) == bus &&
             PCI_DEVFN2(bdf) == devfn )
        {
            ret = iommu_identity_mapping(d, p2m_access_rw, rmrr->base_address,
                                         rmrr->end_address, flag);
            if ( ret )
            {
                int rc;

                rc = reassign_device_ownership(d, s, devfn, pdev);
                printk(XENLOG_G_ERR VTDPREFIX
                       " cannot map reserved region (%"PRIx64",%"PRIx64"] for Dom%d (%d)\n",
                       rmrr->base_address, rmrr->end_address,
                       d->domain_id, ret);
                if ( rc )
                {
                    printk(XENLOG_ERR VTDPREFIX
                           " failed to reclaim %pp from %pd (%d)\n",
                           &PCI_SBDF3(seg, bus, devfn), d, rc);
                    domain_crash(d);
                }
                break;
            }
        }
    }

    return ret;
}

static int intel_iommu_group_id(u16 seg, u8 bus, u8 devfn)
{
    u8 secbus;
    if ( find_upstream_bridge(seg, &bus, &devfn, &secbus) < 0 )
        return -1;
    else
        return PCI_BDF2(bus, devfn);
}

static int __must_check vtd_suspend(void)
{
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;
    u32    i;
    int rc;

    if ( !iommu_enabled )
        return 0;

    rc = iommu_flush_all();
    if ( unlikely(rc) )
    {
        printk(XENLOG_WARNING VTDPREFIX
               " suspend: IOMMU flush all failed: %d\n", rc);

        return rc;
    }

    for_each_drhd_unit ( drhd )
    {
        iommu = drhd->iommu;
        i = iommu->index;

        iommu_state[i].fectl = dmar_readl(iommu->reg, DMAR_FECTL_REG);

        /* don't disable VT-d engine when force_iommu is set. */
        if ( force_iommu )
            continue;

        iommu_disable_translation(iommu);

        /* If interrupt remapping is enabled, queued invalidation
         * will be disabled following interupt remapping disabling
         * in local apic suspend
         */
        if ( !iommu_intremap && iommu_qinval )
            disable_qinval(iommu);
    }

    return 0;
}

static void vtd_crash_shutdown(void)
{
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;

    if ( !iommu_enabled )
        return;

    if ( iommu_flush_all() )
        printk(XENLOG_WARNING VTDPREFIX
               " crash shutdown: IOMMU flush all failed\n");

    for_each_drhd_unit ( drhd )
    {
        iommu = drhd->iommu;
        iommu_disable_translation(iommu);
        disable_intremap(drhd->iommu);
        disable_qinval(drhd->iommu);
    }
}

static void vtd_resume(void)
{
    struct acpi_drhd_unit *drhd;
    struct vtd_iommu *iommu;
    u32 i;
    unsigned long flags;

    if ( !iommu_enabled )
        return;

    for_each_drhd_unit ( drhd )
    {
        iommu = drhd->iommu;

        spin_lock_irqsave(&iommu->register_lock, flags);
        dmar_writel(iommu->reg, DMAR_FEDATA_REG, iommu->msi.msg.data);
        dmar_writel(iommu->reg, DMAR_FEADDR_REG, iommu->msi.msg.address_lo);
        if ( x2apic_enabled )
            dmar_writel(iommu->reg, DMAR_FEUADDR_REG,
                        iommu->msi.msg.address_hi);
        spin_unlock_irqrestore(&iommu->register_lock, flags);
    }

    if ( init_vtd_hw(true) != 0 && force_iommu )
         panic("IOMMU setup failed, crash Xen for security purpose\n");

    for_each_drhd_unit ( drhd )
    {
        iommu = drhd->iommu;
        i = iommu->index;

        spin_lock_irqsave(&iommu->register_lock, flags);
        dmar_writel(iommu->reg, DMAR_FECTL_REG, iommu_state[i].fectl);
        spin_unlock_irqrestore(&iommu->register_lock, flags);

        iommu_enable_translation(drhd);
    }
}

static void vtd_dump_page_table_level(paddr_t pt_maddr, int level, paddr_t gpa,
                                      int indent)
{
    paddr_t address;
    int i;
    struct dma_pte *pt_vaddr, *pte;
    int next_level;

    if ( level < 1 )
        return;

    pt_vaddr = map_vtd_domain_page(pt_maddr);
    if ( pt_vaddr == NULL )
    {
        printk(VTDPREFIX " failed to map domain page %"PRIpaddr"\n",
               pt_maddr);
        return;
    }

    next_level = level - 1;
    for ( i = 0; i < PTE_NUM; i++ )
    {
        if ( !(i % 2) )
            process_pending_softirqs();

        pte = &pt_vaddr[i];
        if ( !dma_pte_present(*pte) )
            continue;

        address = gpa + offset_level_address(i, level);
        if ( next_level >= 1 ) 
            vtd_dump_page_table_level(dma_pte_addr(*pte), next_level,
                                      address, indent + 1);
        else
            printk("%*sdfn: %08lx mfn: %08lx\n",
                   indent, "",
                   (unsigned long)(address >> PAGE_SHIFT_4K),
                   (unsigned long)(dma_pte_addr(*pte) >> PAGE_SHIFT_4K));
    }

    unmap_vtd_domain_page(pt_vaddr);
}

static void vtd_dump_page_tables(struct domain *d)
{
    const struct domain_iommu *hd = dom_iommu(d);

    if ( iommu_use_hap_pt(d) )
    {
        printk(VTDPREFIX " %pd sharing EPT table\n", d);
        return;
    }

    printk(VTDPREFIX" %pd table has %d levels\n", d,
           agaw_to_level(hd->arch.vtd.agaw));
    vtd_dump_page_table_level(hd->arch.vtd.pgd_maddr,
                              agaw_to_level(hd->arch.vtd.agaw), 0, 0);
}

static int __init intel_iommu_quarantine_init(struct domain *d)
{
    struct domain_iommu *hd = dom_iommu(d);
    struct page_info *pg;
    struct dma_pte *parent;
    unsigned int agaw = width_to_agaw(DEFAULT_DOMAIN_ADDRESS_WIDTH);
    unsigned int level = agaw_to_level(agaw);
    int rc = 0;

    spin_lock(&hd->arch.mapping_lock);

    if ( hd->arch.vtd.pgd_maddr )
    {
        ASSERT_UNREACHABLE();
        goto out;
    }

    pg = iommu_alloc_pgtable(d);

    rc = -ENOMEM;
    if ( !pg )
        goto out;

    hd->arch.vtd.pgd_maddr = page_to_maddr(pg);

    parent = map_vtd_domain_page(hd->arch.vtd.pgd_maddr);
    while ( level )
    {
        uint64_t maddr;
        unsigned int offset;

        /*
         * The pgtable allocator is fine for the leaf page, as well as
         * page table pages, and the resulting allocations are always
         * zeroed.
         */
        pg = iommu_alloc_pgtable(d);

        if ( !pg )
            goto out;

        maddr = page_to_maddr(pg);
        for ( offset = 0; offset < PTE_NUM; offset++ )
        {
            struct dma_pte *pte = &parent[offset];

            dma_set_pte_addr(*pte, maddr);
            dma_set_pte_readable(*pte);
        }
        iommu_sync_cache(parent, PAGE_SIZE);

        unmap_vtd_domain_page(parent);
        parent = map_vtd_domain_page(maddr);
        level--;
    }
    unmap_vtd_domain_page(parent);

    rc = 0;

 out:
    spin_unlock(&hd->arch.mapping_lock);

    if ( !rc )
        rc = iommu_flush_iotlb_all(d);

    /* Pages may be leaked in failure case */
    return rc;
}

static struct iommu_ops __initdata vtd_ops = {
    .init = intel_iommu_domain_init,
    .hwdom_init = intel_iommu_hwdom_init,
    .quarantine_init = intel_iommu_quarantine_init,
    .add_device = intel_iommu_add_device,
    .enable_device = intel_iommu_enable_device,
    .remove_device = intel_iommu_remove_device,
    .assign_device  = intel_iommu_assign_device,
    .teardown = iommu_domain_teardown,
    .clear_root_pgtable = iommu_clear_root_pgtable,
    .map_page = intel_iommu_map_page,
    .unmap_page = intel_iommu_unmap_page,
    .lookup_page = intel_iommu_lookup_page,
    .reassign_device = reassign_device_ownership,
    .get_device_group_id = intel_iommu_group_id,
    .enable_x2apic = intel_iommu_enable_eim,
    .disable_x2apic = intel_iommu_disable_eim,
    .update_ire_from_apic = io_apic_write_remap_rte,
    .update_ire_from_msi = msi_msg_write_remap_rte,
    .read_apic_from_ire = io_apic_read_remap_rte,
    .setup_hpet_msi = intel_setup_hpet_msi,
    .adjust_irq_affinities = adjust_vtd_irq_affinities,
    .suspend = vtd_suspend,
    .resume = vtd_resume,
    .crash_shutdown = vtd_crash_shutdown,
    .iotlb_flush = iommu_flush_iotlb_pages,
    .iotlb_flush_all = iommu_flush_iotlb_all,
    .get_reserved_device_memory = intel_iommu_get_reserved_device_memory,
    .dump_page_tables = vtd_dump_page_tables,
};

const struct iommu_init_ops __initconstrel intel_iommu_init_ops = {
    .ops = &vtd_ops,
    .setup = vtd_setup,
    .supports_x2apic = intel_iommu_supports_eim,
};

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
