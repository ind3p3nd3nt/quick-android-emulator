/*
 *  Common CPU TLB handling
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/cputlb.h"
#include "opt/optimizations.h"

#include <stdio.h>
static inline large_page_t *find_and_remove_large_page(large_page_list_t *l,
                                                       target_ulong vaddr,
                                                       int mmu_idx)
{
    large_page_t *lp;
    target_ulong hash = large_page_hash_func(vaddr);
    large_page_t **p= &l->allocated[mmu_idx][hash];

    while ((lp = *p)) {
        if (lp->vaddr == (vaddr & lp->mask))
            break;
        p = &lp->next;
    }
    if (lp) *p = lp->next;
    return lp;
}

static inline large_page_t *new_large_page(large_page_list_t *l,
                                           target_ulong vaddr,
                                           target_ulong size,
                                           int mmu_idx,
                                           hwaddr paddr,
                                           int prot)
{
    /* we assume the large page (vaddr,size) is not in l->allocated. */
    large_page_t *lp;
    uint64_t mask;
    uint64_t size1 = size;
    uint64_t vaddr1;

    lp = pool_alloc(&l->large_page_pool);
    mask =  ~(size1 - 1);
    lp->paddr = paddr & mask;
    lp->prot = prot;
    vaddr1 = vaddr & mask;
    lp->vaddr = vaddr1;
    lp->mask = mask;
    lp->entry_list = NULL;
    target_ulong hash = large_page_hash_func(vaddr);
    large_page_t **list_head = &l->allocated[mmu_idx][hash];
    lp->next = *list_head;
    *list_head = lp;
     return lp;
}

static inline void free_large_page(CPUArchState *env, large_page_t *lp)
{
    const int exec_page = lp->prot | PAGE_EXEC;
    if (lp->entry_list) {
        /* flush all tlb entries of this large page */
        tlb_entry_t *te = lp->entry_list;
        while (te) {
            CPUTLBEntry *e = te->entry;
            e->addr_read = -1;
            e->addr_write = -1;
            if (exec_page) {
                target_ulong addr_code = e->addr_code;
                int i = tb_jmp_cache_hash_page(addr_code);
                memset(&env->tb_jmp_cache[i], 0,
                       TB_JMP_PAGE_SIZE * sizeof(TranslationBlock *));
                itlb_set_phy_page(env, addr_code, -1L);
            }
            e->addr_code = -1;
            te = te->next;
        }
        lp->entry_list = NULL;
    }
}

static inline void free_all_large_pages(large_page_list_t *l)
{
    memset(l->allocated, 0, sizeof(l->allocated));
    pool_reset(&l->large_page_pool);
    pool_reset(&l->tlb_entry_pool);
}

static inline void add_tlb_entry(large_page_list_t *l, large_page_t *p, CPUTLBEntry *e)
{
    tlb_entry_t *te = pool_alloc(&l->tlb_entry_pool);
    te->entry = e;
    /* insert te into the entry list of this large page */
    te->next = p->entry_list;
    p->entry_list = te;
}

/* Called when VCPU allocated */
void large_page_list_init(large_page_list_t *l)
{
    memset(l->allocated, 0, sizeof(l->allocated));
    pool_init(&l->large_page_pool, sizeof(large_page_t), 1<<17);
    pool_init(&l->tlb_entry_pool, sizeof(tlb_entry_t), 1<<20);
}

/* statistics */
int tlb_flush_count;

static const CPUTLBEntry s_cputlb_empty_entry = {
    .addr_read  = -1,
    .addr_write = -1,
    .addr_code  = -1,
    .addend     = -1,
};

/* hash table for tlb profiles */
#define TLB_PROFILE_HASH_BITS 12
#define TLB_PROFILE_HASH_MASK ((1 << TLB_PROFILE_HASH_BITS) - 1)
#define TLB_PROFILE_HASH_SIZE (1 << TLB_PROFILE_HASH_BITS)
static tlb_profile_t *tlb_profile_hash[TLB_PROFILE_HASH_SIZE];
static pool_t tlb_profile_pool;
static inline uint64_t tlb_profile_hash_func(uint64_t id)
{
    uint64_t hash = ((id >> 12) ^ (id >> (12 + TLB_PROFILE_HASH_BITS)));
    return  hash & TLB_PROFILE_HASH_MASK;
}

static inline tlb_profile_t *tlb_profile_allocate(uint64_t id)
{
    tlb_profile_t *profile = pool_alloc(&tlb_profile_pool);
    profile->id = id;
    profile->bits = DEFAULT_TLB_BITS;
    profile->next = NULL;
    return profile;
}

tlb_profile_t *find_or_create_tlb_profile(uint64_t id)
{
    uint64_t h;
    tlb_profile_t **p, *profile;

    h = tlb_profile_hash_func(id);
    p = &tlb_profile_hash[h];
    while ((profile = *p)) {
        if (profile->id == id)
            goto found;
        p = &profile->next;
    }
    /* not found, allocate one */
    if (!profile)
        profile = tlb_profile_allocate(id);
found:
    if (*p) *p = profile->next; /* remove from list */
    /* move profile to the first item of list */
    profile->next = tlb_profile_hash[h];
    tlb_profile_hash[h] = profile->next;
    return profile;
}

/* tlb_info stuff */
static inline void set_tlb_info(tlb_info_t *I, tlb_profile_t *profile)
{
    int cpu_tlb_size = 1 << profile->bits;
    I->profile = profile;

    I->nb_tlb_entries = cpu_tlb_size;
    I->tlb_table_size = cpu_tlb_size * sizeof(CPUTLBEntry);
    I->tlb_table_mask = (cpu_tlb_size - 1) * sizeof(CPUTLBEntry);
}

void init_tlb_info(void *env1)
{
    pool_init(&tlb_profile_pool, sizeof(tlb_profile_t), (1 << 16));
    CPUArchState *env = (CPUArchState*)env1;
    set_tlb_info(&env->tlb_info, tlb_profile_allocate(0));
    memset(env->tlb_table, -1, sizeof(env->tlb_table));
}

#define MAX_CONFLICT_MISS 10
static inline int find_max(int *A, int n)
{
    int *I, *E;
    int max = 0;
    for (I = A, E = A + n; I != E; ++I)
        if (*I > max)
            max = *I;
    return max;
}

static void update_tlb_info(CPUArchState *env)
{
    tlb_info_t *I = &env->tlb_info;
    tlb_profile_t *profile = I->profile;
    int bits = profile->bits;
    int nb_tlb_entries_used;
    int miss;
    uint64_t id;

    miss = find_max(profile->nb_conflict_misses, NB_MMU_MODES);
    if (bits < MAX_TLB_BITS && miss > MAX_CONFLICT_MISS) {
        profile->bits = bits + 1;
        goto done;
    }

    nb_tlb_entries_used = find_max(profile->nb_tlb_entries_used, NB_MMU_MODES);
    if (bits > MIN_TLB_BITS && nb_tlb_entries_used < (I->nb_tlb_entries >> 2)) {
        profile->bits = bits - 1;
        goto done;
    }
done:
    id = get_page_table(env);
    if (profile->id != id)
        profile = find_or_create_tlb_profile(id);
    set_tlb_info(I, profile);
}

/* NOTE:
 * If flush_global is true (the usual case), flush all tlb entries.
 * If flush_global is false, flush (at least) all tlb entries not
 * marked global.
 *
 * Since QEMU doesn't currently implement a global/not-global flag
 * for tlb entries, at the moment tlb_flush() will also flush all
 * tlb entries in the flush_global == false case. This is OK because
 * CPU architectures generally permit an implementation to drop
 * entries from the TLB at any time, so flushing more entries than
 * required is only an efficiency issue, not a correctness issue.
 */
void tlb_flush(CPUArchState *env, int flush_global)
{
    tlb_info_t *I = &env->tlb_info;
    tlb_profile_t *profile = I->profile;
    int i;

#if defined(DEBUG_TLB)
    printf("tlb_flush:\n");
#endif
    /* must reset current TB so that interrupts cannot modify the
       links while we are modifying them */
    env->current_tb = NULL;
    update_tlb_info(env);
    for (i = 0; i != NB_MMU_MODES; ++i) {
        memset(&env->tlb_table[i][0], -1, I->tlb_table_size);
        profile->nb_conflict_misses[i] = 0;
        profile->nb_tlb_entries_used[i] = 0;
    }
    memset(env->tb_jmp_cache, 0, TB_JMP_CACHE_SIZE * sizeof (void *));
    free_all_large_pages(&env->large_page_list);

    tlb_flush_count++;
#if defined(ITLB_ENABLE)
    itlb_reset(env);
#endif
}

static inline void tlb_flush_entry(CPUTLBEntry *tlb_entry, target_ulong addr)
{
    if (addr == (tlb_entry->addr_read &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_write &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_code &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        *tlb_entry = s_cputlb_empty_entry;
    }
}

void tlb_flush_page(CPUArchState *env, target_ulong addr)
{
    int i;
    int mmu_idx;

#if defined(DEBUG_TLB)
    printf("tlb_flush_page: " TARGET_FMT_lx "\n", addr);
#endif
    /* Check if we need to flush due to large pages.  */
    bool flag = false;
    large_page_t *lp;
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        lp = find_and_remove_large_page(&env->large_page_list, addr, mmu_idx);
        if (lp) {
            free_large_page(env, lp);
            flag = true;
        }
    }
    if (flag) {
        return ;
    }
    /* must reset current TB so that interrupts cannot modify the
       links while we are modifying them */
    env->current_tb = NULL;

    addr &= TARGET_PAGE_MASK;
    i = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE(env) - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        tlb_flush_entry(&env->tlb_table[mmu_idx][i], addr);
    }

    tb_flush_jmp_cache(env, addr);
}

/* update the TLBs so that writes to code in the virtual page 'addr'
   can be detected */
void tlb_protect_code(ram_addr_t ram_addr)
{
    cpu_physical_memory_reset_dirty(ram_addr,
                                    ram_addr + TARGET_PAGE_SIZE,
                                    CODE_DIRTY_FLAG);
}

/* update the TLB so that writes in physical page 'phys_addr' are no longer
   tested for self modifying code */
void tlb_unprotect_code_phys(CPUArchState *env, ram_addr_t ram_addr,
                             target_ulong vaddr)
{
    cpu_physical_memory_set_dirty_flags(ram_addr, CODE_DIRTY_FLAG);
}

static bool tlb_is_dirty_ram(CPUTLBEntry *tlbe)
{
    return (tlbe->addr_write & ~TARGET_PAGE_MASK) == IO_MEM_RAM;
}

void tlb_reset_dirty_range(CPUTLBEntry *tlb_entry, uintptr_t start,
                           uintptr_t length)
{
    uintptr_t addr;

    if (tlb_is_dirty_ram(tlb_entry)) {
        addr = (tlb_entry->addr_write & TARGET_PAGE_MASK) + tlb_entry->addend;
        if ((addr - start) < length) {
            tlb_entry->addr_write &= TARGET_PAGE_MASK;
            tlb_entry->addr_write |= TLB_NOTDIRTY;
        }
    }
}

static inline void tlb_set_dirty1(CPUTLBEntry *tlb_entry, target_ulong vaddr)
{
    if (tlb_entry->addr_write == (vaddr | TLB_NOTDIRTY)) {
        tlb_entry->addr_write = vaddr;
    }
}

/* update the TLB corresponding to virtual page vaddr
   so that it is no longer dirty */
void tlb_set_dirty(CPUArchState *env, target_ulong vaddr)
{
    int i;
    int mmu_idx;

    vaddr &= TARGET_PAGE_MASK;
    i = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE(env) - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        tlb_set_dirty1(&env->tlb_table[mmu_idx][i], vaddr);
    }
}

/* Our TLB does not support large pages, so remember the area covered by
   large pages and trigger a full TLB flush if these are invalidated.  */
static inline void tlb_add_large_page(CPUArchState *env, target_ulong vaddr,
                                      target_ulong size)
{
    target_ulong mask = ~(size - 1);

    if (env->tlb_flush_addr == (target_ulong)-1) {
        env->tlb_flush_addr = vaddr & mask;
        env->tlb_flush_mask = mask;
        return;
    }
    /* Extend the existing region to include the new page.
       This is a compromise between unnecessary flushes and the cost
       of maintaining a full variable size TLB.  */
    mask &= env->tlb_flush_mask;
    while (((env->tlb_flush_addr ^ vaddr) & mask) != 0) {
        mask <<= 1;
    }
    env->tlb_flush_addr &= mask;
    env->tlb_flush_mask = mask;
}

/* Add a new TLB entry. At most one entry for a given virtual address
   is permitted. Only a single TARGET_PAGE_SIZE region is mapped, the
   supplied size is only used by tlb_flush_page.  */
void tlb_set_page(CPUArchState *env, target_ulong vaddr,
                  hwaddr paddr, int prot,
                  int mmu_idx, target_ulong size)
{
    PhysPageDesc *p;
    unsigned long pd;
    unsigned int index;
    target_ulong address;
    target_ulong code_address;
    ptrdiff_t addend;
    CPUTLBEntry *te;
    CPUWatchpoint *wp;
    hwaddr iotlb;

    assert(size >= TARGET_PAGE_SIZE);
    p = phys_page_find(paddr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }
#if defined(DEBUG_TLB)
    printf("tlb_set_page: vaddr=" TARGET_FMT_lx " paddr=0x" TARGET_FMT_plx
           " prot=%x idx=%d pd=0x%08lx\n",
           vaddr, paddr, prot, mmu_idx, pd);
#endif

    address = vaddr;
    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM && !(pd & IO_MEM_ROMD)) {
        /* IO memory case (romd handled later) */
        address |= TLB_MMIO;
    }
    addend = (ptrdiff_t)qemu_get_ram_ptr(pd & TARGET_PAGE_MASK);
    if ((pd & ~TARGET_PAGE_MASK) <= IO_MEM_ROM) {
        /* Normal RAM.  */
        iotlb = pd & TARGET_PAGE_MASK;
        if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_RAM)
            iotlb |= IO_MEM_NOTDIRTY;
        else
            iotlb |= IO_MEM_ROM;
    } else {
        /* IO handlers are currently passed a physical address.
           It would be nice to pass an offset from the base address
           of that region.  This would avoid having to special case RAM,
           and avoid full address decoding in every device.
           We can't use the high bits of pd for this because
           IO_MEM_ROMD uses these as a ram address.  */
        iotlb = (pd & ~TARGET_PAGE_MASK);
        if (p) {
            iotlb += p->region_offset;
        } else {
            iotlb += paddr;
        }
    }

    code_address = address;
    /* Make accesses to pages with watchpoints go via the
       watchpoint trap routines.  */
    QTAILQ_FOREACH(wp, &env->watchpoints, entry) {
        if (vaddr == (wp->vaddr & TARGET_PAGE_MASK)) {
            iotlb = io_mem_watch + paddr;
            /* TODO: The memory case can be optimized by not trapping
               reads of pages with a write breakpoint.  */
            address |= TLB_MMIO;
        }
    }

    index = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE(env) - 1);
    env->iotlb[mmu_idx][index] = iotlb - vaddr;
    te = &env->tlb_table[mmu_idx][index];
    {
        target_ulong test = te->addr_read;
        test &= te->addr_write;
        test &= te->addr_code;
        if (test == -1)
            env->tlb_info.profile->nb_tlb_entries_used[mmu_idx]++;
    }
    te->addend = addend - vaddr;
    if (prot & PAGE_READ) {
        te->addr_read = address;
    } else {
        te->addr_read = -1;
    }

    if (prot & PAGE_EXEC) {
        te->addr_code = code_address;
    } else {
        te->addr_code = -1;
    }
    if (prot & PAGE_WRITE) {
        if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_ROM ||
            (pd & IO_MEM_ROMD)) {
            /* Write access calls the I/O callback.  */
            te->addr_write = address | TLB_MMIO;
        } else if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_RAM &&
                   !cpu_physical_memory_is_dirty(pd)) {
            te->addr_write = address | TLB_NOTDIRTY;
        } else {
            te->addr_write = address;
        }
    } else {
        te->addr_write = -1;
    }

    if (size != TARGET_PAGE_SIZE) {
        large_page_t *p;
        /* 1. find out whether this large page has been allocated */
        p = find_large_page(&env->large_page_list, vaddr, mmu_idx);
        if (!p) {
            p = new_large_page(&env->large_page_list, vaddr, size, mmu_idx, paddr, prot);
        }
        /* add CPUTLBEntry into this large page */
        add_tlb_entry(&env->large_page_list, p, te);
    }
}

/* NOTE: this function can trigger an exception */
/* NOTE2: the returned address is not exactly the physical address: it
   is the offset relative to phys_ram_base */
tb_page_addr_t get_page_addr_code(CPUArchState *env1, target_ulong addr)
{
    int mmu_idx, page_index, pd;
    void *p;

    page_index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE(env1) - 1);
    mmu_idx = cpu_mmu_index(env1);
    if (unlikely(env1->tlb_table[mmu_idx][page_index].addr_code !=
                 (addr & TARGET_PAGE_MASK))) {
        cpu_ldub_code(env1, addr);
    }
    pd = env1->tlb_table[mmu_idx][page_index].addr_code & ~TARGET_PAGE_MASK;
    if (pd > IO_MEM_ROM && !(pd & IO_MEM_ROMD)) {
#if defined(TARGET_SPARC) || defined(TARGET_MIPS)
        cpu_unassigned_access(env1, addr, 0, 1, 0, 4);
#else
        cpu_abort(env1, "Trying to execute code outside RAM or ROM at 0x" TARGET_FMT_lx "\n", addr);
#endif
    }
    p = (void *)((uintptr_t)addr + env1->tlb_table[mmu_idx][page_index].addend);
    return qemu_ram_addr_from_host_nofail(p);
}

#define MMUSUFFIX _cmmu
#define SOFTMMU_CODE_ACCESS

#define SHIFT 0
#include "exec/softmmu_template.h"

#define SHIFT 1
#include "exec/softmmu_template.h"

#define SHIFT 2
#include "exec/softmmu_template.h"

#define SHIFT 3
#include "exec/softmmu_template.h"
