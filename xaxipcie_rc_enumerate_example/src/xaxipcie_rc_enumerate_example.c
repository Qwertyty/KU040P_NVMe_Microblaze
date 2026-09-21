/******************************************************************************
* Copyright (C) 2018 - 2020 Xilinx, Inc.  All rights reserved.
* Copyright (C) 2022 - 2023 Advanced Micro Devices, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/****************************************************************************/
/**
* @file xaxipcie_rc_enumerate_example.c
*
* This file contains a design example for using AXI PCIe IP and its driver.
* This is an example to show the usage of driver APIs when AXI PCIe IP is
* configured as a Root Port.  The AXI PCIe can be configured as a Root Port
* only on the 7 Series Xilinx FPGA families.
*
* The example initialises the AXI PCIe IP and shows how to enumerate the PCIe
* system.
*
* @note
*
* This example should be used only when AXI PCIe IP is configured as
* root complex.
*
* This code will illustrate how the XAxiPcie IP and its standalone driver can
* be used to:
*	- Initialize a AXI PCIe IP core built as a root complex
*	- Enumerate PCIe end points in the system
*
* Please note that this example enumerates and initializes PCIe end points
* only. It does not shows how to deal with PCIe switches (and its virtual
* P2P bridges)
*
* We tried to use as much of the driver's API calls as possible to show the
* reader how each call could be used and that probably made the example not
* the shortest way of doing the tasks shown as they could be done.
*
*<pre>
* MODIFICATION HISTORY:
*
* Ver   Who  Date     Changes
* ----- ---- -------- -----------------------------------------------
* 2.00a nm   10/19/11 Initial version of AXI PCIe root complex example
* 2.02a nm   08/19/12 Removed the calls to XAxiPcie_GetLocalBusBar2PcieBar
*		      and XAxiPcie_SetLocalBusBar2PcieBar as they are not
*		      needed for enumeration
* 3.1   ms   01/23/17 Added xil_printf statement in main function to
*                     ensure that "Successfully ran" and "Failed" strings
*                     are available in all examples. This is a fix for
*                     CR-965028.
*       ms   04/05/17 Added tabspace for return statements in functions
*                     for proper documentation while generating doxygen.
*
*</pre>
*****************************************************************************/

/***************************** Include Files ********************************/

#include "xparameters.h"	/* Defines for XPAR constants */
#include "xaxipcie.h"		/* XAxiPcie level 1 interface */
#include "stdio.h"
#include "xil_printf.h"
#include "xnvme.h"
#include "xnvmeha_config.h"
#include "xil_cache.h"
#include "xil_mem.h"
#include "xil_exception.h"
#include "xil_io.h"
#include "xil_types.h"
#include "xintc.h"
#include "xstatus.h"

/************************** Constant Definitions ****************************/

#ifndef SDT
#define AXIPCIE_DEVICE_ID 	XPAR_AXIPCIE_0_DEVICE_ID
#endif


/*
 * Command register offsets
 */
#define PCIE_CFG_CMD_IO_EN	0x00000001 /* I/O access enable */
#define PCIE_CFG_CMD_MEM_EN	0x00000002 /* Memory access enable */
#define PCIE_CFG_CMD_BUSM_EN	0x00000004 /* Bus master enable */
#define PCIE_CFG_CMD_PARITY	0x00000040 /* parity errors response */
#define PCIE_CFG_CMD_SERR_EN	0x00000100 /* SERR report enable */

/*
 * PCIe Configuration registers offsets
 */

#define PCIE_CFG_ID_REG			0x0000 /* Vendor ID/Device ID offset */
#define PCIE_CFG_CMD_STATUS_REG		0x0001 /*
						* Command/Status Register
						* Offset
						*/
#define PCIE_CFG_PRI_SEC_BUS_REG	0x0006 /*
						* Primary/Sec.Bus Register
						* Offset
						*/
#define PCIE_CFG_CAH_LAT_HD_REG		0x0003 /*
 						* Cache Line/Latency Timer/
 						* Header Type/
 						* BIST Register Offset
 						*/
#define PCIE_CFG_BAR_0_REG		0x0004 /* PCIe Base Addr 0 */

#define PCIE_CFG_FUN_NOT_IMP_MASK	0xFFFF
#define PCIE_CFG_HEADER_TYPE_MASK	0x00EF0000
#define PCIE_CFG_MUL_FUN_DEV_MASK	0x00800000


/*
 * ECAM scan range.
 *
 * PG194 (v3.0) Table 51 "ECAM Addressing" defines the Gen3 bridge's ECAM
 * address breakdown as bits[27:20]=Bus, [19:15]=Device, [14:12]=Function,
 * [11:2]=Register/Extended-Register. This is bit-for-bit identical to what
 * the Gen2 axipcie driver composes (XAXIPCIE_ECAM_BUS_SHIFT=20, DEV=15,
 * FUN=12, REG=2, ECAM_MASK=0x0FFFFFFF), so the driver's addressing is
 * CORRECT for this core -- no software workaround is needed for config
 * access itself.
 *
 * PG194 Table 11 parameter G39 (C_BASEADDR) requires that, in Root Port
 * mode, the AXI4-Lite control aperture have a minimum alignment granularity
 * of 256MB because bits[27:0] carry Bus/Device/Function. This design's BD
 * only assigns S_AXI_CTL 16KB (0x10000000-0x10003FFF), so any Bus>=1 ECAM
 * address (>= 0x10100000) falls outside the decoded window and reads back
 * bus-float garbage (0xDEE3) instead of a config completion. That is a
 * hardware address-assignment bug, fixed in Vivado (see
 * fix_addr_ranges.tcl), not in software.
 *
 * PG194 p.57 further states that an ECAM access to a bus number outside the
 * secondary..subordinate range does not generate a configuration request and
 * signals SLVERR. PcieInitRootComplex() programs primary=0, secondary=1,
 * subordinate=7 (PCIE_CFG_PRIM_SEC_BUS), so buses 0..7 are legal. Scanning
 * bus 0 (the RC's own Type-1 header) plus bus 1 (the SSD) is all this design
 * needs, and it avoids provoking SLVERR on out-of-range buses.
 */
#define PCIE_CFG_MAX_NUM_OF_BUS		2
#define PCIE_CFG_MAX_NUM_OF_DEV		1
#define PCIE_CFG_MAX_NUM_OF_FUN		8

#define PCIE_CFG_PRIM_SEC_BUS		0x00070100

#define PCIE_CFG_HEADER_O_TYPE		0x0000

/* SSD BAR0 base assigned during enumeration.
 * MUST be 4 KB-aligned and MUST fall inside the RC outbound window
 * (AXI 0x76000000 -> PCIe 0x0). 0x0 makes the SSD controller registers
 * appear at PCIe 0x0 == AXI 0x76000000, which is exactly
 * XNVMEHA_SSD0_PCIE_BASE that the driver uses to read CAP/CC/doorbells.
 * (The previous 0x1111 was neither 4 KB-aligned nor matched the window.) */
#define PCIE_CFG_BAR_0_ADDR		0x00000000

/**************************** Type Definitions ******************************/


/***************** Macros (Inline Functions) Definitions ********************/


/************************** Function Prototypes *****************************/

#ifndef SDT
int PcieInitRootComplex(XAxiPcie *AxiPciePtr, u16 DeviceId);
#else
int PcieInitRootComplex(XAxiPcie *AxiPciePtr, UINTPTR BaseAddress);
#endif

void PCIeEnumerateFabric(XAxiPcie *AxiPciePtr);


/************************** Variable Definitions ****************************/

/* Allocate PCIe Root Complex IP Instance */
XAxiPcie AxiPcieInstance;






#define NVME_INTC_DEVICE_ID     0
#define NVME_INTC_VEC           XNVMEHA_INTC_VEC_ID

static XNvme   nvme_dev;
static XIntc   intc;

/* AXI INTC ISR trampoline -> NVMe completion handler. */
static void NvmeIsr(void *callback_ref)
{
    XNvme *nv = (XNvme *)callback_ref;
    XNvme_InterruptHandler(nv);
}

/* Wire hc_interrupt through the AXI Interrupt Controller. */
static int SetupInterrupts(XNvme *nv)
{
    int rc;

    rc = XIntc_Initialize(&intc, NVME_INTC_DEVICE_ID);
    if (rc != XST_SUCCESS) {
        xil_printf("INTC init failed %d\r\n", rc);
        return rc;
    }

    rc = XIntc_Connect(&intc, NVME_INTC_VEC,
                       (XInterruptHandler)NvmeIsr, nv);
    if (rc != XST_SUCCESS) {
        xil_printf("INTC connect failed %d\r\n", rc);
        return rc;
    }

    XIntc_Enable(&intc, NVME_INTC_VEC);

    rc = XIntc_Start(&intc, XIN_REAL_MODE);
    if (rc != XST_SUCCESS) {
        xil_printf("INTC start failed %d\r\n", rc);
        return rc;
    }

    /* Register the INTC with the MicroBlaze exception system. */
    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)XIntc_InterruptHandler,
                                 &intc);
    Xil_ExceptionEnable();
    return XST_SUCCESS;
}

/* LBA-format a u64 for printing (MicroBlaze has no %llu in xil_printf). */
static void print_u64(u64 v)
{
    char buf[24];
    int i = sizeof(buf) - 1;
    buf[i--] = '\0';
    if (v == 0) { xil_printf("0"); return; }
    while (v && i >= 0) { buf[i--] = '0' + (char)(v % 10); v /= 10; }
    xil_printf(&buf[i + 1]);
}

/* Print a fixed-length (non null-terminated) Identify field safely. */
static void print_field(const char *s, u32 n)
{
    u32 i;
    for (i = 0; i < n && s[i]; i++)
        xil_printf("%c", s[i]);
}


/* --------------------------------------------------------------------------
 * Capacity sanity check (diagnostic only - safe to delete).
 *
 * If the reported namespace is far smaller than the physical part, the cause
 * is almost always controller-side, not host-side. The decisive discriminator
 * is TNVMCAP (Identify Controller bytes 295:280): total NVM capacity in
 * BYTES, reported independently of how NS1 happens to be formatted.
 *
 *   TNVMCAP ~= physical size -> media is known; NS1 is merely formatted
 *                               small (fixable with Format NVM / NS mgmt).
 *   TNVMCAP == 0 (or tiny)   -> the controller cannot see its own media, so
 *                               the namespace is a stub. Classic Silicon
 *                               Motion ROM / safe mode, whose fingerprint is:
 *                               MN = bare controller part number ("SM2263"),
 *                               FR = generic ROM build ("0350"), SN = place-
 *                               holder, and NS1 = 2097151 x 512 B = 1023 MiB.
 *                               In that state the host path is fine but the
 *                               drive has no usable media.
 * ------------------------------------------------------------------------ */
static void PrintCapacityDiag(const XNvmeIdentifyController *idc,
                              const XNvmeIdentifyNamespace *idn)
{
    u32 nlbaf, i;
    u64 tnv_lo = XNVME_IDC_TNVMCAP_LO(idc);
    u64 tnv_hi = XNVME_IDC_TNVMCAP_HI(idc);

    xil_printf("\r\n--- capacity diagnostic ---\r\n");
    xil_printf("IDC VID=0x%04X SSVID=0x%04X ver=0x%08X NN=%u FNA=0x%02X\r\n",
               XNVME_IDC_VID(idc), XNVME_IDC_SSVID(idc),
               XNVME_IDC_VER(idc), XNVME_IDC_NN(idc), XNVME_IDC_FNA(idc));

    xil_printf("TNVMCAP (total media bytes) = ");
    if (tnv_hi != 0u)
        xil_printf("<hi64=nonzero> ");
    print_u64(tnv_lo);
    xil_printf("\r\nUNVMCAP (unallocated bytes) = ");
    print_u64(XNVME_IDC_UNVMCAP_LO(idc));

    xil_printf("\r\nNS1 NSZE="); print_u64(XNVME_IDN_NSZE(idn));
    xil_printf(" NCAP=");        print_u64(XNVME_IDN_NCAP(idn));
    xil_printf(" NUSE=");        print_u64(XNVME_IDN_NUSE(idn));
    xil_printf("\r\nNS1 NVMCAP (bytes)="); print_u64(XNVME_IDN_NVMCAP_LO(idn));
    xil_printf(" NSFEAT=0x%02X FLBAS=0x%02X\r\n",
               XNVME_IDN_NSFEAT(idn), XNVME_IDN_FLBAS(idn));

    nlbaf = (u32)XNVME_IDN_NLBAF(idn) & 0x0Fu;    /* 0-based count */
    xil_printf("LBA formats (NLBAF=%u):\r\n", nlbaf);
    for (i = 0; i <= nlbaf && i < 16u; i++) {
        u32 ds = (u32)XNVME_IDN_LBAF_DS(idn, i);
        xil_printf("  LBAF[%u]: ds=%u -> %u B/block, meta=%u B\r\n",
                   i, ds, (ds != 0u && ds < 32u) ? (1u << ds) : 0u,
                   (u32)XNVME_IDN_LBAF_MS(idn, i));
    }

    if (tnv_lo == 0u && tnv_hi == 0u) {
        xil_printf("VERDICT: TNVMCAP=0 -> the controller does not know its own\r\n");
        xil_printf("  media size, so NS1 is a stub and NOT the real capacity.\r\n");
        xil_printf("  Cross-check MN/FR printed above: a bare controller part\r\n");
        xil_printf("  number plus a generic firmware build means the SSD booted\r\n");
        xil_printf("  its ROM instead of its firmware. The FPGA/PCIe/IP path is\r\n");
        xil_printf("  fine - the drive itself needs firmware restored, or swap\r\n");
        xil_printf("  in a known-good SSD to continue data-path bring-up.\r\n");
        xil_printf("  Expect I/O writes to this stub namespace to FAIL.\r\n");
    } else {
        xil_printf("VERDICT: the controller knows its media size; NS1 is simply\r\n");
        xil_printf("  formatted smaller than the media (Format NVM can resize).\r\n");
    }
    xil_printf("--- end capacity diagnostic ---\r\n\r\n");
}


/****************************************************************************/
/**
* This function is the entry point for PCIe Root Complex Enumeration Example
*
* @param 	None
*
* @return
*		- XST_SUCCESS if successful
*		- XST_FAILURE if unsuccessful.
*
* @note 	None.
*
*****************************************************************************/
int main(void)
{

	int Status;
	int rc;

    /* STATIC on purpose: these two structures are 4 KiB EACH. As ordinary
     * locals they consume 8 KiB of stack by themselves, and the benchmark's
     * call chain needs another ~2 KiB on top. That combination overflowed the
     * old 8 KiB stack region, ran through the 1 KiB heap and silently
     * overwrote the last ~672 bytes of the DMA buffer that happened to sit
     * below it (channel 1's) - a "data corruption" that looked like a
     * driver/PCIe/drive fault for several sessions. They are only used for
     * printing, so static is the right home for them.                    */
    static XNvmeIdentifyController idc;
    static XNvmeIdentifyNamespace  idn;
    /* One DDR region per channel - in real use these are filled by the PL
     * (4 high-speed data streams). Here we fill them from the MicroBlaze.
     * 4 KiB-aligned so they also work in PRP mode without padding.       */
	#define NCH              XNVMEHA_ACTIVE_QUEUES
	#define CHAN_BYTES       (256u * 1024u)          /* 256 KiB per channel */
    static u8 chan_buf[NCH][CHAN_BYTES] __attribute__((aligned(4096)));
    static u8 rd_buf[CHAN_BYTES]        __attribute__((aligned(4096)));
    const void *bufs[NCH];
    u64 lbas[NCH];
    u32 nlbs[NCH], lens[NCH];
    u32 ch, i, nlb_per_ch, fail;
    u32 idc_ok = 0u;    /* Identify Controller data valid (for diagnostic) */

	/* Boot breadcrumb. This is the FIRST thing main() does, before any PCIe
	 * or DDR setup, so it splits "the program never got here" from "it died
	 * inside the demo":
	 *   printed     -> crtinit finished, DDR and the UART work; the fault is
	 *                  in whatever ran next (watch for the following line);
	 *   not printed -> we never reached main(): FPGA/DDR state, a stale
	 *                  download, or the .bss clear in crtinit.
	 * Keep it - it costs one line of output and saves a whole round trip. */
	xil_printf("\r\nBOOT: main() entered\r\n");

	/* Enable the caches. Until now NOTHING in this application ever called
	 * these, so every instruction fetch has been an AXI read to DDR and
	 * every DDR store an uncached single beat - that dominated all earlier
	 * measurements (e.g. the SW-port probe numbers). Safe here by config:
	 * the cacheable window is 0x80000000-0xFFFFFFFF (DDR only,
	 * xparameters.h XPAR_MICROBLAZE_D/I_CACHE_BASEADDR) and every device
	 * region (swaxi 0x44A00000, NVMeHA regs, UART, timer, PCIe BARs) sits
	 * below it, so Xil_Out32/Xil_In32 keep their uncached semantics. The
	 * driver's coherency code (PRP-list flush, read-buffer invalidate,
	 * admin SQE/CQE maintenance) has always been in place and now becomes
	 * load-bearing.                                          */
	Xil_ICacheEnable();
	// Xil_DCacheEnable();

	/* Arm the stack-overflow canary as early as possible: from here on, any
	 * call chain that runs below the stack's bottom edge is caught and
	 * reported rather than silently corrupting whatever lies there. */
	(void)XNvme_StackCanaryFill();
	(void)XNvme_StackCanaryCheck("boot");

	/* Bring up the benchmark's time base, and prove it against the UART
	 * before anything else runs: the self-test prints its verdict here, so a
	 * wrong timer frequency (or a baud rate that does not match the hardware)
	 * is reported before any measurement is believed rather than afterwards. */
	(void)XNvme_TimerInit();
	(void)XNvme_TimerSelfTest();

	/* Initialize Root Complex */
#ifndef SDT
	Status = PcieInitRootComplex(&AxiPcieInstance, AXIPCIE_DEVICE_ID);
#else
	Status = PcieInitRootComplex(&AxiPcieInstance, XPAR_XAXIPCIE_0_BASEADDR);
#endif

	if (Status != XST_SUCCESS) {
		xil_printf("Axipcie rc enumerate Example Failed\r\n");
		return XST_FAILURE;
	}

	/* Scan PCIe Fabric */
	PCIeEnumerateFabric(&AxiPcieInstance);

	xil_printf("Successfully ran Axipcie rc enumerate Example\r\n");


	/* ---- DEBUG: make sure the SSD endpoint is found & BAR0 mapped ----
	 * The generic scan above only printed the RC bridge; if it never entered
	 * the End Point branch, the SSD BAR0 was never assigned and a read of
	 * CAP via AXI 0x76000000 (PCIe 0x0) returns 0 because nothing answers
	 * there yet. Force-probe 01:00.0 and, if present, assign BAR0=0 and
	 * enable MEM/bus-master so the SSD registers become reachable at PCIe 0x0. */
	{
		u32 vid, ht, bar0, cmd;
		XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
					PCIE_CFG_ID_REG, &vid);
		xil_printf("DBG: 01:00.0 Vendor=0x%04X\r\n", vid & 0xFFFF);
		if (((vid & 0xFFFF) != 0xFFFF) && ((vid & 0xFFFF) != 0xDEE3)) {
			XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
					PCIE_CFG_CAH_LAT_HD_REG, &ht);
			XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
					PCIE_CFG_BAR_0_REG, &bar0);
			xil_printf("DBG: header=0x%08X BAR0=0x%08X\r\n", ht, bar0);

			XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
					PCIE_CFG_CMD_STATUS_REG, &cmd);
			cmd |= (PCIE_CFG_CMD_BUSM_EN | PCIE_CFG_CMD_MEM_EN);
			XAxiPcie_WriteRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
					PCIE_CFG_CMD_STATUS_REG, cmd);
			XAxiPcie_WriteRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
					PCIE_CFG_BAR_0_REG, PCIE_CFG_BAR_0_ADDR);
			xil_printf("DBG: SSD BAR0=0x%08X assigned, MEM+BusMaster enabled\r\n",
				   PCIE_CFG_BAR_0_ADDR);

			{
				u32 lo = Xil_In32((UINTPTR)XNVMEHA_SSD0_PCIE_BASE);
				u32 hi = Xil_In32((UINTPTR)XNVMEHA_SSD0_PCIE_BASE + 4);
				xil_printf("DBG: CAP@0x%08X = 0x%08X%08X\r\n",
					   (u32)XNVMEHA_SSD0_PCIE_BASE, hi, lo);
			}

			/* Negotiated PCIe link width and speed - the hard ceiling for any
			 * throughput number measured later. The link has to be at least as
			 * fast as whatever the benchmark reports, so print this before
			 * believing an MB/s figure. Plain config offsets instead of driver
			 * macros: 0x34 = capability pointer, then cap+0x0C = Link
			 * Capabilities (max) and cap+0x12 = Link Status (negotiated).
			 * The pointer and the capability id (0x10 = PCIe) are validated
			 * before anything is read.                                     */
			{
				u32 cap_ptr = 0u, cap_id = 0u, lnkcap = 0u, lnksta = 0u;
				static const u32 gts_tab[5] = { 0u, 2u, 5u, 8u, 16u };

				XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
							       0x34u, &cap_ptr);
				cap_ptr &= 0xFCu;
				if ((cap_ptr >= 0x40u) && (cap_ptr <= 0xF0u)) {
					XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
							       cap_ptr, &cap_id);
					if ((cap_id & 0xFFu) == 0x10u) {
						u32 mxw, mxs, ngw, ngs;

						XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
								       cap_ptr + 0x0Cu, &lnkcap);
						XAxiPcie_ReadRemoteConfigSpace(&AxiPcieInstance, 1, 0, 0,
								       cap_ptr + 0x12u, &lnksta);
						mxw = (lnkcap >> 4) & 0x3Fu;
						mxs = lnkcap & 0xFu;
						ngw = (lnksta >> 4) & 0x3Fu;
						ngs = lnksta & 0xFu;
						xil_printf("DBG: PCIe cap@0x%02X: max %u GT/s x%u | "
							   "NEGOTIATED %u GT/s x%u\r\n",
							   cap_ptr,
							   (mxs < 5u) ? gts_tab[mxs] : 0u, mxw,
							   (ngs < 5u) ? gts_tab[ngs] : 0u, ngw);
						xil_printf("DBG: link ceiling ~%u MB/s of payload - a "
							   "benchmark figure above this is an artefact\r\n",
							   ((ngs < 5u) ? gts_tab[ngs] : 0u) * ngw * 100u);
					}
				}
			}
		} else {
			xil_printf("DBG: 01:00.0 read 0x%04X = GARBAGE (not 0xFFFF, not a valid Vendor)." "\r\n",
				   (u16)(vid & 0xFFFF));
			xil_printf("DBG: root cause = axi_pcie3_0 S_AXI_CTL is only 16KB in the BD." "\r\n");
			xil_printf("DBG: PG194 Table 11 (C_BASEADDR) requires 256MB alignment in Root Port" "\r\n");
			xil_printf("DBG: mode, because ECAM bits[27:0] carry Bus/Dev/Fun. Any Bus>=1 ECAM" "\r\n");
			xil_printf("DBG: address (>=0x10100000) is outside the decoded window, so the read" "\r\n");
			xil_printf("DBG: floats instead of returning a config completion." "\r\n");
			xil_printf("DBG: FIX: in Vivado set the S_AXI_CTL range to 0x10000000-0x1FFFFFFF" "\r\n");
			xil_printf("DBG: (256M), re-export the XSA, then rebuild. See fix_addr_ranges.tcl." "\r\n");
		}
	}


	

    xil_printf("\r\n=== NVMe Host Accelerator demo (KU05P / MicroBlaze) ===\r\n");

    rc = XNvme_Init(&nvme_dev, 0, 0, XNVMEHA_SSD0_PCIE_BASE, 0);
    if (rc != XST_SUCCESS) {
        xil_printf("XNvme_Init failed: %d\r\n", rc);
        return rc;
    }
    xil_printf("NVMe initialised: %s mode, max_xfer=%u bytes, channels=%u\r\n",
               nvme_dev.use_sgl ? "SGL" : "PRP", nvme_dev.max_xfer,
               XNvme_NumChannels(&nvme_dev));

    rc = SetupInterrupts(&nvme_dev);
    if (rc == XST_SUCCESS) {
        XNvmeHa_SetInterruptMode(&nvme_dev.ha, 1);
    } else {
        xil_printf("WARNING: running without ISR (polling only)\r\n");
    }

    /* Identify controller + namespace 1. */
    rc = XNvme_IdentifyController(&nvme_dev, &idc);
    if (rc == XST_SUCCESS) {
        xil_printf("Model: ");    print_field(XNVME_IDC_MN(&idc), 40);
        xil_printf("\r\nSerial: "); print_field(XNVME_IDC_SN(&idc), 20);
        xil_printf("\r\nFirmware: "); print_field(XNVME_IDC_FR(&idc), 8);
        xil_printf("\r\n");
        idc_ok = 1u;
    }
    rc = XNvme_IdentifyNamespace(&nvme_dev, 1, &idn);
    if (rc != XST_SUCCESS) {
        xil_printf("Identify NS failed: %d\r\n", rc);
        return rc;
    }
    xil_printf("NS1 size (LBAs): "); print_u64(nvme_dev.ns_size);
    xil_printf("\r\nNS1 LBA size: %u bytes\r\n", nvme_dev.lba_size);

    /* Capacity cross-check: tells "namespace just formatted small" apart from
     * "controller cannot see its own media" (SMI ROM/safe mode). Needs both
     * the Identify Controller and Identify Namespace data. */
    if (idc_ok)
        PrintCapacityDiag(&idc, &idn);

    /* ---- Direct-path sanity check: can the SSD finish an I/O at all?
     * FLUSH (no data) then WRITE (with PRP1), with the NVMe Host Accelerator
     * completely out of the loop (own queue pair in DDR, doorbell rung by the
     * MicroBlaze). It splits "the drive does not service I/O queues" from
     * "the data DMA hangs" from "the IP's SSD-side path is broken" - see the
     * comment block in xnvme.c. LBA 0x400000 is clear of the per-channel
     * regions used below. Diagnostic only.                              */
    xil_printf("\r\n--- direct I/O test (NVMeHA out of the loop) ---\r\n");
    rc = XNvme_DirectIoTest(&nvme_dev, 0x400000ULL);
    xil_printf("--- direct I/O test: %s ---\r\n\r\n",
               (rc == XST_SUCCESS) ? "at least one command completed"
                                   : "NO CLEAN COMPLETION (see lines above)");

    /* ---- IP-side diagnostics: OFF by default, and they must be off for any
     * run whose result you intend to believe.
     *
     * Both of them leave the IP's own queues in a state it cannot clean up
     * by itself:
     *   - XNvme_IpPathProbe() rings SQ1TDBL by hand. The IP had already rung
     *     it, so the SSD fetches the same SQE twice and the IP's internal
     *     SQ1 pointers end up behind the SSD's. After that the IP will not
     *     issue anything else from SQ1: the entry just sits in its FIFO
     *     (SQm-Empty=0, bit13 response-timeout set) - which is exactly what
     *     the 17:26 log shows for SQ1 when the demo's own write ran.
     *   - XNvme_IpSqFetchTest() points qid 4 at the IP's SQ4 with the CQ in
     *     DDR, so the IP never sees that completion: SQ4 stays "issued,
     *     never completed" for the rest of the session.
     *
     * They have done their job: they proved the SSD fetches SQEs out of the
     * IP's own memory and completes them (the 32 KB S_AXI_SSD window is
     * fine and the CQE write into 0x44A24000 does land - the statistics
     * counters `issued=1 completed=1` right after the push say so). Set
     * this to 1 only when you want to re-prove that, and power-cycle the
     * board before trusting any functional result. */
#ifndef XNVME_RUN_IP_DIAGNOSTICS
#define XNVME_RUN_IP_DIAGNOSTICS   0
#endif
#if (XNVME_RUN_IP_DIAGNOSTICS == 1)
    (void)XNvme_IpPathProbe(&nvme_dev, 0x600000ULL);
    (void)XNvme_IpSqFetchTest(&nvme_dev, 0x700000ULL);
#endif

    /* ---- Ask the drive itself: last error-log entry + SMART counters.
     * The admin queue keeps working while I/O hangs, so this is the only open
     * window into what the drive thinks is wrong.                        */
    xil_printf("--- drive logs (admin queue still works) ---\r\n");
    (void)XNvme_AdminLogDump(&nvme_dev);
    xil_printf("--- end drive logs ---\r\n\r\n");

    if (XNvme_NumChannels(&nvme_dev) < NCH) {
        xil_printf("Need %u SW queues; set C_NUM_SQ_SW>=%u in config\r\n",
                   NCH, NCH);
        return XST_FAILURE;
    }
	
	/* Fill each channel's DDR region with a distinct pattern, then write
     * all 4 channels concurrently - one SQ per channel - for throughput.
     * Each channel's transfer is split at MDTS internally.              */
    nlb_per_ch = CHAN_BYTES / nvme_dev.lba_size;
    for (ch = 0u; ch < NCH; ch++) {
        for (i = 0u; i < CHAN_BYTES; i++)
            chan_buf[ch][i] = (u8)((i + ch * 17u) & 0xFFu);
        bufs[ch] = chan_buf[ch];
        lens[ch] = CHAN_BYTES;
        nlbs[ch] = nlb_per_ch;
        lbas[ch] = 0x100000ULL + (u64)ch * 0x100000ULL; /* disjoint LBAs */
    }

    xil_printf("Writing %u channels x %u bytes concurrently...\r\n",
               NCH, CHAN_BYTES);
    rc = XNvme_WriteChannels(&nvme_dev, NCH, lbas, nlbs, bufs, lens);
    if (rc != XST_SUCCESS) {
        xil_printf("Multi-channel write failed: %d\r\n", rc);
        return rc;
    }
    rc = XNvme_Flush(&nvme_dev);
    if (rc != XST_SUCCESS)
        xil_printf("Flush failed: %d\r\n", rc);

    /* Read each channel back via its own queue and verify. */
    fail = 0u;
    for (ch = 0u; ch < NCH; ch++) {
        int cid = XNvme_ReadChannelAsync(&nvme_dev, ch, lbas[ch],
                                         (u16)nlb_per_ch, rd_buf, CHAN_BYTES);
        if (cid < 0) { xil_printf("CH%u read submit failed\r\n", ch); fail++; continue; }
        if (XNvme_WaitIo(&nvme_dev, cid) != XST_SUCCESS) {
            xil_printf("CH%u read failed\r\n", ch); fail++; continue;
        }
        for (i = 0u; i < CHAN_BYTES; i++) {
            if (rd_buf[i] != chan_buf[ch][i]) {
                xil_printf("CH%u verify mismatch @ %u\r\n", ch, i);
                fail++;
                break;
            }
        }
    }
    if (fail == 0u)
        xil_printf("Verify OK (%u channels x %u bytes).\r\n", NCH, CHAN_BYTES);

    /* ---- Throughput benchmark: generate the data in DDR, time a write
     * pass and a read pass, then verify what came back. It uses its own
     * static buffers (XNVME_BENCH_BYTES per channel in xnvme.c, 8 MiB by
     * default - raise it there for longer runs) and writes from LBA
     * 0x800000 upward, i.e. its own scratch region. Set this to 0 when you
     * only want the functional demo.                                     */
#ifndef XNVME_RUN_BENCH
#define XNVME_RUN_BENCH     1
#endif
#if (XNVME_RUN_BENCH == 1)
    (void)XNvme_Bench(&nvme_dev, NCH, 0x800000ULL,
                      (u32)XNVME_BENCH_BYTES, (u32)XNVME_BENCH_CHUNK);
#endif

    /* Final stack check: if this ever reports a disturbance, the corruption
     * is the CPU's own stack, not the drive or the IP. */
    (void)XNvme_StackCanaryCheck("end of demo");
    xil_printf("Demo complete.\r\n");
    return XST_SUCCESS;
}

/****************************************************************************/
/**
* This function initializes a AXI PCIe IP built as a root complex
*
* @param	AxiPciePtr is a pointer to an instance of XAxiPcie data
*		structure represents a root complex IP.
* @param 	DeviceId is AXI PCIe IP unique ID
*
* @return
*		- XST_SUCCESS if successful.
*		- XST_FAILURE if unsuccessful.
*
* @note 	None.
*
*
******************************************************************************/
#ifndef SDT
int PcieInitRootComplex(XAxiPcie *AxiPciePtr, u16 DeviceId)
#else
int PcieInitRootComplex(XAxiPcie *AxiPciePtr, UINTPTR BaseAddress)
#endif
{
	int Status;
	u32 HeaderData;
	u32 InterruptMask;
	u8  BusNumber;
	u8  DeviceNumber;
	u8  FunNumber;
	u8  PortNumber;

	XAxiPcie_Config *ConfigPtr;
#ifndef SDT
	ConfigPtr = XAxiPcie_LookupConfig(DeviceId);
#else
	ConfigPtr = XAxiPcie_LookupConfig(BaseAddress);
#endif


	Status = XAxiPcie_CfgInitialize(AxiPciePtr, ConfigPtr,
						ConfigPtr->BaseAddress);

	if (Status != XST_SUCCESS) {
		xil_printf("Failed to initialize PCIe Root Complex"
							"IP Instance\r\n");
		return XST_FAILURE;
	}

	if(!AxiPciePtr->Config.IncludeRootComplex) {
		xil_printf("Failed to initialize...AXI PCIE is configured"
							" as endpoint\r\n");
		return XST_FAILURE;
	}


	/* See what interrupts are currently enabled */

	/*
	 * REQUIRED when using the Gen2 axipcie driver on the Gen3 axi_pcie3 core.
	 *
	 * XAxiPcie_CfgInitialize() derives MaxNumOfBuses from the Bridge Info
	 * register's "ECAM Size" field (XAXIPCIE_BI_ECAM_SIZE_MASK = 0x00070000
	 * at offset 0x130). On the Gen3 bridge that field does not exist at all:
	 * PG194 (v3.0) Table 14 "Bridge Info Register" defines only bit0 (Gen2
	 * Capable), bit1 (Root Port Present) and bit3 (Gen3 Capable), with bits
	 * 31:4 Reserved and reading 0. So MaxNumOfBuses comes out 0, and both
	 * XAxiPcie_ReadRemoteConfigSpace() and WriteRemoteConfigSpace() then
	 * reject every Bus > 0 access and hand back 0xFFFFFFFF -- the endpoint
	 * at 01:00.0 is never reached and NVMe CAP reads back 0.
	 *
	 * Set it to the subordinate bus number programmed below in
	 * PCIE_CFG_PRIM_SEC_BUS (7). The driver's guard is "Bus > MaxNumOfBuses",
	 * so buses 0..7 are permitted, which matches exactly the range the
	 * bridge will honour (PG194 p.57: ECAM accesses outside
	 * secondary..subordinate produce no config request and signal SLVERR).
	 * Using 256 here would instead let software walk buses the bridge
	 * rejects.
	 *
	 * Also normalise IncludeRootComplex: the SDT-generated config carries the
	 * raw port type (2), while the driver compares against the boolean
	 * XAXIPCIE_IS_RC == 1.
	 */
	if (AxiPciePtr->Config.IncludeRootComplex) {
		AxiPciePtr->Config.IncludeRootComplex = XAXIPCIE_IS_RC;
	}
	AxiPciePtr->MaxNumOfBuses = 7;

	XAxiPcie_GetEnabledInterrupts(AxiPciePtr, &InterruptMask);
	xil_printf("Interrupts currently enabled are %8X\r\n", InterruptMask);

	/* Make sure all interrupts disabled. */
	XAxiPcie_DisableInterrupts(AxiPciePtr, XAXIPCIE_IM_ENABLE_ALL_MASK);


	/* See what interrupts are currently pending */
	XAxiPcie_GetPendingInterrupts(AxiPciePtr, &InterruptMask);
	xil_printf("Interrupts currently pending are %8X\r\n", InterruptMask);

	/* Just if there is any pending interrupt then clear it.*/
	XAxiPcie_ClearPendingInterrupts(AxiPciePtr,
						XAXIPCIE_ID_CLEAR_ALL_MASK);

	/*
	 * Read enabled interrupts and pending interrupts
	 * to verify the previous two operations and also
	 * to test those two API functions
	 */

	XAxiPcie_GetEnabledInterrupts(AxiPciePtr, &InterruptMask);
	xil_printf("Interrupts currently enabled are %8X\r\n", InterruptMask);

	XAxiPcie_GetPendingInterrupts(AxiPciePtr, &InterruptMask);
	xil_printf("Interrupts currently pending are %8X\r\n", InterruptMask);

	/* Make sure link is up. */
	Status = XAxiPcie_IsLinkUp(AxiPciePtr);
	if (Status != TRUE ) {
		xil_printf("Link is not up\r\n");
		return XST_FAILURE;
	}

	xil_printf("Link is up\r\n");

	/*
	 * Read back requester ID.
	 */
	XAxiPcie_GetRequesterId(AxiPciePtr, &BusNumber,
				&DeviceNumber, &FunNumber, &PortNumber);

	xil_printf("Bus Number is %02X\r\n"
			"Device Number is %02X\r\n"
	 			"Function Number is %02X\r\n"
	 				"Port Number is %02X\r\n",
	 		BusNumber, DeviceNumber, FunNumber, PortNumber);


	/* Set up the PCIe header of this Root Complex */
	XAxiPcie_ReadLocalConfigSpace(AxiPciePtr,
					PCIE_CFG_CMD_STATUS_REG, &HeaderData);

	HeaderData |= (PCIE_CFG_CMD_BUSM_EN | PCIE_CFG_CMD_MEM_EN |
				PCIE_CFG_CMD_IO_EN | PCIE_CFG_CMD_PARITY |
							PCIE_CFG_CMD_SERR_EN);

	XAxiPcie_WriteLocalConfigSpace(AxiPciePtr,
					PCIE_CFG_CMD_STATUS_REG, HeaderData);

	/*
	 * Read back local config reg.
	 * to verify the write.
	 */

	XAxiPcie_ReadLocalConfigSpace(AxiPciePtr,
					PCIE_CFG_CMD_STATUS_REG, &HeaderData);

	xil_printf("PCIe Local Config Space is %8X at register"
					" CommandStatus\r\n", HeaderData);

	/*
	 * Set up Bus number
	 */

	HeaderData = PCIE_CFG_PRIM_SEC_BUS;

	XAxiPcie_WriteLocalConfigSpace(AxiPciePtr,
					PCIE_CFG_PRI_SEC_BUS_REG, HeaderData);

	/*
	 * Read back local config reg.
	 * to verify the write.
	 */
	XAxiPcie_ReadLocalConfigSpace(AxiPciePtr,
					PCIE_CFG_PRI_SEC_BUS_REG, &HeaderData);

	xil_printf("PCIe Local Config Space is %8X at register "
					"Prim Sec. Bus\r\n", HeaderData);

	/* Now it is ready to function */

	xil_printf("Root Complex IP Instance has been successfully"
							" initialized\r\n");

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
* This function enumerates its PCIe system and figures out the nature of each
* component there like end points,bridges,...
*
* @param 	AxiPciePtr is a pointer to an instance of XAxiPcie
*		data structure represents a root complex IP.
*
* @return 	None.
*
* @note 	None.
*
******************************************************************************/
void PCIeEnumerateFabric(XAxiPcie *AxiPciePtr)
{

	u32 ConfigData;
	u32 PCIeHeaderType;
	u32 PCIeMultiFun;
	u32 PCIeBusNum;
	u32 PCIeDevNum;
	u32 PCIeFunNum;
	u16 PCIeVendorID;
	u32 RegVal;

	xil_printf("Start Enumeration of PCIe Fabric on This System\r\n");

	/* Scan PCIe Fabric */

	for (PCIeBusNum = 0; PCIeBusNum < PCIE_CFG_MAX_NUM_OF_BUS;
								PCIeBusNum++) {
		for (PCIeDevNum = 0; PCIeDevNum < PCIE_CFG_MAX_NUM_OF_DEV;
								PCIeDevNum++) {
			for (PCIeFunNum = 0;
				PCIeFunNum < PCIE_CFG_MAX_NUM_OF_FUN;
								PCIeFunNum++) {

				/* Vendor ID */
				XAxiPcie_ReadRemoteConfigSpace(
					AxiPciePtr,PCIeBusNum,
					PCIeDevNum, PCIeFunNum,
					PCIE_CFG_ID_REG, &ConfigData);

				PCIeVendorID = (u16) (ConfigData & 0xFFFF);

				if (PCIeVendorID ==
						PCIE_CFG_FUN_NOT_IMP_MASK) {
					if (PCIeFunNum == 0)
					/*
					 * We don't need to look
					 * any further on this device.
					 */
					break;
				}
				else {
					xil_printf("PCIeBus is %02X\r\n"
						"PCIeDev is %02X\r\n"
						"PCIeFunc is %02X\r\n",
						PCIeBusNum, PCIeDevNum,
								PCIeFunNum);

					xil_printf("Vendor ID is %04X \r\n",
								PCIeVendorID);

					/* Header Type */
					XAxiPcie_ReadRemoteConfigSpace(
						AxiPciePtr, PCIeBusNum,
						PCIeDevNum, PCIeFunNum,
						PCIE_CFG_CAH_LAT_HD_REG,
						&ConfigData);

					PCIeHeaderType = ConfigData &
						PCIE_CFG_HEADER_TYPE_MASK;

					PCIeMultiFun = ConfigData &
						PCIE_CFG_MUL_FUN_DEV_MASK;

					if (PCIeHeaderType ==
						PCIE_CFG_HEADER_O_TYPE) {
						/* This is an End Point */
						xil_printf("This is an "
							"End Point\r\n");

						/*
						 * Initialize this end point
						 * and return.
						 */

						XAxiPcie_ReadRemoteConfigSpace(
							AxiPciePtr,
							PCIeBusNum, PCIeDevNum,
							PCIeFunNum,
						PCIE_CFG_CMD_STATUS_REG,
								&ConfigData);

						ConfigData |=
						(PCIE_CFG_CMD_BUSM_EN |
							PCIE_CFG_CMD_MEM_EN);

						XAxiPcie_WriteRemoteConfigSpace
							(AxiPciePtr,
							PCIeBusNum, PCIeDevNum,
							PCIeFunNum,
						PCIE_CFG_CMD_STATUS_REG,
								ConfigData);

						/*
						 * Write Address to
						 * PCIe BAR0
						 */
						ConfigData = PCIE_CFG_BAR_0_ADDR; /* BAR0 = PCIe 0x0 (AXI 0x76000000) */

						XAxiPcie_WriteRemoteConfigSpace
						(AxiPciePtr,
						PCIeBusNum, PCIeDevNum,
						PCIeFunNum, PCIE_CFG_BAR_0_REG,
						ConfigData);

						xil_printf("End Point has been"
							" enabled\r\n");

					}
					else {
						/* This is a bridge */
						xil_printf("This is a "
								"Bridge\r\n");
					}
				}

				if ((!PCIeFunNum) && (!PCIeMultiFun)) {
					/*
					 * If it is function 0 and it is not a
					 * multi function device, we don't need
					 * to look any further on this devie
					 */
					break;
				}
			}  /* Functions in one device */
		}  /* Devices on the same bus */
	}  /* Buses in the same system */

	xil_printf("End of Enumeration of PCIe Fabric on This system\r\n");

	/* Bridge enable */
	XAxiPcie_GetRootPortStatusCtrl(AxiPciePtr, &RegVal);
	RegVal |= XAXIPCIE_RPSC_BRIDGE_ENABLE_MASK;
	XAxiPcie_SetRootPortStatusCtrl(AxiPciePtr, RegVal);

	return;
}

