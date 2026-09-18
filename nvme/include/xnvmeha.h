/*
 * xnvmeha.h
 *
 * Driver for the Xilinx NVMe Host Accelerator (PG328) - the part that
 * manages the I/O submission/completion queues exposed to software.
 *
 * This driver:
 *   - programs the IP register space (PCIe base, SQ/CQ base, interrupts),
 *   - pushes NVMe I/O submission queue entries (SQEs) over the SW_AXI4
 *     memory-mapped interface,
 *   - pops completion queue entries (CQEs) and dispatches them to the
 *     originator by command id,
 *   - implements the credit-based flow control mandated by the IP,
 *   - services the hc_interrupt line (usable from an ISR or a poll loop).
 *
 * The admin queue (controller bring-up, Create I/O CQ/SQ, Identify) is
 * handled in xnvme_admin.* and is independent of this module.
 */

#ifndef XNVMEHA_H_
#define XNVMEHA_H_

#include "xil_types.h"
#include "xstatus.h"
#include "xnvmeha_hw.h"
#include "xnvme_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default instance base addresses (override via XNvmeHa_Init). */
#define XNVMEHA_DEFAULT_REG_BASE      XNVMEHA_REG_BASE
#define XNVMEHA_DEFAULT_SW_AXI_BASE   XNVMEHA_SW_AXI_BASE

/* Completion callback invoked when a command completes.
 *   cid     - command identifier (= slot index)
 *   status  - raw status word bits [28:17] (SCT|SC|DNR packed); 0 == success
 *   result0 - CQE DW0 (command-specific result, low)
 *   result1 - CQE DW1 (command-specific result, high)
 *   sqid    - SQ ID reported in the CQE
 */
typedef void (*XNvmeCompletionCb)(u16 cid, u32 status, u32 result0,
                                  u32 result1, u16 sqid);

/* A pending-command slot. */
typedef struct {
    volatile u8  in_use;     /* slot occupied by an outstanding command  */
    volatile u8  done;       /* completion received                       */
    volatile u16 ssd;        /* target SSD (for credit accounting)        */
    volatile u32 status;     /* raw CQE status field                      */
    volatile u32 result0;    /* CQE DW0                                   */
    volatile u32 result1;    /* CQE DW1                                   */
    volatile u16 sqid;       /* CQE SQ ID                                 */
    void        *buf;        /* data buffer (for post-read invalidate)    */
    u32          xfer_len;   /* transfer length in bytes                  */
    u8           is_read;    /* 1 = read (invalidate buf on completion)   */
} XNvmeCmdSlot;

/* Driver instance. */
typedef struct {
    u32                  reg_base;     /* AXI4-Lite register base       */
    u32                  sw_axi_base;  /* SW_AXI4 memory-mapped base    */
    volatile u32         sw_credit[XNVMEHA_MAX_SSD]; /* per-SSD credit  */
    volatile u16         next_cid;     /* round-robin CID hint          */
    volatile u32         cq_processed; /* total CQEs processed (stat)   */
    volatile u32         cq_errors;    /* CQEs with non-zero status     */
    volatile u8          int_mode;     /* 1 = ISR services CQEs, 0 = poll */
    u8                   use_sgl;      /* 1 = SGL (partial cmd), 0 = PRP (full cmd) */
    u32                  prp_page_size;/* MPS page size for PRP building (2^(12+mps)) */
    XNvmeCmdSlot         slot[XNVMEHA_MAX_OUTSTANDING];
    XNvmeCompletionCb    completion_cb;
} XNvmeHa;

/* ----------------- Lifecycle ----------------- */

/* Initialise the instance and program the IP registers:
 *   - per-SSD PCIe base addresses,
 *   - SSD0 SQ0 / CQ0 PCIe base addresses,
 *   - timeout register,
 *   - interrupt-enable register (CQ available + error sources).
 * Does NOT touch the SSD (no admin commands).                   */
int  XNvmeHa_Init(XNvmeHa *ha, u32 reg_base, u32 sw_axi_base);

/* Reset the per-SSD credit counters to their initial values:
 *   credit = num_sw_queues * (depth - 1)
 * Call after the I/O queues have been created on the SSD.       */
void XNvmeHa_ResetCredit(XNvmeHa *ha);

/* ----------------- SQE submission ----------------- */

/* Push a partial (32-byte) I/O SQE to SSDn / SQm. Returns the allocated
 * CID, or -1 if no credit / no free slot. The SQE opcode/NSID/LBA/SGL
 * must already be filled in. With SW_QOS_EN=1 `sq` selects the exact
 * NVMe I/O queue (0 -> qid 1, 1 -> qid 2, ...); with QoS off it is
 * ignored (the IP round-robins). Caller handles cache of the data buf. */
int  XNvmeHa_PushPartialSqe(XNvmeHa *ha, u32 ssd, u32 sq,
                            const XNvmePartialSqe *sqe);

/* Push a full (64-byte) NVMe SQE to SSDn / SQm.
 *
 * `prp_buf`/`prp_len` describe the data buffer, if any. The PRP fields are
 * filled in HERE, after the command's slot (CID) has been allocated, because
 * each in-flight command needs its own PRP list - pass NULL/0 for commands
 * without data (FLUSH). Pass a non-NULL buffer and the function ignores
 * whatever PRP values the caller put in the SQE.
 *
 * Returns the allocated CID, or -1 if no credit / no free slot.            */
int  XNvmeHa_PushFullSqe(XNvmeHa *ha, u32 ssd, u32 sq,
                         const XNvmeFullSqe *sqe,
                         void *prp_buf, u32 prp_len);

/* ----------------- Asynchronous I/O (for multi-queue pipelining) ---- */

/* Build + submit one I/O command (read/write/flush) to SSDn / SQm without
 * waiting. Handles cache pre-op (flush for write, invalidate for read)
 * and SGL/PRP selection. Returns the CID (>0) or negative on error.
 * `nlb` must fit a single command (caller splits at MDTS for larger). */
int  XNvmeHa_SubmitIo(XNvmeHa *ha, u32 ssd, u32 sq, u32 nsid, u64 lba,
                      u16 nlb, void *buf, u32 len, u8 opcode);

/* Wait for a command submitted via XNvmeHa_SubmitIo to complete, then
 * finalize it: invalidate the buffer for reads and free the slot.
 * Returns XST_SUCCESS on clean completion, XST_TIMEOUT, or XST_FAILURE
 * on NVMe status error.                                              */
int  XNvmeHa_WaitIo(XNvmeHa *ha, int cid);

/* ----------------- Completion handling ----------------- */

/* Service pending interrupts. Reads INT_STATUS; if CQs are available it
 * pops the reported number of CQEs and dispatches them, then clears the
 * status bits. Safe to call from the hc_interrupt ISR or from a poll
 * loop. Returns the number of CQEs processed.                    */
u32  XNvmeHa_ServiceInterrupts(XNvmeHa *ha);

/* Poll fallback for designs that do not wire hc_interrupt to an ISR.
 * Checks INT_STATUS and calls XNvmeHa_ServiceInterrupts if needed.     */
u32  XNvmeHa_PollCompletions(XNvmeHa *ha);

/* Wait synchronously for the command in slot `cid` to complete (with
 * timeout). Processes completions while waiting. Returns XST_SUCCESS on
 * completion, XST_TIMEOUT otherwise. The slot's status/result fields are
 * available in ha->slot[cid].                                  */
int  XNvmeHa_WaitComplete(XNvmeHa *ha, int cid);

/* Release a slot after the caller is done reading its result.        */
void XNvmeHa_FreeSlot(XNvmeHa *ha, int cid);

/* ----------------- Synchronous I/O convenience ----------------- */

/* Build+push a partial read/write/flush SQE and block until it completes.
 *   ssd   - target SSD index
 *   nsid  - namespace id
 *   lba   - starting LBA
 *   nlb   - number of logical blocks (1-based count; 0 means 1 block)
 *   buf   - data buffer (PCIe-reachable DDR). For reads the buffer is
 *           invalidated before submission; for writes it is flushed.
 *   len   - buffer length in bytes (must be >= nlb * LBA-data-size).
 * Returns XST_SUCCESS / error.                                     */
int  XNvmeHa_SyncRead(XNvmeHa *ha, u32 ssd, u32 nsid, u64 lba,
                      u16 nlb, void *buf, u32 len);
int  XNvmeHa_SyncWrite(XNvmeHa *ha, u32 ssd, u32 nsid, u64 lba,
                       u16 nlb, const void *buf, u32 len);
int  XNvmeHa_SyncFlush(XNvmeHa *ha, u32 ssd, u32 nsid);

/* ----------------- Diagnostics ----------------- */

/* Read the SQ/CQ FIFO status register for SSDn/SQm. */
u32  XNvmeHa_GetQueueStatus(XNvmeHa *ha, u32 ssd, u32 sq);

/* Read the per-SSD issued / completed / error counters. */
void XNvmeHa_GetSsdStats(XNvmeHa *ha, u32 ssd,
                         u32 *issued, u32 *completed, u32 *errors);

/* Flush a single SQ (PG328: assert flush, wait for done, deassert). */
int  XNvmeHa_FlushQueue(XNvmeHa *ha, u32 ssd, u32 sq);

/* ---------------- SSD-side (IP internal memory) probes ----------------
 * These read the IP's own SQ/CQ buffers through the MicroBlaze alias of
 * S_AXI_SSD (XNVMEHA_SSD_ALIAS_BASE), i.e. the same memory the SSD fetches
 * SQEs from and writes CQEs into. Read-only, no side effects on the queues
 * (the documented side-effecting access is the SW CQ pop, not this one).  */

/* Print the IP-derived SQm/CQm PCIe base addresses, the interrupt / SW-CQ
 * count / SQ-CQ-status registers and the first 64 B of the SQm slot 0 plus
 * the first 32 B of the CQm entry 0, all through the alias. `tag` is a
 * short label so several dumps in one run stay distinguishable.          */
void XNvmeHa_DumpSsdMemory(XNvmeHa *ha, u32 ssd, u32 sq, const char *tag);

/* Ring the SSD's SQyTDBL from the MicroBlaze (PG328 Table 6 is the IP doing
 * the same thing over its own m_axi_ssd). `qid` is the NVMe queue id, i.e.
 * 0 for admin, 1.. for I/O, `tail` the new tail value. Used to force the
 * notification the IP should have issued, so "the IP never told the SSD"
 * can be told apart from "the SSD cannot reach the IP's memory".          */
void XNvmeHa_RingSsdSqDoorbell(u32 ctrl_base, u32 dstrd, u32 qid, u32 tail);

/* Read back the interrupt-status and SW-available-CQ-count registers
 * without consuming anything (for polling in a diagnostic loop).          */
u32  XNvmeHa_GetIntStatus(XNvmeHa *ha);
u32  XNvmeHa_GetCqAvail(XNvmeHa *ha);

/* Command error count (0x8110, PG328 Table 21). A plain AXI4-Lite register,
 * NOT part of the statistics block, so it is live even with
 * statistics_en = 0: it increments whenever the IP rejects a command coming
 * from the SW or HW interface.                                           */
u32  XNvmeHa_GetCmdErrCount(XNvmeHa *ha);

/* Overwrite the interrupt-enable mask (0x8104) and return the previous value.
 * Used by diagnostics that need the error sources to stay out of the ISR
 * while they watch the status register: a masked source still latches in
 * INT_STATUS, it just no longer asserts hc_interrupt. Call it again with the
 * returned value to restore.                                            */
u32  XNvmeHa_SetIntEnable(XNvmeHa *ha, u32 mask);

/* Runtime on/off switch for the verbose bring-up logs (push@ / pop / CQE
 * lines). They cost ~30-40 ms each at 9600 baud, which dominates any
 * throughput measurement - turn them off around a timed region, back on
 * afterwards. Error logs are not affected. 1 = verbose (default).       */
void XNvmeHa_SetVerbose(u32 on);

/* Per-command D-cache maintenance on/off (default 1 = on). Only turn it off
 * if the caller does the coherency work itself AND the D-cache is
 * write-through (this design: pl.dtsi dcache-use-writeback = 0):
 *   - writes then need nothing (DDR already holds the store),
 *   - reads need ONE invalidate per buffer after the transfer.
 * Reason to use it: the flush/invalidate costs O(bytes/cache-line) inside a
 * BSP library call, which a benchmark clock cannot see - so a throughput
 * measurement either disables it (pure I/O figure) or reports a number that
 * silently includes it.                                                 */
void XNvmeHa_SetCacheOps(u32 on);

/* Per-iteration hook called from the completion-wait spin loop and from the
 * submit-retry loop. NULL by default. XNvme_Bench() in xnvme.c installs a
 * clock here while it measures throughput.                             */
extern void (*XNvmeHa_WaitHook)(void);

/* Register a completion callback (optional; synchronous APIs do not need
 * one because they use the slot done flag).                        */
void XNvmeHa_SetCompletionCb(XNvmeHa *ha, XNvmeCompletionCb cb);

/* Tell the driver whether a real ISR services the hc_interrupt line.
 * Call with enable=1 AFTER registering your ISR (so the synchronous
 * wait path does not also poll and race the ISR). Default is 0 (poll).*/
void XNvmeHa_SetInterruptMode(XNvmeHa *ha, u8 enable);

/* Select the data-pointer encoding for I/O commands:
 *   use_sgl=1 -> partial 32 B command with an SGL Data Block descriptor
 *                (one descriptor describes a whole contiguous buffer;
 *                multi-page is supported by the partial format).
 *   use_sgl=0 -> full 64 B command with PRP1 / PRP2 (PRP list when the
 *                buffer spans more than two pages). Use this only if the
 *                SSD does not support SGL.                              */
void XNvmeHa_SetTransferMode(XNvmeHa *ha, u8 use_sgl);

#ifdef __cplusplus
}
#endif

#endif /* XNVMEHA_H_ */
