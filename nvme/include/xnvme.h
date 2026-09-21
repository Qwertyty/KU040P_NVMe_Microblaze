/*
 * xnvme.h
 *
 * High-level NVMe control-flow API for the Xilinx NVMe Host Accelerator
 * (PG328) on MicroBlaze bare-metal.
 *
 * This layer orchestrates the full NVMe bring-up sequence:
 *   1. program the NVMe Host Accelerator IP (PCIe bases, SQ/CQ bases,
 *      interrupts)                       -> xnvmeha
 *   2. bring the SSD controller up via the admin queue (CC.EN/CSTS.RDY)
 *                                        -> xnvme_admin
 *   3. create the I/O CQ/SQ pairs the IP expects to exist on the SSD
 *   4. arm the credit-based flow control
 *
 * After XNvme_Init() succeeds, the read/write/flush/identify calls can be
 * issued. A single instance drives SSD0; instantiate more for more SSDs.
 */

#ifndef XNVME_H_
#define XNVME_H_

#include "xil_types.h"
#include "xstatus.h"
#include "xnvmeha.h"
#include "xnvme_admin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    XNvmeHa    ha;        /* IP driver state (I/O queues)            */
    XNvmeAdmin ad;        /* admin-queue state                       */
    u32        nsid;      /* active namespace id                     */
    u32        lba_size;  /* bytes per logical block                 */
    u64        ns_size;   /* namespace size in LBAs                  */
    u32        target_ssd;/* SSD index this instance drives (0..)    */
    u32        max_xfer;  /* max bytes per I/O command (MDTS); 0 = no limit */
    u8         sgl_supported; /* SSD reports SGL support (SGLS bit0) */
    u8         use_sgl;   /* 1 = use SGL (partial cmd); 0 = use PRP (full cmd) */
} XNvme;

/* Full bring-up of one SSD. `ssd_pcie_base` is the SSD BAR0 / NVMe
 * controller base over PCIe (the same value programmed into the IP's
 * PCIe-base register for that SSD). Pass 0 for reg_base / sw_axi_base to
 * use the defaults from xnvmeha_config.h.                          */
int  XNvme_Init(XNvme *nv, u32 reg_base, u32 sw_axi_base,
                u64 ssd_pcie_base, u32 ssd_index);

/* Identify the controller (optional, for information). */
int  XNvme_IdentifyController(XNvme *nv, XNvmeIdentifyController *out);

/* Identify a namespace and cache its LBA size / size in the instance.
 * Call this after Init to learn the LBA geometry of namespace 1.      */
int  XNvme_IdentifyNamespace(XNvme *nv, u32 nsid,
                             XNvmeIdentifyNamespace *out);

/* Synchronous I/O on the active namespace (nv->nsid). `nlb` is the
 * total 1-based block count (0 is treated as 1). The driver splits the
 * transfer at the controller's MDTS (and at the 65535-block NVMe limit)
 * and issues as many commands as needed, so a single call can move far
 * more than one command's worth of data (e.g. 1 MiB). `len` is the
 * buffer size in bytes and must be >= nlb * nv->lba_size.            */
int  XNvme_Read (XNvme *nv, u64 lba, u32 nlb, void *buf, u32 len);
int  XNvme_Write(XNvme *nv, u64 lba, u32 nlb, const void *buf, u32 len);
int  XNvme_Flush(XNvme *nv);

/* ----------------- Multi-channel (SW QoS) I/O -----------------
 * Requires XNVMEHA_SW_QOS_EN=1 in xnvmeha_config.h. Each channel maps
 * to its own NVMe I/O submission queue (channel 0 -> qid 1, 1 -> qid 2,
 * ...) so several channels can be in flight at once for throughput.
 * The SSD DMAs each channel's buffer directly from DDR; the MicroBlaze
 * only drives commands. Credit is shared per SSD across all channels.
 */

/* Number of usable SW channels (= C_NUM_SQ_SW). */
u32  XNvme_NumChannels(const XNvme *nv);

/* Submit ONE command (no wait) to channel `ch` (0..N-1). `nlb` must fit
 * a single command (<= max blocks per command). Returns the command id
 * (>=0) to pass to XNvme_WaitIo, or negative on error. The buffer must
 * be PCIe-reachable DDR.                                            */
int  XNvme_WriteChannelAsync(XNvme *nv, u32 ch, u64 lba, u16 nlb,
                             const void *buf, u32 len);
int  XNvme_ReadChannelAsync (XNvme *nv, u32 ch, u64 lba, u16 nlb,
                             void *buf, u32 len);

/* Wait for a command submitted with *ChannelAsync, finalize it (post-read
 * invalidate) and free its slot. Returns XST_SUCCESS / XST_TIMEOUT /
 * XST_FAILURE.                                                       */
int  XNvme_WaitIo(XNvme *nv, int cid);

/* Convenience: write `nch` channels concurrently. Each channel writes
 * nlb[ch] blocks from buf[ch] (len[ch] bytes) starting at lba[ch]. The
 * transfer is split at MDTS and the channels are pipelined round-by-round
 * so all queues stay busy. Returns XST_SUCCESS or the first error.    */
int  XNvme_WriteChannels(XNvme *nv, u32 nch,
                         const u64 *lba, const u32 *nlb,
                         const void *const *buf, const u32 *len);

/* ----------------- Throughput benchmark -----------------
 * Generate a pattern in DDR (one per channel), time a write pass and a read
 * pass over `bytes_per_ch` bytes per channel, verify, and print KB/s + MB/s +
 * commands/s. `chunk_bytes` should be the largest single command the IP
 * accepts (512 KiB) so each chunk is exactly one command; it must divide both
 * bytes_per_ch and the LBA size.
 *
 * Size knobs - these are shared with xnvme.c, so change them here:
 *   XNVME_BENCH_BYTES  bytes per channel the benchmark can touch, and the
 *                      size of its static buffers (costs .bss, 8 MiB per
 *                      channel by default -> 16 MiB for 2 channels).
 *                      Longer runs = better clock resolution.
 *   XNVME_BENCH_CHUNK  bytes per command (512 KiB = max_xfer).
 * Timing comes from the print UART (see the block comment in xnvme.c): the
 * BSP has no hardware timer and XTime_GetTime() is not implemented for the
 * MicroBlaze sleep timer, so the UART baud rate is the time base. Accuracy
 * is that of the board oscillator; resolution is 1/baud. Keep
 * XNVME_BENCH_BAUD equal to the UART's C_BAUDRATE.
 * The verbose bring-up logs are muted for the timed windows automatically.
 * Writes go to lbas derived from `lba0` upward - use a scratch region.
 * Returns XST_SUCCESS when both passes and the verify succeed.          */
#define XNVME_BENCH_BYTES      (128u * 1024u * 1024u)     /* per channel */
#define XNVME_BENCH_CHUNK      (512u * 1024u)           /* = max_xfer  */
/* -----------------The benchmark clock ----------------
 * XNvme_TimerInit() brings axi_timer_0 (100 MHz, 10 ns/tick here) up as a
 * free-running counter, driving its registers directly - so nothing depends on
 * which timer the BSP's xiltimer was configured with. It first proves that a
 * counter is really there, by checking that the counter MOVES: a mere
 * "write the enable bit, read it back" test is worthless on this design,
 * because a read from an unmapped address does not fault (the BSP's default
 * handler for the data-bus exception returns), so it silently yields stale
 * data and the failure shows up later as every measurement reading zero.
 *
 * XNvme_TimerInit()  - idempotent; returns 0 when timer 0 is counting, -1 when
 *                      it is not. Either way the benchmark's clock source is
 *                      decided inside: the timer when it works, else the UART
 *                      TX FIFO stopwatch (XNVME_BENCH_CLK_SRC = 2, auto).
 * XNvme_TimerNow()   - raw counter, safe across its 32-bit wrap because callers
 *                      only take differences over short windows.
 * XNvme_TimerUs()    - convert a raw delta to microseconds (timer ticks only).
 * XNvme_TimerSelfTest() - times a known number of UART bytes and reports what
 *                      baud rate that IMPLIES, next to what the platform
 *                      claims. A UART Lite's divider is a synthesis-time
 *                      constant, so a 24x disagreement means the FPGA is
 *                      running a bitstream built before the C_BAUDRATE change
 *                      - which is otherwise invisible from software. Prints
 *                      once; returns 1 when timer and platform agree.      */
int  XNvme_TimerInit(void);
u32  XNvme_TimerNow(void);
u32  XNvme_TimerUs(u32 delta);
u32  XNvme_TimerSelfTest(void);

/* ----------------- stack-overflow canary -----------------
 * Fills the dead 1 KiB .heap that lscript.ld places directly below .stack with
 * a pattern, then reports how many bytes of it the stack has run over. A
 * non-zero result means STACK OVERFLOW - raise _STACK_SIZE in lscript.ld.
 * This exists because a stack overflow was silently overwriting the last ~672
 * bytes of the benchmark's channel-1 buffer and looked like a DMA/drive bug
 * for several sessions.
 * Fill() once, early in main(); Check() wherever buffers are verified - it
 * reports the disturbance since the previous check and then re-arms itself.
 * Both return the number of bytes disturbed (Fill returns bytes armed).   */
u32  XNvme_StackCanaryFill(void);
u32  XNvme_StackCanaryCheck(const char *where);

int  XNvme_Bench(XNvme *nv, u32 nch, u64 lba0,
                 u32 bytes_per_ch, u32 chunk_bytes);

/* ----------------- Direct-path diagnostic -----------------
 * Issue I/O commands with the NVMe Host Accelerator completely out of the
 * loop: extra I/O queue pair in DDR (qid 3), SQE pushed by a plain DDR
 * store, SQ tail doorbell rung by the MicroBlaze, completion polled in DDR.
 * Runs a FLUSH (no data, queue-only) then a WRITE (adds the PRP1 data DMA)
 * and finally reports CSTS, so a hang can be pinned on the queue path or on
 * the data path. `lba` is the start LBA of one 512 B block (pick an LBA no
 * other test writes to).
 * Returns XST_SUCCESS (at least one command completed cleanly), XST_FAILURE
 * (queue creation failed or a command completed with an NVMe error - the log
 * carries sct/sc), or the last command's XST_TIMEOUT.
 * Diagnostic only - safe to delete once the data path is proven.        */
int  XNvme_DirectIoTest(XNvme *nv, u64 lba);

/* ----------------- IP-side probe -----------------
 * Same question as the direct test, but with the NVMe Host Accelerator back
 * in the loop. Uses AXI4-Lite reads only (they cannot hang), in this order:
 *
 *   0. read idle INT_STATUS / SW-available-CQ / SQ-CQ status / command-error
 *      count / per-SSD issued+completed as a baseline (all of these need
 *      STATISTICS_EN=1 in the IP except cmdErr and INT_STATUS);
 *   1. push ONE write to the IP's SW SQ0 (=> IP SQ1 => NVMe qid 1), no wait;
 *   2. re-read the same registers - a command-error increment means the IP
 *      rejected the SQE; the SQ-CQ status delta is the decider for the
 *      "one SQE must arrive as a single 16-beat write" question:
 *        SQempty 1->0 and SQavail down by one  => the SQE did enter the IP's
 *           SQ, so the SW-port framing is fine and the break is downstream;
 *        both unchanged                        => the push was never taken as
 *           a command (the SW_AXI4 FIFO is 512 bits wide per C_SW_FULL_CMD=1
 *           while this port is 32 bits, so one push = one 16-beat write);
 *   3. mask the error interrupt sources (keeping CQ-available on), then ring
 *      SQ1TDBL from the MicroBlaze - the exact mechanism the direct test just
 *      proved, which takes "the IP never rang the doorbell" out of the
 *      picture. The mask matters because xil_printf is not reentrant and the
 *      timeout ISR otherwise prints in the middle of this function's output;
 *      masked sources still latch and are reported;
 *   4. poll INT_STATUS / SW-available-CQ for a while - only CQ-available or a
 *      non-zero count counts as a completion, a latched timeout does not -
 *      and cross-check the driver's own cq_processed so a completion eaten
 *      silently by the ISR cannot hide;
 *   5. drain from the main context, restore the interrupt mask, and
 *      optionally dump the IP's internal SQ/CQ memory through the MicroBlaze
 *      alias - OFF by default, because that read stalls on the current design
 *      (see XNVMEHA_PROBE_READ_SQ_MEM in xnvmeha.c).
 *
 * Reading the result:
 *   command-error count grew                -> the IP rejected the SW SQE.
 *   SQavail/SQempty changed at step 2       -> the SQE entered the SQ; the
 *      break is the doorbell or the SSD-side path.
 *   INT/CQAVL move after our doorbell       -> the SSD does fetch and
 *      complete; the IP's own SQ1TDBL write is the missing piece.
 *   nothing moves at all                    -> the SSD cannot fetch the SQE
 *      from 0x44A20000 or its CQE cannot land in 0x44A24000. Our doorbell
 *      address and the host PCIe path are already ruled out.
 * Diagnostic only - safe to delete once the IP path is proven.            */
int  XNvme_IpPathProbe(XNvme *nv, u64 lba);

/* ----------------- IP-SQ fetch test -----------------
 * Splits the two faults that look identical on the IP path, with no bitstream
 * change and no statistics:
 *   1. create an extra I/O queue pair, qid 4, with its SQ on the IP's own
 *      internal SQ4 (0x44A23000 - inside the SSD-side window either way) and
 *      its CQ in DDR (proven writable by the SSD in the direct test);
 *   2. push one write into SW slot 3 and let the IP ring SQ4TDBL itself;
 *   3. poll the DDR buffer for the completion.
 *   CQE arrives  -> the SSD does fetch from the IP's SQ memory and complete;
 *      the broken link is CQE delivery into the IP's internal CQ, which is
 *      what a 16 KB inbound window (SQ/CQ addresses up to 0x44A23FFF only)
 *      would do, since CQ1 sits at 0x44A24000.
 *   no CQE       -> the SSD never fetched from the IP's SQ memory, so the
 *      fault is on the SQ-read side.
 * Diagnostic only - safe to delete once the data path is proven.          */
int  XNvme_IpSqFetchTest(XNvme *nv, u64 lba);

/* Read the drive's own diagnosis over the admin queue (which keeps working
 * while I/O hangs): Error Information log 0x01 and SMART/Health log 0x02.
 * Prints the last error entry (sqid/cid/status) plus critical warning,
 * temperature, spare, percentage used, data units and error counters.
 * Works whenever the admin queue does; returns XST_SUCCESS unless both log
 * reads fail.                                                           */
int  XNvme_AdminLogDump(XNvme *nv);

/* Top-level interrupt handler: wire hc_interrupt to this (via the AXI
 * Interrupt Controller). Processes available completions and clears
 * the status.                                                        */
void XNvme_InterruptHandler(XNvme *nv);

/* Poll fallback when hc_interrupt is not wired to an ISR. */
static inline u32 XNvme_PollCompletions(XNvme *nv)
{
    return XNvmeHa_ServiceInterrupts(&nv->ha);
}

#ifdef __cplusplus
}
#endif

#endif /* XNVME_H_ */
