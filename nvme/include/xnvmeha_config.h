/*
 * xnvmeha_config.h
 *
 * Build-time configuration for the Xilinx NVMe Host Accelerator (PG328)
 * bare-metal MicroBlaze driver.
 *
 * >>> EDIT THE VALUES BELOW TO MATCH YOUR VIVADO IP CONFIGURATION <<<
 *
 * These must exactly match the parameters set in the NVMe Host Accelerator
 * IP customization dialog (see PG328 Table 27: User Parameters) and the
 * address assignments in your Vivado block design.
 */

#ifndef XNVMEHA_CONFIG_H_
#define XNVMEHA_CONFIG_H_

#include "xil_types.h"

/* =========================================================================
 *  IP core parameters (must match Vivado IP customization)
 * ========================================================================= */

/* C_MAX_SSD : number of backend SSDs (1..24).                        */
#define XNVMEHA_MAX_SSD                 1

/* C_MAX_SQ  : number of SQ/CQ pairs per SSD (1, 2 or 4).              */
#define XNVMEHA_MAX_SQ                  4

/* C_NUM_SQ_SW : number of SQs per SSD assigned to the SW interface
 * (0..4). When set to 0 the software interface is disabled. For a
 * software-only design set this equal to C_MAX_SQ.                    */
#define XNVMEHA_NUM_SQ_SW               4

/* C_NUM_SQ_HW : number of SQs per SSD assigned to the HW interface
 * (0..4). This driver only drives the SW interface; leave 0 for a pure
 * SW design.                                                          */
#define XNVMEHA_NUM_SQ_HW               0

/* C_SQ_DEPTH_SW : depth of every SW queue (32 or 64).                 */
#define XNVMEHA_SQ_DEPTH_SW             64

/* AXI_MEM_DWIDTH : data width of the SW_AXI4 slave (32 or 64).
 * For a 32-bit MicroBlaze keep this at 32 and connect MicroBlaze's AXI
 * data master directly (no width converter).                          */
#define XNVMEHA_AXI_MEM_DWIDTH          32

/* SQE push addressing experiment: 1 = word i written to addr+i*4 (fills
 * a 64 B slot by address); 0 = all words written to the same address
 * (FIFO-style). The SW-port probe (INT_STATUS RD_NO_CQ) proved the SW
 * port decodes 0x44A00000; the push readback debug print in
 * XNvmeHa_PushWords tells which model the IP implements.              */
#define XNVMEHA_PUSH_ADDR_INC           1

/* C_SW_FULL_CMD :
 *   1 -> full 64-byte SQE pushed by SW (SW_AXI4 write data width 512b).
 *   0 -> partial 32-byte SQE pushed by SW.
 * MISMATCH WARNING: top_ku040.xsa reports xlnx,sw-full-cmd = <1> for the
 *   nvmeha_0 IP (see platform hw_artifacts pl.dtsi:556 / sdt.dts:605). That
 *   means the IP's SW_AXI4 interface EXPECTS 64-byte SQEs. This driver
 *   currently sets XNVMEHA_SW_FULL_CMD = 0 (32-byte partial path), which will
 *   produce malformed commands. MUST RESOLVE ONE OF:
 *     (a) set XNVMEHA_SW_FULL_CMD = 1 here to match the IP (least disruption,
 *         no Vivado IP re-customization), OR
 *     (b) in Vivado change nvmeha_0 "sw-full-cmd" to 0 to match this driver.
 *   Leave as 0 only if you deliberately switch the IP to sw-full-cmd=0.     */
#define XNVMEHA_SW_FULL_CMD             1

/* SW_QOS_EN : 1 enables QoS on the SW interface (SW must target the
 * exact SSDn/SQm address). 0 lets the IP round-robin internally and SW
 * always writes to SQ0 of each SSD.
 *
 * Enable (1) when you want to drive individual submission queues from
 * software - e.g. mapping one NVMe I/O SQ per data channel so several
 * channels can be submitted concurrently for higher write throughput.
 * Each SW SQ still belongs to the same SSD and shares that SSD's credit
 * pool. NVMe qid mapping: SW SQ index 0 -> qid 1, 1 -> qid 2, ...      */
#define XNVMEHA_SW_QOS_EN               1

/* CQ_FIFO_DEPTH : CQ FIFO depth (16..64). Informational only.         */
#define XNVMEHA_CQ_FIFO_DEPTH           16


/* =========================================================================
 *  Address map (must match your Vivado address editor)
 *
 *  Values below are taken from the address editor of design_1. The nvmeha_0
 *  IP exposes three AXI slave segments; their CURRENT assignments in
 *  top_ku040_v3.xsa (design_1.hwh / pl.dtsi:553) are:
 *     S_AXI_SSD  (SSD-side)   @ 0x44A20000 .. 0x44A27FFF  (32 KB)
 *         - from axi_pcie3_0 M_AXI (this is what the SSD sees), AND
 *         - from MicroBlaze M_AXI_DC @ 0x44A50000 .. 0x44A5FFFF (alias)
 *     S_AXI      (SW AXI4 IF) @ 0x44A00000 .. 0x44A3FFFF  (256 KB)
 *         - the CQE pop lives at +0x30000 = 0x44A30000, so this segment must
 *           stay >= 0x31000; do not shrink it back to 16 KB
 *     S_AXI_LITE (registers)  @ 0x44A40000 .. 0x44A4FFFF  (64 KB)
 *  (An earlier revision of this comment documented 16 KB for all three and
 *   put S_AXI_LITE at 0x44A10000 - both stale. The numbers above match the
 *   exported XSA and platform/hw/sdt/pl.dtsi.)
 * ========================================================================= */

/* AXI4-Lite register base of the NVMe Host Accelerator, as seen by
 * MicroBlaze.  (nvmeha_0 / S_AXI_LITE, design_1.hwh MEMRANGE S_AXI_LITE.)  */
#define XNVMEHA_REG_BASE                0x44A40000u

/* AXI4 memory-mapped SW interface base (for pushing SQEs / reading
 * CQEs), as seen by MicroBlaze.  (nvmeha_0 / S_AXI.)
 * VERIFIED against top_ku040_new.xsa (design_1.hwh): S_AXI is at
 * 0x44A00000 with size 0x40000 (256 KB) - it was enlarged IN PLACE, not
 * relocated. The old 0x44A70000 pointed into an unmapped hole: SQE
 * pushes silently vanished (posted AXI writes, no error) and the IP
 * never issued anything (iss=0). CQ pop at +0x30000 = 0x44A30000 fits
 * the 256 KB segment.                                                        */
#define XNVMEHA_SW_AXI_BASE             0x44A00000u

/* AXI Interrupt Controller base + interrupt input vector that the
 * ha_interrupt pin of the IP is connected to (pl.dtsi:
 *   nvmeha_0 { interrupt-parent = <&axi_intc_0>; interrupts = <1 2>; } ).
 * Vec 0 = axi_dma_0, vec 1 = nvmeha ha_interrupt, vec 2 = axi_uartlite_0.
 * Set XNVMEHA_USE_INTC to 0 if you wire the interrupt yourself.            */
#define XNVMEHA_USE_INTC                1
#define XNVMEHA_INTC_BASE               0x41200000u   /* XPAR_AXI_INTC_0_BASEADDR */
#define XNVMEHA_INTC_VEC_ID             1u


/* =========================================================================
 *  PCIe / NVMe controller geometry
 *
 *  PCIe address translation in design_1 (axi_pcie3_0, RC mode):
 *    - Outbound (AXI->PCIe), AXIBAR2PCIEBAR_0 = 0:
 *        AXI 0x76000000..0x760FFFFF  ->  PCIe 0x00000000..0x000FFFFF  (1 MB)
 *        AXI 0x76100000..0x761FFFFF  ->  PCIe 0x00000000..0x000FFFFF  (1 MB)
 *      => the SSD's BAR0 must be enumerated at PCIe 0x00000000 and is
 *         reached from MicroBlaze / from the IP's M_AXI_SSD at AXI 0x76000000.
 *    - Inbound (PCIe->AXI), C_PCIEBAR2AXIBAR_0 = 0 (identity):
 *        PCIe 0x44A20000  ->  AXI 0x44A20000  (nvmeha_0 S_AXI_SSD: I/O SQ/CQ)
 *        PCIe 0x80000000..0xFFFFFFFF -> AXI DDR4 (admin queue + data buffers)
 *      axi_pcie3_0 M_AXI master can only reach nvmeha S_AXI_SSD and DDR4
 *      (design_1.hwh MEMORYMAP of axi_pcie3_0).
 *
 *  Because the outbound window is NOT identity-mapped (PG328 p.30 assumes an
 *  identity window), the value programmed into the SSD PCIe base register and
 *  used as ctrl_base for doorbell writes is the AXI-side address 0x76000000,
 *  NOT the PCIe address 0x0.
 * ========================================================================= */

/* SSD PCIe base address (controller registers / BAR0). Programmed into the
 * NVMeHA REG_PCIE_BASE_LO/HI(n) registers and used by the driver as the
 * ctrl_base for NVMe controller register + doorbell accesses. In this design
 * this is the AXI-side address that maps to the SSD BAR0 (= 0x76000000),
 * which assumes enumeration assigns SSD BAR0 at PCIe 0x00000000. If your
 * enumeration assigns BAR0 elsewhere, recompute:
 *    XNVMEHA_SSD0_PCIE_BASE = 0x76000000 + (pcie_bar0 & 0xFFFFF).
 * NOTE: xaxipcie_rc_enumerate_example.c assigns the SSD BAR0 = 0x00000000
 *    (PCIE_CFG_BAR_0_ADDR, 4 KB aligned, lands inside the 0x76000000 outbound
 *    window which maps AXI 0x76000000 -> PCIe 0x0). This was already fixed;
 *    do NOT revert it to the original 0x00001111.                            */
#define XNVMEHA_SSD0_PCIE_BASE          0x76000000u
#define XNVMEHA_SSD0_PCIE_BASE_HI       0x00000000u
#define XNVMEHA_SSD1_PCIE_BASE          0x00000000u
#define XNVMEHA_SSD1_PCIE_BASE_HI       0x00000000u

/* PCIe address of SSD0 SQ0 (4 KB aligned; must equal the address handed to
 * the SSD in Create I/O Submission Queue). The IP derives every other SQ/CQ
 * base from the two registers below (PG328 programming sequence, page 30).
 *
 * LAYOUT (verified against the IP's own read-back):
 *   The IP is built with C_MAX_SQ = 4 and C_SQ_DEPTH_SW = 64, so its internal
 *   SSD-side memories always span 4 x 4 KB per direction, whatever the host
 *   creates:
 *     SQ region : SQ0 = 0x44A20000, +0x1000 each -> ..0x44A23FFF  (16 KB)
 *     CQ region : CQ0 = 0x44A24000, +0x1000 each -> ..0x44A27FFF  (16 KB)
 *   => exactly the 32 KB that the axi_pcie3_0 M_AXI assignment of
 *   nvmeha_0/S_AXI_SSD now covers in the Vivado address editor (it was 16 KB
 *   before, which forced CQ1 onto the IP's SQ3 address - the aliasing that
 *   made every I/O completion disappear). Keep CQ0 OUT of the SQ region: it
 *   must be >= SQ0 + C_MAX_SQ * 0x1000.
 *
 * The 0x44A50000 alias of S_AXI_SSD is reachable ONLY from MicroBlaze, NOT
 * from the SSD.                                                              */
#define XNVMEHA_SQ0_PCIE_BASE           0x44A20000u   /* SQ1..SQ4 = +0x1000 each */
/* PCIe address of SSD0 CQ0 (4 KB aligned, 4 KB stride - the SSD enforces
 * page alignment on I/O CQ bases, see the CQ_STRIDE_BYTES note in xnvme.c).
 * Must sit past the whole SQ region (see above).                             */
#define XNVMEHA_CQ0_PCIE_BASE           0x44A24000u

/* Number of NVMe I/O queue pairs actually created / driven. The 32 KB
 * S_AXI_SSD window allows 4; 2 keeps the bench focused on 2 channels.        */
#define XNVMEHA_ACTIVE_QUEUES           2u

/* MicroBlaze-side alias of the IP's SSD-side memory.
 *
 * design_1 (v3) reaches nvmeha_0/S_AXI_SSD from two masters:
 *   axi_pcie3_0 M_AXI  @ 0x44A20000..0x44A27FFF  <- what the SSD sees
 *   microblaze  M_AXI_DC @ 0x44A50000..0x44A5FFFF <- what we can inspect
 * (design_1.hwh MEMRANGE, and pl.dtsi:553 reg). Both decode the SAME slave
 * with a fixed +0x30000 offset, so a PCIe address P inside 0x44A20000..
 * 0x44A27FFF reads back at P + 0x30000. That is the only software-visible
 * window onto exactly the buffers the SSD is supposed to read (SQE fetch)
 * and write (CQE post-back), which is what XNvmeHa_DumpSsdMemory uses.
 *
 * Do NOT read 0x44A20000 from the MicroBlaze for this: M_AXI_DC decodes
 * 0x44A00000..0x44A3FFFF onto the SW port (S_AXI), so it would land in the
 * middle of the SQE push region instead. And do not read the SW slot of an
 * SSD that does not exist either (0x44A01000+ is SSD1's slot 0, and
 * C_MAX_SSD = 1) - that is an address error and the read may never return.
 *
 * Offset check against the live design (32 KB SSD-side window):
 *   SQ1 0x44A20000 -> 0x44A50000     CQ1 0x44A24000 -> 0x44A54000
 *   SQ2 0x44A21000 -> 0x44A51000     CQ2 0x44A25000 -> 0x44A55000
 * all inside the 0x44A50000..0x44A5FFFF range.                              */
#define XNVMEHA_SSD_ALIAS_BASE          0x44A50000u
#define XNVMEHA_SSD_DECODE_BASE         0x44A20000u
#define XNVMEHA_SSD_ALIAS(pcie_addr) \
    ((u32)(XNVMEHA_SSD_ALIAS_BASE + ((u32)(pcie_addr) - XNVMEHA_SSD_DECODE_BASE)))

/* Admin queue SQ/CQ: PCIe address of the host memory used for the admin
 * queue. The admin queue lives in DDR4 (0x80000000..0xFFFFFFFF, reachable
 * by the SSD over the identity PCIe->AXI inbound window). These macros are
 * informational only - the driver derives the actual addresses at runtime
 * from the linker placement of admin_sq/admin_cq via XNvme_PalAdminMemToPcie.
 * Make sure the linker script places .bss/.data in DDR (not in LMB BRAM).   */
#define XNVMEHA_ADMIN_SQ_PCIE_BASE      0x80000000u
#define XNVMEHA_ADMIN_CQ_PCIE_BASE      0x80001000u
#define XNVMEHA_ADMIN_SQ_DEPTH          16u
#define XNVMEHA_ADMIN_CQ_DEPTH          16u


/* =========================================================================
 *  Driver behaviour
 * ========================================================================= */

/* Maximum number of commands that may be outstanding (command-id pool).
 * Must be <= 65535 and should be >= the per-SSD SW credit
 * (C_NUM_SQ_SW * (C_SQ_DEPTH_SW - 1)) so the slot pool is never the
 * bottleneck versus the hardware queue capacity. For multi-SSD designs
 * that drive several SSDs concurrently, multiply by C_MAX_SSD.        */
#define XNVMEHA_MAX_OUTSTANDING         (XNVMEHA_NUM_SQ_SW * XNVMEHA_SQ_DEPTH_SW)

/* Spin-wait timeout for synchronous commands, in raw loop iterations
 * (CPU-speed dependent). Increase if commands time out prematurely.     */
#define XNVMEHA_SYNC_TIMEOUT            0x1000000u

/* Enable xil_printf tracing. 0 = silent, 1 = errors, 2 = verbose.      */
#define XNVMEHA_DEBUG                   2

#endif /* XNVMEHA_CONFIG_H_ */
