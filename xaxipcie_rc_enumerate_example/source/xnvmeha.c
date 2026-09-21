/*
 * xnvmeha.c
 *
 * Implementation of the Xilinx NVMe Host Accelerator (PG328) software
 * driver for MicroBlaze bare-metal.
 *
 * See xnvmeha.h for the API contract. The control flow implemented here
 * follows PG328 Chapter 3 (Register Space / SWAXI4 memory map) and the
 * "NVMe Host Accelerator Programming Sequence" on page 30.
 */

#include "xnvmeha.h"
#include "xnvmeha_config.h"
#include "xil_io.h"
#include "xil_mem.h"
#include "xil_printf.h"

/* Per-command D-cache maintenance switch. Leave it ON for normal operation.
 * Turning it OFF is only valid when the caller takes over the coherency
 * work, and only on a WRITE-THROUGH D-cache (this design: pl.dtsi
 * dcache-use-writeback = 0), because then a store is already visible in DDR
 * and there are no dirty lines to write back:
 *   - WRITE commands need nothing (DDR is current),
 *   - READ commands need ONE invalidate per buffer after the transfer,
 *     before the CPU touches the data again.
 * The reason a caller might want this: the flush/invalidate is O(bytes /
 * cache-line) and lives in a BSP library call with no callback, so a
 * throughput benchmark cannot time it and it can dominate the run.       */
u32 xnvmeha_cache_ops = 1u;

void XNvmeHa_SetCacheOps(u32 on)
{
    xnvmeha_cache_ops = on ? 1u : 0u;
}

/* Runtime debug switch. The bring-up logs (`push@...`, `pop ...`,
 * `CQE cid=...`) cost ~30-40 ms each at 9600 baud - far longer than the I/O
 * they describe - so anything that measures throughput must turn them off.
 * Errors (XNVMEHA_ERR) stay on regardless.                              */
volatile u32 XNvmeHaVerbose = 1u;

void XNvmeHa_SetVerbose(u32 on)
{
    XNvmeHaVerbose = on ? 1u : 0u;
}

/* Optional per-iteration hook called from the completion-wait spin loop AND
 * from the PRP-list / SW-push loops (see XNVMEHA_TICK below).
 * The throughput benchmark uses it to advance its own clock (see
 * XNvme_Bench() in xnvme.c). Left NULL in normal use.                  */
void (*XNvmeHa_WaitHook)(void) = NULL;

#define XNVMEHA_TICK()  do { if (XNvmeHa_WaitHook != NULL) XNvmeHa_WaitHook(); } while (0)

#if (XNVMEHA_DEBUG >= 2)
#define XNVMEHA_DBG(...)   do { if (XNvmeHaVerbose != 0u) { \
                                    xil_printf(__VA_ARGS__); } } while (0)
#else
#define XNVMEHA_DBG(...)   do {} while (0)
#endif
#if (XNVMEHA_DEBUG >= 1)
#define XNVMEHA_ERR(...)   xil_printf(__VA_ARGS__)
#else
#define XNVMEHA_ERR(...)   do {} while (0)
#endif

/* SSD response-timeout programmed into the IP. 0 disables the timeout
 * interrupt (PG328 Table 22: a non-zero value is required for the timeout
 * feature to assert). This is the only lever that distinguishes "SQE is
 * sitting in the IP's SQ and the SSD never completed it" from "the SQE
 * never entered the IP at all", so it is left armed by default now.
 * 0x08000000 cycles @125 MHz clk_core ~= 1.07 s - far longer than any
 * legitimate 256 KiB I/O, so a fired timeout really is a stuck command. */
#ifndef XNVMEHA_TIMEOUT_CYCLES
#define XNVMEHA_TIMEOUT_CYCLES   0x08000000u
#endif

/* PRP list buffers - ONE PER COMMAND SLOT, indexed by CID.
 *
 * A single shared list is only safe when one command is in flight: with a
 * queue depth > 1 the next command overwrites the list before the SSD has
 * fetched it, so every in-flight command ends up describing the pages of the
 * most recently built one and the DMA lands in the wrong buffer. That is
 * exactly what happened on 17 Sep: the throughput benchmark (QD 8, 16
 * commands in flight) failed its verify on channel 1 only - the last list
 * built is channel 1's, so all the traffic went into *its* buffer while
 * channel 0's stayed untouched and checksummed correctly.
 *
 * Each slot gets a FULL PAGE (4 KiB), not just the 128 entries it needs:
 * NVMe requires PRP2 - the list base - to be aligned to the memory page size
 * (2^(12+MPS)). This design runs MPSMIN=0, i.e. 4 KiB pages. If a design ever
 * needs MPS > 0, widen the row to that page size; only the first
 * XNVMEHA_PRP_LIST_MAX entries are used.
 *
 * 128 slots x 4 KiB = 512 KiB.                                                */
#define XNVMEHA_PRP_ROW_ENTRIES   512u          /* 512 x 8 B = 4096 B */
static u64 xnvmeha_prp_list[XNVMEHA_MAX_OUTSTANDING][XNVMEHA_PRP_ROW_ENTRIES]
                __attribute__((aligned(4096)));

/* ----------------- register helpers ----------------- */

static inline u32 HaRegRd(XNvmeHa *ha, u32 off)
{
    return Xil_In32(ha->reg_base + off);
}

static inline void HaRegWr(XNvmeHa *ha, u32 off, u32 val)
{
    Xil_Out32(ha->reg_base + off, val);
}

/* Per-SSD PCIe base address lookup from the config. Extend for more SSDs. */
static void XNvmeHa_GetSsdPcieBase(u32 ssd, u32 *lo, u32 *hi)
{
    switch (ssd) {
    case 0:
        *lo = XNVMEHA_SSD0_PCIE_BASE;
        *hi = XNVMEHA_SSD0_PCIE_BASE_HI;
        break;
    case 1:
        *lo = XNVMEHA_SSD1_PCIE_BASE;
        *hi = XNVMEHA_SSD1_PCIE_BASE_HI;
        break;
    default:
        *lo = 0u;
        *hi = 0u;
        break;
    }
}

/* Program the PCIe base address registers for one SSD. */
void XNvmeHa_SetSsdPcieBase(XNvmeHa *ha, u32 ssd, u32 lo, u32 hi)
{
    if (ssd >= XNVMEHA_MAX_SSD)
        return;
    HaRegWr(ha, XNVMEHA_REG_PCIE_BASE_LO(ssd), lo);
    HaRegWr(ha, XNVMEHA_REG_PCIE_BASE_HI(ssd), hi);
}

/* ----------------- lifecycle ----------------- */

int XNvmeHa_Init(XNvmeHa *ha, u32 reg_base, u32 sw_axi_base)
{
    u32 ssd;
    u32 lo, hi;

    if (ha == NULL)
        return XST_FAILURE;

    for (ssd = 0; ssd < (u32)(sizeof(ha->slot) / sizeof(ha->slot[0])); ssd++) {
        ha->slot[ssd].in_use = 0;
        ha->slot[ssd].done = 0;
    }
    ha->reg_base        = reg_base; /* PG328.P17 : Table 9: Register Address Space */
    ha->sw_axi_base     = sw_axi_base;
    ha->next_cid        = 0;
    ha->cq_processed    = 0;
    ha->cq_errors       = 0;
    ha->int_mode        = 0;
    ha->use_sgl         = 1;        /* default: SGL (partial command) */
    ha->prp_page_size   = 4096u;    /* default until AdminInit reports MPS */
    ha->completion_cb   = NULL;
    for (ssd = 0; ssd < XNVMEHA_MAX_SSD; ssd++)
        ha->sw_credit[ssd] = 0;

    /* 1. Program per-SSD PCIe base addresses (PG328 p.30). */
    for (ssd = 0; ssd < XNVMEHA_MAX_SSD; ssd++) {
        XNvmeHa_GetSsdPcieBase(ssd, &lo, &hi);
        XNvmeHa_SetSsdPcieBase(ha, ssd, lo, hi);
    }

    /* 2. Program SSD0 SQ0 / CQ0 PCIe base addresses. The IP derives every
     *    other SQ/CQ base address from these. Only bits [31:12] matter. */
    HaRegWr(ha, XNVMEHA_REG_SQ_BASE(0, 0), XNVMEHA_SQ0_PCIE_BASE);
    HaRegWr(ha, XNVMEHA_REG_CQ_BASE(0, 0), XNVMEHA_CQ0_PCIE_BASE);

    /* 调试：回读队列的地址 */
    {
        u32 m;
        for (m = 0u; m < (u32)XNVMEHA_NUM_SQ_SW && m < XNVMEHA_MAX_SQ; m++) {
            XNVMEHA_DBG("NVMeHA: IP SQ%u base=0x%08x CQ%u base=0x%08x\r\n",
                        m + 1u, HaRegRd(ha, XNVMEHA_REG_SQ_BASE(0, m)),
                        m + 1u, HaRegRd(ha, XNVMEHA_REG_CQ_BASE(0, m)));
        }
    }

    /* 3. 超时寄存器，具体作用见 (PG328 Table 22) */
    HaRegWr(ha, XNVMEHA_REG_TIMEOUT, XNVMEHA_TIMEOUT_CYCLES);
    XNVMEHA_DBG("NVMeHA: timeout reg=0x%08x\r\n",
                HaRegRd(ha, XNVMEHA_REG_TIMEOUT));

    /* 调试：是否 STATISTICS_EN = 1？ */
    {
        u32 s = HaRegRd(ha, XNVMEHA_REG_SQCQ_STATUS(0u, 0u));

        XNVMEHA_DBG("NVMeHA: build fingerprint SQstat=0x%08x -> %s\r\n", s,
                    (s == 0u) ?
                    "OLD build (no statistics block: STATISTICS_EN=0, so the "
                    "S_AXI_SSD window is probably still 16 KB too)"
                    : "NEW build (statistics block live)");
    }

    /* 使能中断  */
    HaRegWr(ha, XNVMEHA_REG_INT_ENABLE,
            XNVMEHA_INT_CQ_AVAIL | XNVMEHA_INT_ALL_ERRORS);

    /* 清除中断状态 */
    HaRegWr(ha, XNVMEHA_REG_INT_STATUS, XNVMEHA_INT_ALL);

    /* 当没有可供读取的 CQE，
    但软件接口执行了 CQ 读取操作时，该位会被置位。（Table 19 ：bit3） */
    (void)Xil_In32((UINTPTR)(ha->sw_axi_base + XNVMEHA_SW_CQ_ADDR));
    XNVMEHA_DBG("NVMeHA: SW port probe INT_STATUS=0x%08x\r\n",
                HaRegRd(ha, XNVMEHA_REG_INT_STATUS));

    /* 清除上一步触发的bit 3 */
    HaRegWr(ha, XNVMEHA_REG_INT_STATUS, XNVMEHA_INT_ALL);

    XNVMEHA_DBG("NVMeHA: init done reg=0x%08x swaxi=0x%08x\r\n",
                reg_base, sw_axi_base);
    return XST_SUCCESS;
}

void XNvmeHa_ResetCredit(XNvmeHa *ha)
{
    u32 ssd;
    /* Initial credit per SSD = (#SW queues) * (depth - 1).  PG328 p.14:
     * a queue of depth D is full with D-1 entries present.              */
    u32 per = (u32)XNVMEHA_NUM_SQ_SW * ((u32)XNVMEHA_SQ_DEPTH_SW - 1u);
    for (ssd = 0; ssd < XNVMEHA_MAX_SSD; ssd++)
        ha->sw_credit[ssd] = per;
    XNVMEHA_DBG("NVMeHA: credit reset to %u per SSD\r\n", per);
}

void XNvmeHa_SetCompletionCb(XNvmeHa *ha, XNvmeCompletionCb cb)
{
    ha->completion_cb = cb;
}

void XNvmeHa_SetInterruptMode(XNvmeHa *ha, u8 enable)
{
    ha->int_mode = enable ? 1u : 0u;
}

void XNvmeHa_SetTransferMode(XNvmeHa *ha, u8 use_sgl)
{
    ha->use_sgl = use_sgl ? 1u : 0u;
}

/* Core of the PRP builder, shared by two callers:
 *
 *  - XNvmeHa_BuildPrp(): passes a per-slot list (see xnvmeha_prp_list) and
 *    flushes it - the general per-command path.
 *  - XNvmeHa_SubmitIoPrebuilt() users: build into their OWN persistent storage
 *    once, outside any timed window, then reuse the returned PRP1/PRP2 for
 *    every later command on the same buffer. The benchmark used to rebuild a
 *    127-entry list per command, which at ~1 ms per 512 KiB command was the
 *    MicroBlaze's dominant submit cost (xnvme.c bench comments).
 *
 * `list` holds `list_entries` u64 PRPs and must be 8-byte aligned (page
 * alignment is recommended; the IP fetches it from DDR through PRP2).
 * Returns the number of list entries written - 0 when PRP1/PRP2 alone covered
 * the buffer (len <= 2 pages) and no list was touched. Does NOT flush `list`;
 * the caller owns the cache maintenance.                                    */
u32 XNvmeHa_BuildPrpInto(XNvmeHa *ha, void *buf, u32 len,
                         u64 *list, u32 list_entries,
                         u64 *prp1, u64 *prp2)
{
    u64 base = XNvme_PalLocalToPcie(buf);
    u32 page = ha->prp_page_size ? ha->prp_page_size : 4096u;
    u32 offset = (u32)(base & (u64)(page - 1u));
    u32 first = page - offset;            /* bytes in 1st page */
    u32 remaining, npages, i;
    u64 second_page;

    *prp1 = base;
    *prp2 = 0;

    if (len <= first)
        return 0u;                        /* single page, PRP1 only */

    remaining = len - first;
    npages = (remaining + page - 1u) / page;
    second_page = (base & ~(u64)(page - 1u)) + page;

    if (npages == 1u) {
        *prp2 = second_page;              /* one extra page: pointer directly */
        return 0u;
    }

    if (npages > list_entries)
        npages = list_entries;            /* caller must have split at MDTS */

    for (i = 0u; i < npages; i++)
        list[i] = second_page + (u64)i * page;

    *prp2 = XNvme_PalLocalToPcie(list);
    return npages;
}

/* Build PRP1/PRP2 for a contiguous buffer of `len` bytes starting at the
 * PCIe address of `buf`. Returns the PRP1 and PRP2 values to place in the
 * full 64-byte SQE (DW6/DW7 = PRP1, DW8/DW9 = PRP2).
 *   - fits in one page      : PRP2 = 0
 *   - spans exactly 2 pages : PRP2 = address of the 2nd page
 *   - spans >2 pages        : PRP2 = address of a PRP list (page aligned)
 */
static void XNvmeHa_BuildPrp(XNvmeHa *ha, void *buf, u32 len,
                             u64 *prp1, u64 *prp2, u32 slot)
{
    /* This command's own list - see the note on xnvmeha_prp_list. */
    u64 *list = xnvmeha_prp_list[slot % XNVMEHA_MAX_OUTSTANDING];
    u32 npages = XNvmeHa_BuildPrpInto(ha, buf, len, list,
                                      XNVMEHA_PRP_LIST_MAX, prp1, prp2);

    if (npages != 0u)
        XNvme_PalFlushDcache(list, npages * (u32)sizeof(u64));
}

/* ----------------- command-id / slot management ----------------- */

static int XNvmeHa_AllocSlot(XNvmeHa *ha, u32 ssd)
{
    u32 i;
    u32 start = ha->next_cid;
    for (i = 0; i < XNVMEHA_MAX_OUTSTANDING; i++) {
        u32 idx = (start + i) % XNVMEHA_MAX_OUTSTANDING;
        if (!ha->slot[idx].in_use) {
            ha->slot[idx].in_use = 1;
            ha->slot[idx].done   = 0;
            ha->slot[idx].ssd    = (u16)ssd;
            ha->slot[idx].status = 0;
            ha->slot[idx].result0 = 0;
            ha->slot[idx].result1 = 0;
            ha->slot[idx].sqid   = 0;
            ha->next_cid = (u16)((idx + 1u) % XNVMEHA_MAX_OUTSTANDING);
            return (int)idx;
        }
    }
    return -1;
}

void XNvmeHa_FreeSlot(XNvmeHa *ha, int cid)
{
    if ((u32)cid < XNVMEHA_MAX_OUTSTANDING)
        ha->slot[cid].in_use = 0;
}

/* ----------------- SQE push ----------------- */

/* Write one SQE to the SW_AXI4 port for SSDn/SQm. Two framing models:
 *  (a) FIFO-style: successive word writes to the SAME address;
 *  (b) slot-style: word i goes to addr+i*4, filling a 64 B slot.
 * XNVMEHA_PUSH_ADDR_INC selects (b). The SW-port probe proved the port
 * decodes this region; use the readback below to see which model the
 * IP actually implements (readback matching => slot-style memory).     */
static void XNvmeHa_PushWords(XNvmeHa *ha, u32 ssd, u32 sq,
                              const u32 *words, u32 nwords)
{
    u32 i;
    u32 addr = ha->sw_axi_base + XNVMEHA_SW_SQ_ADDR(ssd, sq);

    /* The benchmark's UART clock only counts a tick when it is CALLED (it can
     * push a byte only while the TX FIFO has room, and the FIFO drains at
     * 1/BAUD). So a pass that calls the hook once per command under-counts its
     * own window - which is exactly what made "16 MiB in 3 ms" appear. Feeding
     * it from inside these hot loops keeps the call rate well above the baud
     * rate, which is the only condition under which the count equals elapsed
     * time. The hook is NULL everywhere except during a timed window, so this
     * is a load in the normal path.                                      */
    XNVMEHA_TICK();
#if (XNVMEHA_AXI_MEM_DWIDTH == 64)
    for (i = 0; i < nwords; i += 2u) {
        u64 dw = (u64)words[i];
        if ((i + 1u) < nwords)
            dw |= ((u64)words[i + 1u] << 32);
        Xil_Out64((UINTPTR)addr, dw);
        XNVMEHA_TICK();
    }
#else
    for (i = 0; i < nwords; i++) {
#if XNVMEHA_PUSH_ADDR_INC
        Xil_Out32((UINTPTR)(addr + i * 4u), words[i]);
#else
        Xil_Out32((UINTPTR)addr, words[i]);
#endif
        XNVMEHA_TICK();
    }
#endif
    XNvme_PalDsb();
#if (XNVMEHA_DEBUG >= 2)
    {
        u32 rb0 = Xil_In32((UINTPTR)addr);
        u32 rbn = Xil_In32((UINTPTR)(addr + (nwords - 1u) * 4u));
        XNVMEHA_DBG("NVMeHA: push@0x%08x n=%u rb0=0x%08x rblast=0x%08x "
                    "(w0=0x%08x wlast=0x%08x)\r\n",
                    addr, nwords, rb0, rbn, words[0], words[nwords - 1u]);
    }
#endif
}

int XNvmeHa_PushPartialSqe(XNvmeHa *ha, u32 ssd, u32 sq,
                           const XNvmePartialSqe *sqe)
{
    int cid;

    if ((ha == NULL) || (sqe == NULL) || (ssd >= XNVMEHA_MAX_SSD))
        return -1;
    if (sq >= XNVMEHA_MAX_SQ)
        return -1;

    /* With QoS disabled the IP round-robins internally and `sq` is
     * effectively ignored; with QoS enabled `sq` selects the exact NVMe
     * I/O queue (0 -> qid 1, ...).                                */

    /* Credit check: never submit beyond the available credit. */
    if (ha->sw_credit[ssd] == 0u) {
        XNVMEHA_DBG("NVMeHA: no credit for SSD%u\r\n", ssd);
        return -1;
    }

    cid = XNvmeHa_AllocSlot(ha, ssd);
    if (cid < 0) {
        XNVMEHA_ERR("NVMeHA: no free command slot\r\n");
        return -1;
    }

    /* Patch the command id into DW0 (bits [31:16]). */
    {
        XNvmePartialSqe q = *sqe;
        q.dw0 = (q.dw0 & 0x0000FFFFu) | ((u32)cid << 16);
        XNvmeHa_PushWords(ha, ssd, sq, (const u32 *)&q,
                          XNVMEHA_SQE_WORDS);
    }

    ha->sw_credit[ssd]--;   /* consume one credit */
    XNVMEHA_DBG("NVMeHA: push SSD%u SQ%u cid=%u opc=0x%02x credit=%u\r\n",
                ssd, sq, cid, sqe->dw0 & 0xFF, ha->sw_credit[ssd]);
    return cid;
}

/* Build the SQE's PRP fields for (prp_buf, prp_len) into THIS command's own
 * PRP list, then push it.
 *
 * Building the list here - rather than in the caller, before the slot is known
 * - is what makes queue depth > 1 safe: the list index must be the CID, and the
 * CID only exists after AllocSlot(). (The caller used to build one shared list
 * and hand the same PRP2 to every in-flight command.)
 *
 * `prp_buf` may be NULL for commands without data (FLUSH); the PRP fields then
 * stay zero. Returns the CID, or -1 (no credit / no free slot).            */
int XNvmeHa_PushFullSqe(XNvmeHa *ha, u32 ssd, u32 sq,
                        const XNvmeFullSqe *sqe,
                        void *prp_buf, u32 prp_len)
{
    int cid;

    if ((ha == NULL) || (sqe == NULL) || (ssd >= XNVMEHA_MAX_SSD))
        return -1;
    if (sq >= XNVMEHA_MAX_SQ)
        return -1;
    if (ha->sw_credit[ssd] == 0u)
        return -1;

    cid = XNvmeHa_AllocSlot(ha, ssd);
    if (cid < 0)
        return -1;

    {
        XNvmeFullSqe q = *sqe;

        if ((prp_buf != NULL) && (prp_len != 0u)) {
            u64 p1, p2;

            XNvmeHa_BuildPrp(ha, prp_buf, prp_len, &p1, &p2, (u32)cid);
            q.prp1_lo = (u32)p1;
            q.prp1_hi = (u32)(p1 >> 32);
            q.prp2_lo = (u32)p2;
            q.prp2_hi = (u32)(p2 >> 32);
        }

        q.dw0 = (q.dw0 & 0x0000FFFFu) | ((u32)cid << 16);
        XNvmeHa_PushWords(ha, ssd, sq, (const u32 *)&q,
                          (XNVMEHA_SQE_BYTES / 4u));
    }
    ha->sw_credit[ssd]--;
    return cid;
}

/* Push a full 64 B SQE whose PRP fields are ALREADY final.
 *
 * Same contract as XNvmeHa_PushFullSqe, but the caller supplies PRP1/PRP2
 * (typically from XNvmeHa_BuildPrpInto into persistent storage) instead of a
 * buffer, so no list is built or flushed per command. Only the CID still has
 * to be patched in after AllocSlot - same as the buffer-driven variant.     */
int XNvmeHa_PushFullSqePrebuilt(XNvmeHa *ha, u32 ssd, u32 sq,
                                const XNvmeFullSqe *sqe, u64 prp1, u64 prp2)
{
    int cid;

    if ((ha == NULL) || (sqe == NULL) || (ssd >= XNVMEHA_MAX_SSD))
        return -1;
    if (sq >= XNVMEHA_MAX_SQ)
        return -1;
    if (ha->sw_credit[ssd] == 0u)
        return -1;

    cid = XNvmeHa_AllocSlot(ha, ssd);
    if (cid < 0)
        return -1;

    {
        XNvmeFullSqe q = *sqe;

        q.prp1_lo = (u32)prp1;
        q.prp1_hi = (u32)(prp1 >> 32);
        q.prp2_lo = (u32)prp2;
        q.prp2_hi = (u32)(prp2 >> 32);
        q.dw0 = (q.dw0 & 0x0000FFFFu) | ((u32)cid << 16);
        XNvmeHa_PushWords(ha, ssd, sq, (const u32 *)&q,
                          (XNVMEHA_SQE_BYTES / 4u));
    }
    ha->sw_credit[ssd]--;
    return cid;
}

/* ----------------- CQE pop ----------------- */

/* Pop one completion entry (32 bytes) from the SW_AXI4 CQ port. */
static void XNvmeHa_PopCqe(XNvmeHa *ha, XNvmeCqeDw *cqe,
                            u32 *ssd, u32 *credit)
{
    u32 addr = ha->sw_axi_base + XNVMEHA_SW_CQ_ADDR;
#if (XNVMEHA_AXI_MEM_DWIDTH == 64)
    /* 4 x 64-bit beats: [0]CQE[63:0] [1]CQE[127:64] [2]hdr [3]rsv */
    u64 b0 = Xil_In64((UINTPTR)addr);
    u64 b1 = Xil_In64((UINTPTR)addr);
    u64 b2 = Xil_In64((UINTPTR)addr);
    (void)Xil_In64((UINTPTR)addr);          /* reserved beat */
    cqe->dw0 = (u32)(b0 & 0xFFFFFFFFu);
    cqe->dw1 = (u32)(b0 >> 32);
    cqe->dw2 = (u32)(b1 & 0xFFFFFFFFu);
    cqe->dw3 = (u32)(b1 >> 32);
    *ssd     = (u32)(b2 & XNVMEHA_CQE_SSD_MASK);
    *credit  = (u32)((b2 & XNVMEHA_CQE_CREDIT_MASK) >> XNVMEHA_CQE_CREDIT_SHIFT);
#else
    /* 8 x 32-bit beats (PG328 Figure 4 "32-bit Interface Read Data"), one
     * 32-byte record per CQE. Layout VERIFIED on hardware (17 Sep, the run
     * that finally printed "Demo complete"):
     *
     *   word[0..3]  the 16-byte NVMe CQE (DW0..DW3)
     *   word[4]     {16'h0, Credit[15:5], SSD_Number[4:0]}   <- the header
     *   word[5..7]  zero
     *
     * e.g. `pop 00000000 00000000 00010001 00010000 | 00000020 ...` is
     * qid 1, CID 0, phase 1, credit 1, SSD 0.
     *
     * XNVMEHA_POP_ADDR_INC: inside a record the ADDRESS matters. Reading
     * the same word eight times returned eight zeros (17 Sep, 08:5x run)
     * even though the IP reported a CQE available and did NOT set bit3
     * (RD_NO_CQ) - the pops were legal, just empty. Walking
     * addr, addr+4, ... addr+28 returns the real record. So this port is
     * slot-style in BOTH directions: the push already writes word i to
     * addr+i*4 (XNVMEHA_PUSH_ADDR_INC=1) and has always worked.
     * Keep this at 1.                                                     */
#ifndef XNVMEHA_POP_ADDR_INC
#define XNVMEHA_POP_ADDR_INC           1
#endif
#if (XNVMEHA_POP_ADDR_INC == 1)
    u32 w0 = Xil_In32((UINTPTR)addr + 0u);
    u32 w1 = Xil_In32((UINTPTR)addr + 4u);
    u32 w2 = Xil_In32((UINTPTR)addr + 8u);
    u32 w3 = Xil_In32((UINTPTR)addr + 12u);
    u32 w4 = Xil_In32((UINTPTR)addr + 16u);
    u32 w5 = Xil_In32((UINTPTR)addr + 20u);
    u32 w6 = Xil_In32((UINTPTR)addr + 24u);
    u32 w7 = Xil_In32((UINTPTR)addr + 28u);
#else
    u32 w0 = Xil_In32((UINTPTR)addr);
    u32 w1 = Xil_In32((UINTPTR)addr);
    u32 w2 = Xil_In32((UINTPTR)addr);
    u32 w3 = Xil_In32((UINTPTR)addr);
    u32 w4 = Xil_In32((UINTPTR)addr);       /* beat 5 */
    u32 w5 = Xil_In32((UINTPTR)addr);       /* beat 6 */
    u32 w6 = Xil_In32((UINTPTR)addr);       /* beat 7 */
    u32 w7 = Xil_In32((UINTPTR)addr);       /* beat 8 */
#endif
    /* Header at word index 4, NOT 6: reading it from 6 returns 0, which
     * silently stops the software credit pool from ever being replenished
     * (252 pushes and then the submit loop spins forever).               */
    u32 wh = w4;

    /* Keep the raw record in the log: this line is what identified the bug
     * and it stays the cheapest way to re-verify after a rebuild.         */
    XNVMEHA_DBG("NVMeHA: pop %08x %08x %08x %08x | %08x %08x %08x %08x\r\n",
                w0, w1, w2, w3, w4, w5, w6, w7);
    if ((w0 | w1 | w2 | w3 | w4 | w5 | w6 | w7) == 0u) {
        /* Symptom of reading the whole record from one address (see above):
         * the IP insists a CQE is available, bit3 never latches, and every
         * field decodes to CID 0 - which falsely completes whichever slot
         * happens to be in use.                                          */
        XNVMEHA_ERR("NVMeHA: *** zeroed CQE record (all 8 beats read 0) ***\r\n");
    }
    cqe->dw0 = w0;
    cqe->dw1 = w1;
    cqe->dw2 = w2;
    cqe->dw3 = w3;
    *ssd     = wh & XNVMEHA_CQE_SSD_MASK;
    *credit  = (wh & XNVMEHA_CQE_CREDIT_MASK) >> XNVMEHA_CQE_CREDIT_SHIFT;
#endif
}

/* Dispatch one popped CQE to its originator slot. */
static void XNvmeHa_DispatchCqe(XNvmeHa *ha, const XNvmeCqeDw *cqe,
                                u32 ssd, u32 credit)
{
    u16 cid = XNVMEHA_CQE_CID(cqe->dw3);
    u32 status = (cqe->dw3 >> 17) & 0xFFFu;  /* SCT|SC|DNR packed */
    u16 sqid = XNVMEHA_CQE_SQID(cqe->dw2);

    /* Log every completion that comes out of the SW FIFO. During bring-up
     * this is the only place that shows *which* CID the IP/SSD handed back:
     * the 17:48 run ends with iss=2 cmp=2 svc=2 (both commands issued, both
     * completed, both CQEs popped) while the wait for cid=1 still spins -
     * so the two dispatched CIDs are the missing fact.                    */
    XNVMEHA_DBG("NVMeHA: CQE cid=%u sqid=%u ssd=%u status=0x%03x "
                "dw0=0x%08x dw3=0x%08x\r\n",
                cid, sqid, ssd, status, cqe->dw0, cqe->dw3);

    /* Return credit to the SSD's pool. */
    if (ssd < XNVMEHA_MAX_SSD)
        ha->sw_credit[ssd] += credit;

    ha->cq_processed++;
    if (status != 0u) {
        ha->cq_errors++;
        XNVMEHA_ERR("NVMeHA: CQE err ssd=%u cid=%u sct=%u sc=%u dnr=%u\r\n",
                    ssd, cid,
                    XNVMEHA_CQE_SCT(cqe->dw3), XNVMEHA_CQE_SC(cqe->dw3),
                    XNVMEHA_CQE_DNR(cqe->dw3));
    }

    if ((u32)cid < XNVMEHA_MAX_OUTSTANDING && ha->slot[cid].in_use) {
        u16 want = ha->slot[cid].sqid;

        if ((want != 0u) && (want != sqid)) {
            /* Same CID, but the completion belongs to a different queue.
             * CIDs are re-used the moment a slot is freed, so a duplicate or
             * replayed SW CQE would otherwise mark an unrelated, still-in-flight
             * command as done - which is exactly the "the wait returned in
             * 0 ms" signature the benchmark has been showing. Refuse it and
             * make it visible instead of silently corrupting the run.       */
            XNVMEHA_ERR("NVMeHA: CQE sqid mismatch cid=%u cqe_sqid=%u "
                        "slot_sqid=%u - ignored (not marking done)\r\n",
                        cid, sqid, want);
        } else {
            ha->slot[cid].done    = 1;
            ha->slot[cid].status  = status;
            ha->slot[cid].result0 = cqe->dw0;
            ha->slot[cid].result1 = cqe->dw1;
            ha->slot[cid].sqid    = sqid;
            if (ha->completion_cb)
                ha->completion_cb(cid, status, cqe->dw0, cqe->dw1, sqid);
        }
    } else {
        XNVMEHA_DBG("NVMeHA: stray CQE cid=%u ssd=%u\r\n", cid, ssd);
    }
}

/* ----------------- interrupt / completion servicing ----------------- */

u32 XNvmeHa_ServiceInterrupts(XNvmeHa *ha)
{
    u32 status;
    u32 count;
    u32 processed = 0;

    status = HaRegRd(ha, XNVMEHA_REG_INT_STATUS);
    if (status == 0u)
        return 0u;

    if (status & XNVMEHA_INT_CQ_AVAIL) {
        /* Read the number of available CQEs (PG328 p.20 procedure). */
        count = HaRegRd(ha, XNVMEHA_REG_CQ_AVAIL_COUNT);
        XNVMEHA_DBG("NVMeHA: CQ irq INT_STATUS=0x%08x count=%u\r\n",
                    status, count);
        while (count-- > 0u) {
            XNvmeCqeDw cqe;
            u32 ssd, credit;
            XNvmeHa_PopCqe(ha, &cqe, &ssd, &credit);
            XNvmeHa_DispatchCqe(ha, &cqe, ssd, credit);
            processed++;
        }
    }

    if (status & XNVMEHA_INT_ADDR_ERR)
        XNVMEHA_ERR("NVMeHA: INT bad SSD/SQ address\r\n");
    if (status & XNVMEHA_INT_CREDIT_ERR)
        XNVMEHA_ERR("NVMeHA: INT SW SQE without credit\r\n");
    if (status & XNVMEHA_INT_RD_NO_CQ)
        XNVMEHA_ERR("NVMeHA: INT SW CQ read with none available\r\n");
    if (status & XNVMEHA_INT_HW_NO_CREDIT)
        XNVMEHA_DBG("NVMeHA: INT HW SQE without credit\r\n");
    if (status & XNVMEHA_INT_SSD_TIMEOUT)
        XNVMEHA_ERR("NVMeHA: INT SSD timeout\r\n");

    /* Clear latched status (write 1 to clear). */
    HaRegWr(ha, XNVMEHA_REG_INT_STATUS, status);
    return processed;
}

u32 XNvmeHa_PollCompletions(XNvmeHa *ha)
{
    /* Same handler; usable when no ISR is wired. Checks the status reg
     * (not the count reg) so the documented CQ-available handshake is
     * respected.                                                       */
    return XNvmeHa_ServiceInterrupts(ha);
}

int XNvmeHa_WaitComplete(XNvmeHa *ha, int cid)
{
    u32 timeout = XNVMEHA_SYNC_TIMEOUT;
    u32 heartbeat = 0;
    if ((u32)cid >= XNVMEHA_MAX_OUTSTANDING)
        return XST_FAILURE;

    while (!ha->slot[cid].done && --timeout) {
        /* Optional clock hook (throughput benchmark). Cheap and NULL by
         * default, so normal operation pays nothing.                    */
        if (XNvmeHa_WaitHook != NULL)
            XNvmeHa_WaitHook();

        /* When an ISR is registered it services the CQ; we just spin.
         * In poll mode (no ISR) we drain completions from here.        */
        if (!ha->int_mode)
            XNvmeHa_ServiceInterrupts(ha);

        /* Bring-up diagnostic: heartbeat-dump the IP state and poll the
         * CQ opportunistically (rescues us if the ISR is not firing). */
        if ((++heartbeat & 0x1FFFFFu) == 0u) {
            /* Slot map (first 32 CIDs): which slots are allocated, and which
             * of those are already marked done. `use` without `done` is
             * exactly the command we are still waiting for - so this line
             * shows at a glance whether the completion was never dispatched
             * or was dispatched under the wrong CID.                      */
            u32 use = 0u, fin = 0u, k;

            for (k = 0u; (k < (u32)XNVMEHA_MAX_OUTSTANDING) && (k < 32u); k++) {
                if (ha->slot[k].in_use) {
                    use |= (1u << k);
                    if (ha->slot[k].done)
                        fin |= (1u << k);
                }
            }

            XNVMEHA_ERR("NVMeHA: wait cid=%d INT=0x%08x CQAVL=%u "
                        "iss=%u cmp=%u err=%u svc=%u\r\n",
                        cid,
                        HaRegRd(ha, XNVMEHA_REG_INT_STATUS),
                        HaRegRd(ha, XNVMEHA_REG_CQ_AVAIL_COUNT),
                        HaRegRd(ha, XNVMEHA_REG_SSD_ISSUED(0)),
                        HaRegRd(ha, XNVMEHA_REG_SSD_COMPLETED(0)),
                        HaRegRd(ha, XNVMEHA_REG_SSD_ERRCOMP(0)),
                        ha->cq_processed);
            XNVMEHA_ERR("NVMeHA:   SQCQ0=0x%08x SQCQ1=0x%08x "
                        "use=0x%08x done=0x%08x\r\n",
                        HaRegRd(ha, XNVMEHA_REG_SQCQ_STATUS(0, 0)),
                        HaRegRd(ha, XNVMEHA_REG_SQCQ_STATUS(0, 1)),
                        use, fin);
            (void)XNvmeHa_ServiceInterrupts(ha);
        }
    }

    if (!ha->slot[cid].done) {
        XNVMEHA_ERR("NVMeHA: timeout waiting for cid=%d\r\n", cid);
        return XST_TIMEOUT;
    }
    return XST_SUCCESS;
}

/* ----------------- asynchronous I/O primitives ----------------- */

/* Build + submit one I/O command to SSDn / SQm. No wait. Returns CID. */
int XNvmeHa_SubmitIo(XNvmeHa *ha, u32 ssd, u32 sq, u32 nsid, u64 lba,
                     u16 nlb, void *buf, u32 len, u8 opcode)
{
    u64 pcie_buf;
    u32 n;
    u32 spin;
    int cid;

    if ((ha == NULL) || (ssd >= XNVMEHA_MAX_SSD) || (sq >= XNVMEHA_MAX_SQ))
        return -1;
    if ((buf == NULL) && (opcode != XNVME_OPC_IO_FLUSH))
        return -1;

    n = nlb ? (u32)nlb : 1u;

    /* Cache coherency: SSD DMAs directly to/from the buffer. See
     * xnvmeha_cache_ops above - a caller that manages coherency itself (the
     * throughput benchmark) turns this off.                              */
    if (xnvmeha_cache_ops != 0u) {
        if (opcode == XNVME_OPC_IO_WRITE)
            XNvme_PalFlushDcache(buf, len);
        else if (opcode == XNVME_OPC_IO_READ)
            XNvme_PalInvalidateDcache(buf, len);
    }

    pcie_buf = (opcode == XNVME_OPC_IO_FLUSH) ? 0ULL
              : XNvme_PalLocalToPcie(buf);

    if (ha->use_sgl) {
        /* Partial 32 B command, one SGL Data Block descriptor. */
        XNvmePartialSqe sqe;
        sqe.dw0        = (u32)opcode;
        sqe.nsid       = nsid;
        sqe.sgl_addr_lo = (u32)pcie_buf;
        sqe.sgl_addr_hi = (u32)(pcie_buf >> 32);
        sqe.sgl_len    = len;
        if (opcode == XNVME_OPC_IO_FLUSH)
            sqe.sgl_id_nlb = 0;
        else
            sqe.sgl_id_nlb = XNVMEHA_SGL_DATA_BLOCK | ((n - 1u) & 0xFFFFu);
        sqe.slba_lo    = (u32)lba;
        sqe.slba_hi    = (u32)(lba >> 32);

        spin = 0u;
        do {
            cid = XNvmeHa_PushPartialSqe(ha, ssd, sq, &sqe);
            if (cid < 0) {
                if (XNvmeHa_WaitHook != NULL)
                    XNvmeHa_WaitHook();
                /* No free slot or no credit. Drain completions (each pop
                 * returns one credit in the CQE header) and retry - but
                 * never spin forever: the old unbounded loop turned any
                 * credit-accounting slip into a silent hang.            */
                if (++spin > 0x100000u) {
                    XNVMEHA_ERR("NVMeHA: submit stuck sq=%u credit=%u\r\n",
                                sq, ha->sw_credit[ssd]);
                    return -1;
                }
                XNvmeHa_ServiceInterrupts(ha);
            }
        } while (cid < 0);
    } else {
        /* Full 64 B command. PRP1/PRP2 - and, beyond two pages, the PRP list -
         * are filled in by PushFullSqe, because the list belongs to whichever
         * slot that push allocates. See its comment.                      */
        XNvmeFullSqe sqe;

        memset(&sqe, 0, sizeof(sqe));
        sqe.dw0     = (u32)opcode;
        sqe.nsid    = nsid;
        sqe.slba_lo = (u32)lba;
        sqe.slba_hi = (u32)(lba >> 32);
        sqe.nlb     = (n - 1u) & 0xFFFFu;

        spin = 0u;
        do {
            cid = XNvmeHa_PushFullSqe(ha, ssd, sq, &sqe,
                                      (opcode == XNVME_OPC_IO_FLUSH) ? NULL : buf,
                                      (opcode == XNVME_OPC_IO_FLUSH) ? 0u : len);
            if (cid < 0) {
                if (XNvmeHa_WaitHook != NULL)
                    XNvmeHa_WaitHook();
                /* Same guard as the partial path above. */
                if (++spin > 0x100000u) {
                    XNVMEHA_ERR("NVMeHA: submit stuck sq=%u credit=%u\r\n",
                                sq, ha->sw_credit[ssd]);
                    return -1;
                }
                XNvmeHa_ServiceInterrupts(ha);
            }
        } while (cid < 0);
    }

    /* Remember what to finalize on completion (post-read invalidate),
     * plus which queue this CID belongs to: the CQE echoes the NVMe queue id
     * (qid = sq + 1) and DispatchCqe refuses a completion that comes back with
     * a different one, so a replayed CQE cannot mark someone else's command
     * as done.                                                          */
    ha->slot[cid].buf      = buf;
    ha->slot[cid].xfer_len = len;
    ha->slot[cid].is_read  = (opcode == XNVME_OPC_IO_READ) ? 1u : 0u;
    ha->slot[cid].sqid     = (u16)(sq + 1u);
    return cid;
}

/* Submit one I/O command with PREBUILT PRP fields.
 *
 * Same contract as XNvmeHa_SubmitIo, except the caller hands in the final
 * PRP1/PRP2 values (from XNvmeHa_BuildPrpInto into persistent storage) instead
 * of having the driver rebuild the overflow list on every command. PRP mode
 * only - SGL mode has no list to prebuild. Everything else matches
 * XNvmeHa_SubmitIo: cache pre-ops on the data buffer, credit/slot retry loop,
 * and the slot bookkeeping XNvmeHa_WaitIo relies on.
 *
 * SAFETY: a prebuilt list must never be shared by two in-flight commands -
 * the IP walks the list in DDR while the command runs. The benchmark uses one
 * table entry per (channel, chunk) and never overlaps them.               */
int XNvmeHa_SubmitIoPrebuilt(XNvmeHa *ha, u32 ssd, u32 sq, u32 nsid, u64 lba,
                             u16 nlb, void *buf, u32 len, u8 opcode,
                             u64 prp1, u64 prp2)
{
    XNvmeFullSqe sqe;
    u32 n, spin;
    int cid;

    if ((ha == NULL) || (ssd >= XNVMEHA_MAX_SSD) || (sq >= XNVMEHA_MAX_SQ))
        return -1;
    if (ha->use_sgl)
        return -1;                 /* prebuilt PRPs are a PRP-mode thing */
    if ((buf == NULL) && (opcode != XNVME_OPC_IO_FLUSH))
        return -1;

    /* Same coherency contract as XNvmeHa_SubmitIo (the benchmark runs with
     * XNvmeHa_SetCacheOps(0) and manages the data buffer itself).        */
    if (xnvmeha_cache_ops != 0u) {
        if (opcode == XNVME_OPC_IO_WRITE)
            XNvme_PalFlushDcache(buf, len);
        else if (opcode == XNVME_OPC_IO_READ)
            XNvme_PalInvalidateDcache(buf, len);
    }

    n = nlb ? (u32)nlb : 1u;

    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0     = (u32)opcode;
    sqe.nsid    = nsid;
    sqe.slba_lo = (u32)lba;
    sqe.slba_hi = (u32)(lba >> 32);
    sqe.nlb     = (n - 1u) & 0xFFFFu;
    sqe.prp1_lo = (u32)prp1;
    sqe.prp1_hi = (u32)(prp1 >> 32);
    sqe.prp2_lo = (u32)prp2;
    sqe.prp2_hi = (u32)(prp2 >> 32);

    spin = 0u;
    do {
        cid = XNvmeHa_PushFullSqePrebuilt(ha, ssd, sq, &sqe, prp1, prp2);
        if (cid < 0) {
            if (XNvmeHa_WaitHook != NULL)
                XNvmeHa_WaitHook();
            /* Same guard as the other submit paths. */
            if (++spin > 0x100000u) {
                XNVMEHA_ERR("NVMeHA: submit(prebuilt) stuck sq=%u credit=%u\r\n",
                            sq, ha->sw_credit[ssd]);
                return -1;
            }
            XNvmeHa_ServiceInterrupts(ha);
        }
    } while (cid < 0);

    ha->slot[cid].buf      = buf;
    ha->slot[cid].xfer_len = len;
    ha->slot[cid].is_read  = (opcode == XNVME_OPC_IO_READ) ? 1u : 0u;
    ha->slot[cid].sqid     = (u16)(sq + 1u);
    return cid;
}

/* Wait for a submitted command, finalize it, and free its slot. */
int XNvmeHa_WaitIo(XNvmeHa *ha, int cid)
{
    void *buf;
    u32 len;
    u8 is_read;
    int rc;

    if ((ha == NULL) || (u32)cid >= XNVMEHA_MAX_OUTSTANDING)
        return XST_FAILURE;

    buf     = ha->slot[cid].buf;
    len     = ha->slot[cid].xfer_len;
    is_read = ha->slot[cid].is_read;

    rc = XNvmeHa_WaitComplete(ha, cid);
    if (rc == XST_SUCCESS) {
        if (ha->slot[cid].status != 0u)
            rc = XST_FAILURE;
        else if (is_read && (xnvmeha_cache_ops != 0u))
            XNvme_PalInvalidateDcache(buf, len); /* see fresh data */
    }
    XNvmeHa_FreeSlot(ha, cid);
    return rc;
}

/* ----------------- synchronous I/O (single queue, sq=0) ----------------- */

static int XNvmeHa_SyncIo(XNvmeHa *ha, u32 ssd, u32 nsid, u64 lba,
                          u16 nlb, void *buf, u32 len, u8 opcode)
{
    int cid = XNvmeHa_SubmitIo(ha, ssd, 0u, nsid, lba, nlb, buf, len, opcode);
    if (cid < 0)
        return XST_FAILURE;
    return XNvmeHa_WaitIo(ha, cid);
}

int XNvmeHa_SyncRead(XNvmeHa *ha, u32 ssd, u32 nsid, u64 lba,
                     u16 nlb, void *buf, u32 len)
{
    return XNvmeHa_SyncIo(ha, ssd, nsid, lba, nlb, buf, len,
                          XNVME_OPC_IO_READ);
}

int XNvmeHa_SyncWrite(XNvmeHa *ha, u32 ssd, u32 nsid, u64 lba,
                      u16 nlb, const void *buf, u32 len)
{
    return XNvmeHa_SyncIo(ha, ssd, nsid, lba, nlb, (void *)buf, len,
                          XNVME_OPC_IO_WRITE);
}

int XNvmeHa_SyncFlush(XNvmeHa *ha, u32 ssd, u32 nsid)
{
    return XNvmeHa_SyncIo(ha, ssd, nsid, 0, 0, NULL, 0,
                          XNVME_OPC_IO_FLUSH);
}

/* ----------------- diagnostics ----------------- */

u32 XNvmeHa_GetQueueStatus(XNvmeHa *ha, u32 ssd, u32 sq)
{
    if (ssd >= XNVMEHA_MAX_SSD || sq >= XNVMEHA_MAX_SQ)
        return 0;
    return HaRegRd(ha, XNVMEHA_REG_SQCQ_STATUS(ssd, sq));
}

void XNvmeHa_GetSsdStats(XNvmeHa *ha, u32 ssd,
                         u32 *issued, u32 *completed, u32 *errors)
{
    if (ssd >= XNVMEHA_MAX_SSD)
        return;
    if (issued)   *issued   = HaRegRd(ha, XNVMEHA_REG_SSD_ISSUED(ssd));
    if (errors)   *errors   = HaRegRd(ha, XNVMEHA_REG_SSD_ERRCOMP(ssd));
    if (completed)*completed= HaRegRd(ha, XNVMEHA_REG_SSD_COMPLETED(ssd));
}

int XNvmeHa_FlushQueue(XNvmeHa *ha, u32 ssd, u32 sq)
{
    u32 timeout = XNVMEHA_SYNC_TIMEOUT;
    u32 flush_reg, done_reg;

    if (ssd >= XNVMEHA_MAX_SSD || sq >= XNVMEHA_MAX_SQ)
        return XST_FAILURE;

    flush_reg = XNVMEHA_REG_SQ_FLUSH(ssd, sq);
    done_reg  = XNVMEHA_REG_SQ_FLUSH_DONE(ssd, sq);

    /* Assert flush, wait for done, then deassert (PG328 Table 16/17). */
    HaRegWr(ha, flush_reg, 1u);
    while ((HaRegRd(ha, done_reg) & 0x1u) == 0u) {
        if (--timeout == 0u) {
            XNVMEHA_ERR("NVMeHA: flush timeout SSD%u SQ%u\r\n", ssd, sq);
            HaRegWr(ha, flush_reg, 0u);
            return XST_TIMEOUT;
        }
    }
    HaRegWr(ha, flush_reg, 0u);
    return XST_SUCCESS;
}

/* ==========================================================================
 *  SSD-side (IP internal memory) probes
 *
 *  Everything below reads the IP's own SQ/CQ buffers, i.e. the memory the
 *  SSD reads SQEs out of and writes CQEs into. MicroBlaze reaches it through
 *  the second decode of S_AXI_SSD at XNVMEHA_SSD_ALIAS_BASE (see
 *  xnvmeha_config.h). This is the only way, without an ILA, to see whether
 *  the SW push really assembled a 64-byte command inside the IP, and whether
 *  a completion ever landed in the IP's CQ.
 * ========================================================================== */

u32 XNvmeHa_GetIntStatus(XNvmeHa *ha)
{
    return (ha == NULL) ? 0u : HaRegRd(ha, XNVMEHA_REG_INT_STATUS);
}

u32 XNvmeHa_GetCqAvail(XNvmeHa *ha)
{
    return (ha == NULL) ? 0u : HaRegRd(ha, XNVMEHA_REG_CQ_AVAIL_COUNT);
}

u32 XNvmeHa_GetCmdErrCount(XNvmeHa *ha)
{
    return (ha == NULL) ? 0u : HaRegRd(ha, XNVMEHA_REG_CMD_ERR_COUNT);
}

u32 XNvmeHa_SetIntEnable(XNvmeHa *ha, u32 mask)
{
    u32 old;

    if (ha == NULL)
        return 0u;
    old = HaRegRd(ha, XNVMEHA_REG_INT_ENABLE);
    HaRegWr(ha, XNVMEHA_REG_INT_ENABLE, mask);
    return old;
}

void XNvmeHa_RingSsdSqDoorbell(u32 ctrl_base, u32 dstrd, u32 qid, u32 tail)
{
    u32 off = XNVME_DB_SQ(qid, dstrd);

    XNvme_PalPcieWrite32((u64)ctrl_base + (u64)off, tail);
    XNVMEHA_DBG("IPPROBE: SQ%uTDBL @0x%08x <- %u (rung by MicroBlaze)\r\n",
                qid, (u32)ctrl_base + off, tail);
}

/* Reading the IP's internal SQ/CQ memory through the MicroBlaze alias is NOT
 * reliable on this design, so both reads are OFF by default.
 *
 * Observed 2026-09-16 12:44: the very first Xil_In32 of 0x44A50000 (SQ1 via
 * the alias) never returned - the log stopped right after the register dump
 * and before the "SQ slot0" lines, i.e. a bus-level stall, not a wrong value.
 * Two candidate causes, both worth knowing:
 *   - the SmartConnect does not translate M_AXI_DC's 0x44A50000 range down to
 *     the slave's C_BASEADDR 0x44A20000 (it is the only one of the three
 *     nvmeha ranges that needs translation: 0x44A00000 -> S_AXI and
 *     0x44A40000 -> S_AXI_LITE are already identity), so the slave sees an
 *     address it does not decode and never asserts RVALID;
 *   - or the IP's S_AXI_SSD read path itself does not answer, which would
 *     also explain why the SSD never fetches the SQE at 0x44A20000.
 * Turn these on only to tell those two apart, and expect the run to stop
 * inside the dump if the path is dead. */
#ifndef XNVMEHA_PROBE_READ_SQ_MEM
#define XNVMEHA_PROBE_READ_SQ_MEM       0
#endif
#ifndef XNVMEHA_PROBE_READ_CQ_MEM
#define XNVMEHA_PROBE_READ_CQ_MEM       0
#endif

void XNvmeHa_DumpSsdMemory(XNvmeHa *ha, u32 ssd, u32 sq, const char *tag)
{
    u32 sq_base, cq_base, sq_alias, cq_alias, i;
    u32 w[16];

    if ((ha == NULL) || (ssd >= XNVMEHA_MAX_SSD) || (sq >= XNVMEHA_MAX_SQ))
        return;

    /* The IP's own RO read-back of the derived base addresses - this is what
     * the SSD was told in Create I/O Queue, i.e. what the SSD will fetch. */
    sq_base = HaRegRd(ha, XNVMEHA_REG_SQ_BASE(ssd, sq));
    cq_base = HaRegRd(ha, XNVMEHA_REG_CQ_BASE(ssd, sq));
    sq_alias = XNVMEHA_SSD_ALIAS(sq_base);
    cq_alias = XNVMEHA_SSD_ALIAS(cq_base);

    XNVMEHA_DBG("IPPROBE[%s]: SSD%u SQ%u pcie=0x%08x alias=0x%08x\r\n",
                tag, ssd, sq + 1u, sq_base, sq_alias);
    XNVMEHA_DBG("IPPROBE[%s]: SSD%u CQ%u pcie=0x%08x alias=0x%08x\r\n",
                tag, ssd, sq + 1u, cq_base, cq_alias);
    XNVMEHA_DBG("IPPROBE[%s]: INT=0x%08x CQAVL=%u SQCQstat=0x%08x\r\n", tag,
                HaRegRd(ha, XNVMEHA_REG_INT_STATUS),
                HaRegRd(ha, XNVMEHA_REG_CQ_AVAIL_COUNT),
                HaRegRd(ha, XNVMEHA_REG_SQCQ_STATUS(ssd, sq)));

    /* SQ slot 0 = the 16 DWORDS of the oldest queued SQE: DW0 shows the
     * opcode (bits 7:0) and the CID the IP patched in (bits 31:16). */
#if (XNVMEHA_PROBE_READ_SQ_MEM == 1)
    for (i = 0u; i < 16u; i++)
        w[i] = Xil_In32((UINTPTR)(sq_alias + i * 4u));
    for (i = 0u; i < 16u; i += 4u)
        XNVMEHA_DBG("IPPROBE[%s]: SQ slot0 +0x%02x : %08x %08x %08x %08x\r\n",
                    tag, i * 4u, w[i], w[i + 1u], w[i + 2u], w[i + 3u]);
#else
    XNVMEHA_DBG("IPPROBE[%s]: SQ alias 0x%08x read disabled\r\n",
                tag, sq_alias);
#endif

    /* CQ entry 0 = 16-byte NVMe CQE (DW3 holds CID | phase | status). Any
     * non-zero phase bit here means the SSD really posted a completion. */
#if (XNVMEHA_PROBE_READ_CQ_MEM == 1)
    for (i = 0u; i < 8u; i++)
        w[i] = Xil_In32((UINTPTR)(cq_alias + i * 4u));
    for (i = 0u; i < 8u; i += 4u)
        XNVMEHA_DBG("IPPROBE[%s]: CQ ent0 +0x%02x : %08x %08x %08x %08x\r\n",
                    tag, i * 4u, w[i], w[i + 1u], w[i + 2u], w[i + 3u]);
#else
    XNVMEHA_DBG("IPPROBE[%s]: CQ alias 0x%08x read disabled\r\n",
                tag, cq_alias);
#endif
}
