/*
 * xnvme_pal.c
 *
 * Default platform-abstraction implementations. Replace the bodies to match
 * your Vivado block design if the assumptions below do not hold.
 */

#include "xnvme_pal.h"
#include "xparameters.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xil_types.h"

/*
 * BSP quirk this driver must work around (standalone, microblaze/xil_cache.c):
 *
 *   microblaze_invalidate_dcache_range() and microblaze_flush_dcache_range()
 *   step over lines with `while (startadr < endadr)`, where
 *   endadr = (addr + len - 1) & ~(line - 1).
 *
 * Two consequences:
 *   1. A range that starts line-aligned and fits within ONE line aligns
 *      startadr and endadr to the SAME address -> the loop body never runs
 *      and the operation is silently a NO-OP. The admin CQE poll
 *      (16-byte XNvmeCqeDw, XPAR_MICROBLAZE_DCACHE_LINE_LEN 4 -> 16-byte
 *      lines, page-aligned admin_cq) hit exactly this: with D-cache enabled
 *      the CPU kept hitting its stale cached CQE and every admin command
 *      "timed out" even though the drive had written the completion.
 *   2. endadr is rounded DOWN, so the cache line containing the LAST byte of
 *      any range is never operated on (a 4 KiB invalidate only covered 255 of
 *      its 256 lines).
 *
 * The wrappers below round the range out to whole lines and add one extra
 * byte, which defeats the `<` loop while never reaching past the line that
 * contains the range's last byte. Note: for an unaligned end this invalidates
 * the remainder of that line too - harmless here, the D-cache is
 * write-through (XPAR_MICROBLAZE_DCACHE_USE_WRITEBACK 0) so no line is ever
 * dirty and invalidating neighbours cannot lose data.
 */
#define XNVME_PAL_DCACHE_LINE  ((u32)XPAR_MICROBLAZE_DCACHE_LINE_LEN * 4u)

static void xnvme_pal_dcache_range(void *addr, u32 len, int flush)
{
    UINTPTR a = (UINTPTR)addr;
    UINTPTR s = a & ~(UINTPTR)(XNVME_PAL_DCACHE_LINE - 1u);
    UINTPTR e = ((a + (UINTPTR)len) + (UINTPTR)(XNVME_PAL_DCACHE_LINE - 1u)) &
                ~(UINTPTR)(XNVME_PAL_DCACHE_LINE - 1u);
    u32     l = (u32)(e - s) + 1u;    /* +1: defeat the `startadr < endadr` loop */

    if (flush)
        Xil_DCacheFlushRange(s, l);
    else
        Xil_DCacheInvalidateRange(s, l);
}

/*
 * Assumption: the PCIe Root Port's AXI slave port is mapped into the
 * MicroBlaze address space such that a write to the SSD's PCIe address
 * reaches the SSD. For most XDMA / PCIe RP designs an outbound (AXI->PCIe)
 * bar translates a fixed AXI window to a PCIe window. If your window has an
 * offset, add it here, e.g.:
 *     return Xil_In32((UINTPTR)(pcie_addr) + MY_PCIE_AXI_OFFSET);
 */
u32 XNvme_PalPcieRead32(u64 pcie_addr)
{
    return Xil_In32((UINTPTR)pcie_addr);
}

void XNvme_PalPcieWrite32(u64 pcie_addr, u32 value)
{
    Xil_Out32((UINTPTR)pcie_addr, value);
}

u64 XNvme_PalPcieRead64(u64 pcie_addr)
{
    u32 lo = Xil_In32((UINTPTR)pcie_addr);
    u32 hi = Xil_In32((UINTPTR)pcie_addr + 4u);
    return ((u64)hi << 32) | (u64)lo;
}

void XNvme_PalPcieWrite64(u64 pcie_addr, u64 value)
{
    Xil_Out32((UINTPTR)pcie_addr,      (u32)(value & 0xFFFFFFFFu));
    Xil_Out32((UINTPTR)pcie_addr + 4u, (u32)(value >> 32));
}

/*
 * Assumption: the SSD can DMA to the same address the MicroBlaze uses for
 * DDR (identity-mapped PCIe outbound window). If your design remaps DDR to
 * a different PCIe base, subtract/add the offset here.
 */
u64 XNvme_PalLocalToPcie(void *local_addr)
{
    return (u64)(UINTPTR)local_addr;
}

u64 XNvme_PalAdminMemToPcie(void *local_addr)
{
    return XNvme_PalLocalToPcie(local_addr);
}

void XNvme_PalFlushDcache(void *addr, u32 len)
{
    xnvme_pal_dcache_range(addr, len, 1);
}

void XNvme_PalInvalidateDcache(void *addr, u32 len)
{
    xnvme_pal_dcache_range(addr, len, 0);
}

void XNvme_PalSleepUsec(u32 usec)
{
    /* Crude spin delay; ~1 iteration per few ns on MicroBlaze. Good enough
     * for controller-ready polling. Replace with a timer for accuracy.  */
    volatile u32 cnt = usec * 10u;
    while (cnt--) {
        __asm__ volatile ("nop");
    }
}

void XNvme_PalDsb(void)
{
    /* Ensure preceding AXI writes (SQE push / doorbell) have reached the
     * IP before we continue. MicroBlaze is strongly ordered, so a compiler
     * barrier suffices for ordering; the mbar instruction additionally
     * drains outstanding AXI data transactions on MicroBlaze.
     * NOTE: MicroBlaze (v11.0 / microblaze-xilinx-elf binutils) has no
     * "msync"/"sync" opcode - the data-sync barrier is "mbar 1" (matches
     * the BSP's DATA_SYNC macro in xil_io.h).                          */
#ifdef __MICROBLAZE__
    __asm__ volatile ("mbar 1" ::: "memory");
#else
    __asm__ volatile ("" ::: "memory");
#endif
}
