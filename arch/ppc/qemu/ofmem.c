/*
 *   Creation Date: <1999/11/07 19:02:11 samuel>
 *   Time-stamp: <2004/01/07 19:42:36 samuel>
 *
 *	<ofmem.c>
 *
 *	OF Memory manager
 *
 *   Copyright (C) 1999-2004 Samuel Rydh (samuel@ibrium.se)
 *   Copyright (C) 2004 Stefan Reinauer
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation
 *
 */

#include "config.h"
#include "libopenbios/bindings.h"
#include "libc/string.h"
#include "libopenbios/ofmem.h"
#include "kernel.h"
#include "mmutypes.h"
#include "asm/processor.h"

#define BIT(n)		(1U << (31 - (n)))

/* called from assembly */
extern void dsi_exception(void);
extern void isi_exception(void);
extern void setup_mmu(unsigned long code_base);

/*
 * From Apple's BootX source comments:
 *
 *  96 MB map (currently unused - 4363357 tracks re-adoption)
 * 00000000 - 00003FFF  : Exception Vectors
 * 00004000 - 03FFFFFF  : Kernel Image, Boot Struct and Drivers (~64 MB)
 * 04000000 - 04FFFFFF  : File Load Area (16 MB)   [80 MB]
 * 05000000 - 053FFFFF  : FS Cache    (4 MB)       [84 MB]
 * 05400000 - 055FFFFF  : Malloc Zone (2 MB)       [86 MB]
 * 05600000 - 057FFFFF  : BootX Image (2 MB)       [88 MB]
 * 05800000 - 05FFFFFF  : Unused/OF   (8 MB)       [96 MB]
 *
 */

#define FREE_BASE		0x00004000
#define OF_CODE_START	0xfff00000UL
#define IO_BASE			0x80000000

#ifdef __powerpc64__
#define HASH_BITS		18
#else
#define HASH_BITS		15
#endif
#define HASH_SIZE		(2 << HASH_BITS)
#define OFMEM_SIZE		(2 * 1024 * 1024)

#define	SEGR_USER		BIT(2)
#define SEGR_BASE		0x0400

static inline unsigned long
get_hash_base(void)
{
    return (mfsdr1() & SDR1_HTABORG_MASK);
}

static inline unsigned long
get_rom_base(void)
{
    ofmem_t *ofmem = ofmem_arch_get_private();
    return ofmem->ramsize - 0x00100000;
}

unsigned long
get_ram_top(void)
{
    return get_hash_base() - (32 + 64 + 64) * 1024 - OFMEM_SIZE;
}

unsigned long
get_ram_bottom(void)
{
    return (unsigned long)FREE_BASE;
}

static ucell get_heap_top(void)
{
    return get_hash_base() - (32 + 64 + 64) * 1024;
}

static inline size_t ALIGN_SIZE(size_t x, size_t a)
{
    return (x + a - 1) & ~(a - 1);
}

ofmem_t* ofmem_arch_get_private(void)
{
    return (ofmem_t*)cell2pointer(get_heap_top() - OFMEM_SIZE);
}

void* ofmem_arch_get_malloc_base(void)
{
    return (char*)ofmem_arch_get_private() + ALIGN_SIZE(sizeof(ofmem_t), 4);
}

ucell ofmem_arch_get_heap_top(void)
{
    return get_heap_top();
}

ucell ofmem_arch_get_virt_top(void)
{
    return IO_BASE;
}

void ofmem_arch_unmap_pages(ucell virt, ucell size)
{
    /* kill page mappings in provided range */
}

void ofmem_arch_early_map_pages(ucell phys, ucell virt, ucell size, ucell mode)
{
    /* none yet */
}

retain_t *ofmem_arch_get_retained(void)
{
    /* not implemented */
    return NULL;
}

/* Return size of a single MMU package translation property entry in cells */
int ofmem_arch_get_translation_entry_size(void)
{
    return 4;
}

/* Generate translation property entry for PPC.
 * According to the platform bindings for PPC
 * (http://playground.sun.com/1275/bindings/ppc/release/ppc-2_1.html#REF34579)
 * a translation property entry has the following layout:
 *
 *      virtual address
 *      length
 *      physical address
 *      mode
 */
void ofmem_arch_create_translation_entry(ucell *transentry, translation_t *t)
{
    transentry[0] = t->virt;
    transentry[1] = t->size;
    transentry[2] = t->phys;
    transentry[3] = t->mode;
}

/************************************************************************/
/*	OF private allocations						*/
/************************************************************************/

void *
malloc(int size)
{
    return ofmem_malloc(size);
}

void
free(void *ptr)
{
    ofmem_free(ptr);
}

void *
realloc(void *ptr, size_t size)
{
    return ofmem_realloc(ptr, size);
}


/************************************************************************/
/*	misc								*/
/************************************************************************/

ucell ofmem_arch_default_translation_mode(ucell phys)
{
    /* XXX: Guard bit not set as it should! */
    if (phys < IO_BASE)
        return 0x02;	/*0xa*/	/* wim GxPp */
    return 0x6a;		/* WIm GxPp, I/O */
}


/************************************************************************/
/*	page fault handler						*/
/************************************************************************/

static ucell
ea_to_phys(ucell ea, ucell *mode)
{
    ucell phys;

    if (ea >= OF_CODE_START) {
        /* ROM into RAM */
        ea -= OF_CODE_START;
        phys = get_rom_base() + ea;
        *mode = 0x02;
		return phys;
    }

    phys = ofmem_translate(ea, mode);
    if (phys == -1) {
        phys = ea;
        *mode = ofmem_arch_default_translation_mode(phys);

        /* print_virt_range(); */
        /* print_phys_range(); */
        /* print_trans(); */
    }
    return phys;
}

static void
hash_page_64(ucell ea, ucell phys, ucell mode)
{
    static int next_grab_slot = 0;
    uint64_t vsid_mask, page_mask, pgidx, hash;
    uint64_t htab_mask, mask, avpn;
    unsigned long pgaddr;
    int i, found;
    unsigned int vsid, vsid_sh, sdr, sdr_sh, sdr_mask;
    mPTE_64_t *pp;

    vsid = (ea >> 28) + SEGR_BASE;
    vsid_sh = 7;
    vsid_mask = 0x00003FFFFFFFFF80ULL;
    sdr = mfsdr1();
    sdr_sh = 18;
    sdr_mask = 0x3FF80;
    page_mask = 0x0FFFFFFF; // XXX correct?
    pgidx = (ea & page_mask) >> PAGE_SHIFT;
    avpn = (vsid << 12) | ((pgidx >> 4) & 0x0F80);;

    hash = ((vsid ^ pgidx) << vsid_sh) & vsid_mask;
    htab_mask = 0x0FFFFFFF >> (28 - (sdr & 0x1F));
    mask = (htab_mask << sdr_sh) | sdr_mask;
    pgaddr = sdr | (hash & mask);
    pp = (mPTE_64_t *)pgaddr;

    /* replace old translation */
    for (found = 0, i = 0; !found && i < 8; i++)
        if (pp[i].avpn == avpn)
            found = 1;

    /* otherwise use a free slot */
    for (i = 0; !found && i < 8; i++)
        if (!pp[i].v)
            found = 1;

    /* out of slots, just evict one */
    if (!found) {
        i = next_grab_slot + 1;
        next_grab_slot = (next_grab_slot + 1) % 8;
    }
    i--;
    {
    mPTE_64_t p = {
        // .avpn_low = avpn,
        .avpn = avpn >> 7,
        .h = 0,
        .v = 1,

        .rpn = (phys & ~0xfff) >> 12,
        .r = mode & (1 << 8) ? 1 : 0,
        .c = mode & (1 << 7) ? 1 : 0,
        .w = mode & (1 << 6) ? 1 : 0,
        .i = mode & (1 << 5) ? 1 : 0,
        .m = mode & (1 << 4) ? 1 : 0,
        .g = mode & (1 << 3) ? 1 : 0,
        .n = mode & (1 << 2) ? 1 : 0,
        .pp = mode & 3,
    };
    pp[i] = p;
    }

    asm volatile("tlbie %0" :: "r"(ea));
}

static void
hash_page_32(ucell ea, ucell phys, ucell mode)
{
#ifndef __powerpc64__
    static int next_grab_slot = 0;
    unsigned long *upte, cmp, hash1;
    int i, vsid, found;
    mPTE_t *pp;

    vsid = (ea >> 28) + SEGR_BASE;
    cmp = BIT(0) | (vsid << 7) | ((ea & 0x0fffffff) >> 22);

    hash1 = vsid;
    hash1 ^= (ea >> 12) & 0xffff;
    hash1 &= (((mfsdr1() & 0x1ff) << 16) | 0xffff) >> 6;

    pp = (mPTE_t*)(get_hash_base() + (hash1 << 6));
    upte = (unsigned long*)pp;

    /* replace old translation */
    for (found = 0, i = 0; !found && i < 8; i++)
        if (cmp == upte[i*2])
            found = 1;

    /* otherwise use a free slot */
    for (i = 0; !found && i < 8; i++)
        if (!pp[i].v)
            found = 1;

    /* out of slots, just evict one */
    if (!found) {
        i = next_grab_slot + 1;
        next_grab_slot = (next_grab_slot + 1) % 8;
    }
    i--;
    upte[i * 2] = cmp;
    upte[i * 2 + 1] = (phys & ~0xfff) | mode;

    asm volatile("tlbie %0" :: "r"(ea));
#endif
}

static int is_ppc64(void)
{
#ifdef __powerpc64__
    return 1;
#elif defined(CONFIG_PPC_64BITSUPPORT)
    unsigned int pvr = mfpvr();
    return ((pvr >= 0x330000) && (pvr < 0x70330000));
#else
    return 0;
#endif
}

/* XXX Remove these ugly constructs when legacy 64-bit support is dropped. */
static void hash_page(unsigned long ea, unsigned long phys, ucell mode)
{
    if (is_ppc64())
        hash_page_64(ea, phys, mode);
    else
        hash_page_32(ea, phys, mode);
}

void
dsi_exception(void)
{
    unsigned long dar, dsisr;
    ucell mode;
    ucell phys;

    asm volatile("mfdar %0" : "=r" (dar) : );
    asm volatile("mfdsisr %0" : "=r" (dsisr) : );

    phys = ea_to_phys(dar, &mode);
    hash_page(dar, phys, mode);
}

void
isi_exception(void)
{
    unsigned long nip, srr1;
    ucell mode;
    ucell phys;

    asm volatile("mfsrr0 %0" : "=r" (nip) : );
    asm volatile("mfsrr1 %0" : "=r" (srr1) : );

    phys = ea_to_phys(nip, &mode);
    hash_page(nip, phys, mode);
}


/************************************************************************/
/*	init / cleanup							*/
/************************************************************************/

void
setup_mmu(unsigned long ramsize)
{
    ofmem_t *ofmem;
#ifndef __powerpc64__
    unsigned long sr_base;
#endif
    unsigned long hash_base;
    unsigned long hash_mask = ~0x000fffffUL; /* alignment for ppc64 */
    int i;

    /* SDR1: Storage Description Register 1 */

    hash_base = (ramsize - 0x00100000 - HASH_SIZE) & hash_mask;
    memset((void *)hash_base, 0, HASH_SIZE);
    if (is_ppc64())
        mtsdr1(hash_base | MAX(HASH_BITS - 18, 0));
    else
        mtsdr1(hash_base | ((HASH_SIZE - 1) >> 16));

#ifdef __powerpc64__

    /* Segment Lookaside Buffer */

    slbia(); /* Invalidate all SLBs except SLB 0 */
    for (i = 0; i < 16; i++) {
        unsigned long rs = ((0x400 + i) << 12) | (0x10 << 7);
        unsigned long rb = ((unsigned long)i << 28) | (1 << 27) | i;
        slbmte(rs, rb);
    }

#else

    /* Segment Register */

    sr_base = SEGR_USER | SEGR_BASE ;
    for (i = 0; i < 16; i++) {
        int j = i << 28;
        asm volatile("mtsrin %0,%1" :: "r" (sr_base + i), "r" (j));
    }

#endif

    ofmem = ofmem_arch_get_private();
    memset(ofmem, 0, sizeof(ofmem_t));
    ofmem->ramsize = ramsize;

    memcpy((void *)get_rom_base(), (void *)OF_CODE_START, 0x00100000);

    /* Enable MMU */

    mtmsr(mfmsr() | MSR_IR | MSR_DR);
}

void
ofmem_init(void)
{
    ofmem_t *ofmem = ofmem_arch_get_private();

    ofmem_claim_phys(0, get_ram_bottom(), 0);
    ofmem_claim_virt(0, get_ram_bottom(), 0);
    ofmem_map(0, 0, get_ram_bottom(), 0);

    ofmem_claim_phys(get_ram_top(), ofmem->ramsize - get_ram_top(), 0);
    ofmem_claim_virt(get_ram_top(), ofmem->ramsize - get_ram_top(), 0);
    ofmem_map(get_ram_top(), get_ram_top(), ofmem->ramsize - get_ram_top(), 0);
}
