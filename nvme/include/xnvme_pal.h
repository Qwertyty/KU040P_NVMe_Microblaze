/*
 * xnvme_pal.h
 *
 * Platform Abstraction Layer for the NVMe Host Accelerator driver.
 *
 * The NVMe Host Accelerator only offloads the I/O queues. The admin queue
 * is managed entirely by software and requires direct access to the SSD's
 * PCIe address space (NVMe controller registers + doorbell writes).
 * Furthermore, the SSD DMAs data buffers directly from/to host DDR, so the
 * buffers must be reachable over PCIe and the driver must translate local
 * MicroBlaze addresses into the PCIe addresses the SSD uses.
 *
 * All of that is board/design specific, so it is isolated here. The
 * default implementations in xnvme_pal.c assume a simple, common setup:
 *   - the PCIe Root Port AXI slave is mapped 1:1 into the MicroBlaze
 *     address space, so a PCIe address is written directly with Xil_Out32;
 *   - the DDR outbound window is an identity map, so a local buffer
 *     address equals its PCIe address.
 * Override these for your design if needed.
 */

#ifndef XNVME_PAL_H_
#define XNVME_PAL_H_

#include "xil_types.h"

/* ----- PCIe memory-mapped access (NVMe controller regs + doorbells) ----- */

/* Read/write a 32-bit value at a PCIe address (the SSD's address space). */
u32  XNvme_PalPcieRead32(u64 pcie_addr);
void XNvme_PalPcieWrite32(u64 pcie_addr, u32 value);

/* Read/write 64-bit (used for CAP / ASQ / ACQ). Implemented as two 32-bit
 * accesses so it works on a 32-bit MicroBlaze. */
u64  XNvme_PalPcieRead64(u64 pcie_addr);
void XNvme_PalPcieWrite64(u64 pcie_addr, u64 value);

/* ----- Address translation ----- */

/* Translate a local (MicroBlaze) buffer address into the PCIe address the
 * SSD must use to DMA to/from that buffer. Used to fill SGL/PRP entries. */
u64  XNvme_PalLocalToPcie(void *local_addr);

/* Translate a local DDR address into the PCIe address for admin-queue
 * memory (SQ/CQ). Usually the same mapping as buffers. */
u64  XNvme_PalAdminMemToPcie(void *local_addr);

/* ----- Cache maintenance (DMA coherency) ----- */

/* Flush a region so the SSD sees the latest data written by the CPU
 * (call before a write command). */
void XNvme_PalFlushDcache(void *addr, u32 len);

/* Invalidate a region so the CPU reads SSD-written data instead of stale
 * cache lines (call before reading the result of a read command). */
void XNvme_PalInvalidateDcache(void *addr, u32 len);

/* ----- Misc ----- */

/* Rough microsecond delay (loops/MicroBlaze timer - any blocking delay). */
void XNvme_PalSleepUsec(u32 usec);

/* Raw CPU memory barrier: force AXI writes to complete before continuing. */
void XNvme_PalDsb(void);

#endif /* XNVME_PAL_H_ */
