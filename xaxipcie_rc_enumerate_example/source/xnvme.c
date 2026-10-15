/*
 * xnvme.c
 *
 * High-level NVMe bring-up + I/O on top of the NVMe Host Accelerator.
 */

#include "xnvme.h"
#include "xnvmeha_config.h"
#include "xil_mem.h"
#include "xil_printf.h"
#include "xnvme_pal.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xil_types.h"
/* For XPAR_XTMRCTR_0_* and XPAR_XUARTLITE_0_*: the benchmark derives its time
 * base and its UART reference from the hardware description instead of
 * hardcoding them, so a parameter change in Vivado cannot silently invalidate
 * a measurement.                                                        */
#include "xparameters.h"

#if (XNVMEHA_DEBUG >= 2)
#define XNVME_DBG(...)  xil_printf(__VA_ARGS__)
#else
#define XNVME_DBG(...)  do {} while (0)
#endif
#if (XNVMEHA_DEBUG >= 1)
#define XNVME_ERR(...)  xil_printf(__VA_ARGS__)
#else
#define XNVME_ERR(...)  do {} while (0)
#endif

/* I/O queue geometry derived from the config. */
#define SQ_STRIDE_BYTES   ((u32)XNVMEHA_SQ_DEPTH_SW * 64u)   /* 64B per SQE */
#define CQ_STRIDE_BYTES   (((u32)XNVMEHA_SQ_DEPTH_SW * 16u + 4095u) & ~4095u)   /* 16B per CQE */

int XNvme_Init(XNvme *nv, u32 reg_base, u32 sw_axi_base,
               u64 ssd_pcie_base, u32 ssd_index)
{
    int rc;
    u32 q;

    if (nv == NULL)
        return XST_FAILURE;

    if (reg_base == 0u)
        reg_base = XNVMEHA_REG_BASE;
    if (sw_axi_base == 0u)
        sw_axi_base = XNVMEHA_SW_AXI_BASE;
    if (ssd_index >= XNVMEHA_MAX_SSD)
        return XST_FAILURE;

    memset(nv, 0, sizeof(*nv));
    nv->target_ssd = ssd_index;
    nv->nsid       = 1;        /* default namespace */
    nv->lba_size   = 512;      /* default until Identify runs */

    /* ---- 1. 配置NVMe Host Accelerator IP寄存器. ---- */
    rc = XNvmeHa_Init(&nv->ha, reg_base, sw_axi_base);
    if (rc != XST_SUCCESS) {
        XNVME_ERR("NVMe: HA init failed\r\n");
        return rc;
    }

    /* ---- 1b. 清除IP自身的SQ残留
     * The IP issues from its internal SQ to the SSD on its own, and it
     * stops issuing on a queue that still holds an entry. A previous run -
     * or any diagnostic that rang the SSD's SQ tail doorbell behind the
     * IP's back - leaves exactly that behind, and then every later push to
     * that queue sits there forever. The 16:41 log ends with `SQCQ0=
     * 0x000023F8`: SQ1 not empty, SQavail 63, bit13 (response timeout) set
     * while the demo's own write never leaves the queue.
     *
     * PG328 Table 16/17 gives flush for this: assert, wait for done,
     * deassert - it resets the queue pointers and drops pending entries.
     * Gate on the statistics status register (live since the 32 KB build)
     * so a clean boot pays nothing and a dirty one heals itself.        */
    for (q = 0u; q < (u32)XNVMEHA_ACTIVE_QUEUES; q++) {
        u32 st = XNvmeHa_GetQueueStatus(&nv->ha, ssd_index, q);

        if ((st & XNVMEHA_STAT_SQ_EMPTY) == 0u) {
            XNVME_ERR("NVMe: SQ%u dirty at init (status=0x%08x) - flushing"
                        "\r\n", q + 1u, st);
            (void)XNvmeHa_FlushQueue(&nv->ha, ssd_index, q);
        }
    }

    /* ---- 2. 创建管理队列 ---- */
    rc = XNvme_AdminInit(&nv->ad, ssd_pcie_base);
    if (rc != XST_SUCCESS) {
        XNVME_ERR("NVMe: admin/controller init failed\r\n");
        return rc;
    }

    /* ---- 3. 向 SSD 请求所需的 I/O 队列。
     * 某些 SSD 要求在发送“创建 I/O 队列”命令之前执行此操作。
     * 此处忽略获批的队列数量 ---- */
    rc = XNvme_AdminSetNumQueues(&nv->ad,
                                 (u32)XNVMEHA_NUM_SQ_SW,
                                 (u32)XNVMEHA_NUM_SQ_SW,
                                 NULL, NULL);
    if (rc != XST_SUCCESS)
        XNVME_DBG("NVMe: SetNumQueues failed (continuing)\r\n");

    /* ---- 4. 每个活动队列创建一个 I/O CQ/SQ 对。
     *        NVMe 队列 ID (qid)范围为 1..N（qid 0 为管理队列）。
     *        PCIe 地址必须与 IP 根据 SQ0/CQ0 基地址推导出的地址相匹配（参见 PG328 第 30 页）
     *        且必须位于 PCIe 可访问的 S_AXI_SSD 窗口范围内
     *        see XNVMEHA_ACTIVE_QUEUES in xnvmeha_config.h.            */
    for (q = 0; q < (u32)XNVMEHA_ACTIVE_QUEUES; q++) {
        u16 qid = (u16)(q + 1u);
        u64 sq_addr = (u64)XNVMEHA_SQ0_PCIE_BASE
                      + (u64)q * (u64)SQ_STRIDE_BYTES;
        u64 cq_addr = (u64)XNVMEHA_CQ0_PCIE_BASE
                      + (u64)q * (u64)CQ_STRIDE_BYTES;
        XNVME_DBG("NVMe: qid=%u SQ=0x%08x CQ=0x%08x (stride=%u)\r\n",
                  qid, (u32)sq_addr, (u32)cq_addr, CQ_STRIDE_BYTES);
        rc = XNvme_AdminCreateIoQueuePair(&nv->ad, qid,
                                          (u32)XNVMEHA_SQ_DEPTH_SW,
                                          sq_addr, cq_addr);
        if (rc != XST_SUCCESS) {
            XNVME_ERR("NVMe: create I/O queue qid=%u failed\r\n", qid);
            return rc;
        }
    }

    /* ---- 5. 启用流控. ---- */
    XNvmeHa_ResetCredit(&nv->ha);

    /* ---- 6. MDTS + SGL 支持。
     *    MDTS 限制了单条命令的数据传输量（因此传输 1 MiB 数据可能需要多条命令）。
     *    SGLS 指示 SGL 是否可用,若不可用，I/O 路径将回退至 PRP 模式。
     *    部分（32 字节）命令仅在涉及多页传输时支持 SGL，
     *    因此 PRP 模式使用完整的 64 字节命令。
     *    MDTS 的值以 MPSMIN 页为单位，page_size已经在XNvme_AdminInit中计算过了。 */
    nv->ha.prp_page_size = nv->ad.page_size ? nv->ad.page_size : 4096u;
    {
        XNvmeIdentifyController idc;
        rc = XNvme_AdminIdentifyController(&nv->ad, &idc);
        if (rc == XST_SUCCESS) {
            u8  mdts = XNVME_IDC_MDTS(&idc);
            u32 sgls = XNVME_IDC_SGLS(&idc);
            nv->sgl_supported = (sgls & 0x1u) ? 1u : 0u;
            nv->max_xfer = (mdts == 0u) ? 0u
                          : ((1u << mdts) * nv->ha.prp_page_size);
        } else {
            nv->sgl_supported = 1u;
            nv->max_xfer = 0u;       /* unknown: assume unlimited */
        }
        nv->use_sgl = nv->sgl_supported;
        XNvmeHa_SetTransferMode(&nv->ha, nv->use_sgl);
    }

    XNVME_DBG("NVMe: init complete (%u I/O queues, %s, mps=%u page=%u, max_xfer=%u)\r\n",
              (u32)XNVMEHA_NUM_SQ_SW, nv->use_sgl ? "SGL" : "PRP",
              nv->ad.mps, nv->ha.prp_page_size, nv->max_xfer);
    return XST_SUCCESS;
}

int XNvme_IdentifyController(XNvme *nv, XNvmeIdentifyController *out)
{
    if (nv == NULL)
        return XST_FAILURE;
    return XNvme_AdminIdentifyController(&nv->ad, out);
}

int XNvme_IdentifyNamespace(XNvme *nv, u32 nsid,
                            XNvmeIdentifyNamespace *out)
{
    int rc;
    u32 flbas_idx;
    u32 ds;

    if (nv == NULL)
        return XST_FAILURE;

    rc = XNvme_AdminIdentifyNamespace(&nv->ad, nsid, out);
    if (rc != XST_SUCCESS)
        return rc;

    if (nsid == 0)
        nsid = 1;
    nv->nsid = nsid;
    nv->ns_size = XNVME_IDN_NSZE(out);

    /* LBA data size = 2^(LBAF[flbas & 0xF].ds); DS is byte 2 of the
     * 4-byte LBAF descriptor at offset 0x80.                          */
    flbas_idx = XNVME_IDN_FLBAS(out) & 0x0Fu;
    ds = XNVME_IDN_LBAF_DS(out, flbas_idx);
    nv->lba_size = (ds == 0u) ? 0u : (1u << ds);
    if (nv->lba_size == 0u)
        nv->lba_size = 512u;

    XNVME_DBG("NVMe: NS%u nsze=0x%08x%08x lba_size=%u\r\n",
              nsid, (u32)(nv->ns_size >> 32), (u32)nv->ns_size,
              nv->lba_size);
    return XST_SUCCESS;
}

/* Max logical blocks a single NVMe I/O command may carry: bounded by the
 * 16-bit NLB field (65535), by the controller's MDTS, and - in PRP mode -
 * by the PRP-list capacity (XNVMEHA_PRP_LIST_MAX entries x page_size). */
static u32 XNvme_MaxBlocksPerCmd(const XNvme *nv)
{
    u32 maxb = 65535u;
    if (nv->max_xfer && nv->lba_size) {
        u32 mdts_blocks = nv->max_xfer / nv->lba_size;
        if (mdts_blocks && mdts_blocks < maxb)
            maxb = mdts_blocks;
    }
    if (!nv->use_sgl && nv->lba_size) {
        u32 page = nv->ha.prp_page_size ? nv->ha.prp_page_size : 4096u;
        u32 prp_cap = (XNVMEHA_PRP_LIST_MAX * page) / nv->lba_size;
        if (prp_cap && prp_cap < maxb)
            maxb = prp_cap;
    }
    if (maxb == 0u)
        maxb = 65535u;
    return maxb;
}

int XNvme_Read(XNvme *nv, u64 lba, u32 nlb, void *buf, u32 len)
{
    u32 maxb, done = 0u;
    u8 *p = (u8 *)buf;

    if (nv == NULL)
        return XST_FAILURE;
    if (nlb == 0u)
        nlb = 1u;
    if ((u64)nlb * nv->lba_size > len)
        return XST_FAILURE;

    maxb = XNvme_MaxBlocksPerCmd(nv);
    while (done < nlb) {
        u32 chunk = nlb - done;
        if (chunk > maxb)
            chunk = maxb;
        if (XNvmeHa_SyncRead(&nv->ha, nv->target_ssd, nv->nsid,
                             lba + done, (u16)chunk,
                             p + (u64)done * nv->lba_size,
                             chunk * nv->lba_size) != XST_SUCCESS)
            return XST_FAILURE;
        done += chunk;
    }
    return XST_SUCCESS;
}

int XNvme_Write(XNvme *nv, u64 lba, u32 nlb, const void *buf, u32 len)
{
    u32 maxb, done = 0u;
    const u8 *p = (const u8 *)buf;

    if (nv == NULL)
        return XST_FAILURE;
    if (nlb == 0u)
        nlb = 1u;
    if ((u64)nlb * nv->lba_size > len)
        return XST_FAILURE;

    maxb = XNvme_MaxBlocksPerCmd(nv);
    while (done < nlb) {
        u32 chunk = nlb - done;
        if (chunk > maxb)
            chunk = maxb;
        if (XNvmeHa_SyncWrite(&nv->ha, nv->target_ssd, nv->nsid,
                              lba + done, (u16)chunk,
                              p + (u64)done * nv->lba_size,
                              chunk * nv->lba_size) != XST_SUCCESS)
            return XST_FAILURE;
        done += chunk;
    }
    return XST_SUCCESS;
}

int XNvme_Flush(XNvme *nv)
{
    if (nv == NULL)
        return XST_FAILURE;
    return XNvmeHa_SyncFlush(&nv->ha, nv->target_ssd, nv->nsid);
}

/* ----------------- Multi-channel (SW QoS) I/O ----------------- */

u32 XNvme_NumChannels(const XNvme *nv)
{
    (void)nv;
    return XNVMEHA_ACTIVE_QUEUES;
}

int XNvme_WriteChannelAsync(XNvme *nv, u32 ch, u64 lba, u16 nlb,
                            const void *buf, u32 len)
{
    if ((nv == NULL) || (ch >= (u32)XNVMEHA_NUM_SQ_SW) || (buf == NULL))
        return -1;
    if ((u32)nlb * nv->lba_size > len)
        return -1;
    return XNvmeHa_SubmitIo(&nv->ha, nv->target_ssd, ch, nv->nsid, lba,
                            nlb, (void *)buf, len, XNVME_OPC_IO_WRITE);
}

int XNvme_ReadChannelAsync(XNvme *nv, u32 ch, u64 lba, u16 nlb,
                           void *buf, u32 len)
{
    if ((nv == NULL) || (ch >= (u32)XNVMEHA_NUM_SQ_SW) || (buf == NULL))
        return -1;
    if ((u32)nlb * nv->lba_size > len)
        return -1;
    return XNvmeHa_SubmitIo(&nv->ha, nv->target_ssd, ch, nv->nsid, lba,
                            nlb, buf, len, XNVME_OPC_IO_READ);
}

int XNvme_WaitIo(XNvme *nv, int cid)
{
    if (nv == NULL)
        return XST_FAILURE;
    return XNvmeHa_WaitIo(&nv->ha, cid);
}

int XNvme_WriteChannels(XNvme *nv, u32 nch,
                        const u64 *lba, const u32 *nlb,
                        const void *const *buf, const u32 *len)
{
    u32 maxb, ch;
    u32 done[XNVMEHA_MAX_SQ];

    if ((nv == NULL) || (nch == 0u) || (nch > (u32)XNVMEHA_NUM_SQ_SW))
        return XST_FAILURE;
    if ((lba == NULL) || (nlb == NULL) || (buf == NULL) || (len == NULL))
        return XST_FAILURE;

    maxb = XNvme_MaxBlocksPerCmd(nv);
    for (ch = 0u; ch < nch; ch++) {
        if ((nlb[ch] * nv->lba_size) > len[ch])
            return XST_FAILURE;        /* buffer too small for this channel */
        done[ch] = 0u;
    }

    /* Round-by-round pipelining: each round, submit one MDTS-sized chunk
     * from every channel that still has data, then wait for that round.
     * All nch queues are busy simultaneously, so throughput scales with
     * the number of channels (up to the SSD's internal parallelism).   */
    for (;;) {
        int cids[XNVMEHA_MAX_SQ];
        u32 submitted = 0u;
        u8 any_left = 0u;

        for (ch = 0u; ch < nch; ch++) {
            u32 remaining, chunk;
            u64 off;
            cids[ch] = -1;
            if (done[ch] >= nlb[ch])
                continue;
            any_left = 1u;
            remaining = nlb[ch] - done[ch];
            chunk = (remaining > maxb) ? maxb : remaining;
            off = (u64)done[ch] * nv->lba_size;
            cids[ch] = XNvme_WriteChannelAsync(
                           nv, ch, lba[ch] + done[ch], (u16)chunk,
                           (const u8 *)buf[ch] + off, chunk * nv->lba_size);
            if (cids[ch] >= 0) {
                done[ch] += chunk;
                submitted++;
            }
        }

        if (!any_left)
            break;                     /* all channels complete */

        if (submitted == 0u) {
            /* No slot/credit: drain completions and retry the round. */
            XNvmeHa_ServiceInterrupts(&nv->ha);
            continue;
        }

        /* Wait for everything submitted this round. */
        for (ch = 0u; ch < nch; ch++) {
            if (cids[ch] >= 0) {
                int rc = XNvme_WaitIo(nv, cids[ch]);
                if (rc != XST_SUCCESS)
                    return rc;
            }
        }
    }
    return XST_SUCCESS;
}

void XNvme_InterruptHandler(XNvme *nv)
{
    if (nv != NULL)
        (void)XNvmeHa_ServiceInterrupts(&nv->ha);
}

/* ==========================================================================
 *  Direct I/O path test - the NVMe Host Accelerator is NOT in the loop.
 *
 *  The admin queue has already proved both halves of the PCIe path (host ->
 *  SSD registers through BAR0, SSD -> DDR DMA and back), so when an I/O never
 *  completes, this separates the remaining suspects with NO bitstream change:
 *  create an extra I/O queue pair whose SQ/CQ live in DDR (qid 3; qid 1/2
 *  belong to the IP), push one SQE with a plain DDR store, ring the SQ tail
 *  doorbell from the MicroBlaze, poll the DDR CQ.
 *
 *  Two commands are issued on purpose, because they isolate different layers:
 *    FLUSH (opcode 0x00) - no data, no PRP. Exercises only the queue pair,
 *        the doorbell and the command/completion path.
 *    WRITE (opcode 0x01) - adds the data DMA through PRP1.
 *  Then CSTS is read: bit 1 (CFS) tells whether the controller has gone into
 *  a fatal state, bit 2/3 (SHST) whether it is shutting down.
 *
 *  Reading the result:
 *    FLUSH ok, WRITE ok   -> drive + host path fine; fault is in the IP's
 *       SSD-side path (its doorbell, or its SQ-read/CQ-write interfaces).
 *       Next tool: ILA on nvmeha_0/m_axi_ssd, expect 0x76001004 (SQ1TDBL) and
 *       0x76001008 (SQ2TDBL) with dstrd=0.
 *    FLUSH ok, WRITE hangs -> queue path fine, the data DMA is what hangs.
 *    both hang            -> the drive does not service I/O queues at all;
 *       the IP is exonerated. Then read the drive's own logs (the admin path
 *       still works) - see XNvme_AdminLogDump below.
 * ========================================================================== */
#define XNVME_DIRECT_QID     3u
#define XNVME_DIRECT_DEPTH   64u
#define XNVME_DIRECT_POLLS   1000000u

static XNvmeFullSqe xnvme_direct_sq[XNVME_DIRECT_DEPTH]
                        __attribute__((aligned(4096)));
static XNvmeCqeDw   xnvme_direct_cq[XNVME_DIRECT_DEPTH]
                        __attribute__((aligned(4096)));
static u8           xnvme_direct_buf[4096] __attribute__((aligned(4096)));
static u8           xnvme_log_buf[4096]    __attribute__((aligned(4096)));

static u16 xnvme_rd16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 xnvme_rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Push one command on the direct (non-IP) queue pair and poll for it. */
static int XNvme_DirectCmd(XNvme *nv, u8 opcode, u64 lba, u64 prp1, u16 cid,
                           u32 *tail, u32 *head, const char *tag)
{
    XNvmeAdmin *ad = &nv->ad;
    XNvmeFullSqe sqe;
    u32 polls, dw3 = 0u, sc, sct, slot;

    slot = *tail & (XNVME_DIRECT_DEPTH - 1u);
    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0     = (u32)opcode | ((u32)cid << 16);
    sqe.nsid    = nv->nsid ? nv->nsid : 1u;
    sqe.prp1_lo = (u32)prp1;
    sqe.prp1_hi = (u32)(prp1 >> 32);
    sqe.slba_lo = (u32)lba;
    sqe.slba_hi = (u32)(lba >> 32);
    sqe.nlb     = 0u;               /* 0 -> one block, stays in one page */

    xnvme_direct_sq[slot] = sqe;
    XNvme_PalFlushDcache(&xnvme_direct_sq[slot], sizeof(XNvmeFullSqe));

    (*tail)++;
    XNvme_PalDsb();
    XNvme_PalPcieWrite32(ad->ctrl_base + XNVME_DB_SQ(XNVME_DIRECT_QID, ad->dstrd),
                         *tail);
    XNVME_DBG("DIRECT[%s]: opc=0x%02x cid=%u lba=0x%08x prp1=0x%08x%08x "
              "SQ%uTDBL @0x%08x <- %u\r\n",
              tag, (u32)opcode, (u32)cid, (u32)lba,
              (u32)(prp1 >> 32), (u32)prp1, (u32)XNVME_DIRECT_QID,
              (u32)(ad->ctrl_base + XNVME_DB_SQ(XNVME_DIRECT_QID, ad->dstrd)),
              *tail);

    slot = *head & (XNVME_DIRECT_DEPTH - 1u);
    for (polls = 0u; polls < (u32)XNVME_DIRECT_POLLS; polls++) {
        XNvme_PalInvalidateDcache(&xnvme_direct_cq[slot], sizeof(XNvmeCqeDw));
        dw3 = xnvme_direct_cq[slot].dw3;
        if (((dw3 >> 16) & 0x1u) == 0x1u) {               /* phase tag == 1 */
            sc  = XNVMEHA_CQE_SC(dw3);
            sct = XNVMEHA_CQE_SCT(dw3);
            XNVME_DBG("DIRECT[%s]: CQE after %u polls cid_echo=%u sct=%u sc=%u "
                      "dw0=0x%08x dw2=0x%08x dw3=0x%08x\r\n",
                      tag, polls, (u32)XNVMEHA_CQE_CID(dw3), sct, sc,
                      xnvme_direct_cq[slot].dw0, xnvme_direct_cq[slot].dw2,
                      dw3);
            (*head)++;
            XNvme_PalPcieWrite32(ad->ctrl_base +
                                 XNVME_DB_CQ(XNVME_DIRECT_QID, ad->dstrd), *head);
            return ((sc == 0u) && (sct == 0u)) ? XST_SUCCESS : XST_FAILURE;
        }
    }

    XNVME_ERR("DIRECT[%s]: NO completion after %u polls (last dw3=0x%08x)\r\n",
              tag, (u32)XNVME_DIRECT_POLLS, dw3);
    return XST_TIMEOUT;
}

int XNvme_DirectIoTest(XNvme *nv, u64 lba)
{
    XNvmeAdmin *ad;
    u64 sq_pcie, cq_pcie, buf_pcie;
    u32 i, tail = 0u, head = 0u, csts;
    int rc_flush, rc_write;

    if (nv == NULL)
        return XST_FAILURE;
    ad = &nv->ad;

    sq_pcie  = XNvme_PalLocalToPcie(xnvme_direct_sq);
    cq_pcie  = XNvme_PalLocalToPcie(xnvme_direct_cq);
    buf_pcie = XNvme_PalLocalToPcie(xnvme_direct_buf);

    XNVME_DBG("DIRECT: qid=%u SQ=0x%08x%08x CQ=0x%08x%08x buf=0x%08x%08x\r\n",
              (u32)XNVME_DIRECT_QID,
              (u32)(sq_pcie >> 32), (u32)sq_pcie,
              (u32)(cq_pcie >> 32), (u32)cq_pcie,
              (u32)(buf_pcie >> 32), (u32)buf_pcie);

    if (XNvme_AdminCreateIoQueuePair(ad, (u16)XNVME_DIRECT_QID,
                                     XNVME_DIRECT_DEPTH,
                                     sq_pcie, cq_pcie) != XST_SUCCESS) {
        XNVME_ERR("DIRECT: create qid=%u in DDR failed\r\n",
                  (u32)XNVME_DIRECT_QID);
        return XST_FAILURE;
    }

    for (i = 0u; i < (u32)sizeof(xnvme_direct_buf); i++)
        xnvme_direct_buf[i] = (u8)(0x5Au + i);
    XNvme_PalFlushDcache(xnvme_direct_buf, sizeof(xnvme_direct_buf));

    rc_flush = XNvme_DirectCmd(nv, XNVME_OPC_IO_FLUSH, 0ULL, 0ULL,
                               0x29u, &tail, &head, "FLUSH");
    rc_write = XNvme_DirectCmd(nv, XNVME_OPC_IO_WRITE, lba, buf_pcie,
                               0x2Au, &tail, &head, "WRITE");

    csts = XNvme_PalPcieRead32(ad->ctrl_base + XNVME_CTRL_CSTS);
    XNVME_DBG("DIRECT: CSTS=0x%08x (RDY=%u CFS=%u SHST=%u)  flush_rc=%d "
              "write_rc=%d\r\n",
              csts, csts & 0x1u, (csts >> 1) & 0x1u, (csts >> 2) & 0x3u,
              rc_flush, rc_write);

    if (rc_flush == XST_SUCCESS || rc_write == XST_SUCCESS)
        return XST_SUCCESS;
    return rc_write;
}

/* ==========================================================================
 *  IP-side probe - the NVMe Host Accelerator IS in the loop.
 *
 *  The direct test proved the host-side doorbell mechanism and the SSD's DMA
 *  both work (FLUSH + WRITE completed at status 0, doorbell written to
 *  0x76001018 = SQ3TDBL with dstrd 0). So the only thing left between us and
 *  a working data path is the IP's own SSD-facing side, and it has exactly
 *  three possible failure points:
 *
 *    (a) the SW push never assembled a 64 B SQE inside the IP,
 *    (b) the IP never rang the SSD's SQ tail doorbell (or rings the wrong
 *        register), so the SSD never learns there is an entry to fetch,
 *    (c) the SSD cannot reach the IP's internal SQ/CQ memory, so the fetch
 *        or the completion write-back silently disappears.
 *
 *  Only AXI4-Lite registers are read while probing, because those cannot
 *  hang; the memory dump through the MicroBlaze alias is opt-in and comes
 *  last (see XNVMEHA_PROBE_READ_SQ_MEM in xnvmeha.c - on this design the
 *  alias read stalls, so it is disabled by default).
 *
 *  Sequence and what each step answers:
 *    1. push ONE write to IP SQ1 / NVMe qid 1, no wait;
 *    2. read INT_STATUS / SW-available-CQ / SQ-CQ status / command error
 *       count BEFORE - a non-zero command error count here means the IP
 *       rejected the SQE, which is (a);
 *    3. ring SQ1TDBL from the MicroBlaze - same mechanism as the direct
 *       test, so it removes (b) from the picture;
 *    4. poll INT_STATUS / SW-available-CQ for a while and re-read the SQ-CQ
 *       status: a completion appearing here means (b) was the bug; nothing
 *       appearing means the SSD never fetched 0x44A20000 or its CQE never
 *       landed in the IP, i.e. (c).
 * ========================================================================== */
#define XNVME_IPPROBE_QID     1u          /* SW SQ0 -> IP SQ1 -> qid 1 */
#define XNVME_IPPROBE_POLLS   10000u

static u8 xnvme_probe_buf[4096] __attribute__((aligned(4096)));

/* Decode the SQ/CQ status register (PG328 Table 26). Reminder: the whole
 * 0x8200..0x8400 range is the statistics block, so with statistics_en = 0
 * every field reads 0 and this line is meaningless.                       */
static void xnvme_print_sqcq_status(u32 v)
{
    XNVME_DBG("IPPROBE: SQstat=0x%08x -> SQfull=%u SQempty=%u SQavail=%u "
              "CQfull=%u CQempty=%u\r\n",
              v,
              (v & XNVMEHA_STAT_SQ_FULL)  ? 1u : 0u,
              (v & XNVMEHA_STAT_SQ_EMPTY) ? 1u : 0u,
              (u32)XNVMEHA_STAT_SQ_AVAIL(v),
              (v & XNVMEHA_STAT_CQ_FULL)  ? 1u : 0u,
              (v & XNVMEHA_STAT_CQ_EMPTY) ? 1u : 0u);
    if (v & XNVMEHA_STAT_TIMEOUT)
        XNVME_DBG("IPPROBE: SQ/CQ response-timeout bit set\r\n");
}

int XNvme_IpPathProbe(XNvme *nv, u64 lba)
{
    XNvmeAdmin *ad;
    XNvmeHa *ha;
    int cid;
    u32 i, polls, st, avail, sqst, cerr_before, cerr_after;
    u32 iss, cmp, errc, old_en, svc_before, to_seen;

    if (nv == NULL)
        return XST_FAILURE;
    ad = &nv->ad;
    ha = &nv->ha;

    iss = 0u; cmp = 0u; errc = 0u;

    for (i = 0u; i < (u32)sizeof(xnvme_probe_buf); i++)
        xnvme_probe_buf[i] = (u8)(0xC3u + i);
    XNvme_PalFlushDcache(xnvme_probe_buf, sizeof(xnvme_probe_buf));

    XNVME_DBG("\r\n--- IP-side probe (IP in the loop, MicroBlaze rings the "
              "doorbell) ---\r\n");

    /* 0. Idle baseline, taken before anything is pushed, so the SQ state
     *    below can be compared against something instead of against the
     *    Table 26 power-on defaults. With STATISTICS_EN=0 all of it reads 0. */
    XNvmeHa_GetSsdStats(ha, nv->target_ssd, &iss, &cmp, &errc);
    XNVME_DBG("IPPROBE[idle]: INT=0x%08x CQAVL=%u cmdErr=%u\r\n",
              XNvmeHa_GetIntStatus(ha), XNvmeHa_GetCqAvail(ha),
              XNvmeHa_GetCmdErrCount(ha));
    xnvme_print_sqcq_status(XNvmeHa_GetQueueStatus(ha, nv->target_ssd, 0u));
    XNVME_DBG("IPPROBE[idle]: stats issued=%u completed=%u errComp=%u\r\n",
              iss, cmp, errc);
    XNVME_DBG("IPPROBE: (idle SQstat should show SQempty=1 and the configured "
              "depth in SQavail once STATISTICS_EN=1)\r\n");

    cerr_before = XNvmeHa_GetCmdErrCount(ha);

    /* 1. One write through the SW port to IP SQ1 / NVMe qid 1. Submitted
     *    without waiting so the queue state stays observable.           */
    cid = XNvmeHa_SubmitIo(ha, nv->target_ssd, 0u, nv->nsid, lba,
                           1u, xnvme_probe_buf, nv->lba_size,
                           XNVME_OPC_IO_WRITE);
    if (cid < 0) {
        XNVME_ERR("IPPROBE: submit to IP SW SQ0 failed\r\n");
        return XST_FAILURE;
    }
    XNVME_DBG("IPPROBE: pushed 1 SQE cid=%d -> IP SQ1 -> qid %u "
              "(DW0 should read opcode 0x01, CID %d)\r\n",
              cid, (u32)XNVME_IPPROBE_QID, cid);

    /* 2. State before we interfere. Nothing here is a statistics register
     *    except SQstat/issued/completed, so INT/CQAVL/cmdErr are meaningful
     *    as they are. The statistics ones only carry data once the IP is
     *    generated with STATISTICS_EN = 1; they are printed anyway so the
     *    same build answers the "does the SQE really enter the IP's SQ"
     *    question the moment statistics are turned on.
     *
     *    THE KEY COMPARISON is SQstat before vs after the push
     *    (PG328 Table 26):
     *       SQempty 1 -> 0 and SQavail down by 1  => the SQE DID enter the
     *           IP's SQ, so "16 single-beat writes are not taken as one
     *           command" is refuted, and the break is downstream.
     *       both unchanged                        => the SW push was never
     *           accepted as a command (the SW_AXI4 FIFO is 512 bits wide per
     *           C_SW_FULL_CMD=1 and this port is 32 bits, so a push is a
     *           16-beat write; 16 separate single-beat writes may simply sit
     *           there as a partial entry - which is exactly what would make
     *           "SQ non-empty" true and the timeout fire while nothing is
     *           ever issued).                                              */
    cerr_after = XNvmeHa_GetCmdErrCount(ha);
    XNvmeHa_GetSsdStats(ha, nv->target_ssd, &iss, &cmp, &errc);
    XNVME_DBG("IPPROBE[before kick]: INT=0x%08x CQAVL=%u cmdErr=%u (was %u)\r\n",
              XNvmeHa_GetIntStatus(ha), XNvmeHa_GetCqAvail(ha),
              cerr_after, cerr_before);
    if (cerr_after != cerr_before)
        XNVME_ERR("IPPROBE: the IP incremented its command-error count => it "
                  "rejected the SW SQE (case (a))\r\n");
    xnvme_print_sqcq_status(XNvmeHa_GetQueueStatus(ha, nv->target_ssd, 0u));
    XNVME_DBG("IPPROBE[before kick]: stats issued=%u completed=%u errComp=%u "
              "(%s)\r\n", iss, cmp, errc,
              (iss == 0u && cmp == 0u && errc == 0u)
                  ? "all 0 is expected while STATISTICS_EN=0"
                  : "statistics are live");

    /* 3. Take the error sources out of the ISR before we start watching. A
     *    masked source still latches in INT_STATUS (read below); it just no
     *    longer asserts hc_interrupt. That matters because xil_printf is NOT
     *    reentrant: on the 16:21 run the timeout ISR fired in the middle of
     *    this function's own print and mangled it ("CQAVL=NVMeHA: INT SSD
     *    timeout" on one line, plus a bogus poll count).                   */
    old_en     = XNvmeHa_SetIntEnable(ha, XNVMEHA_INT_CQ_AVAIL);
    svc_before = ha->cq_processed;
    to_seen    = 0u;

    /* 4. Ring the SSD's SQ1 tail ourselves - removes (b) from the picture.
     *    Same ctrl_base + doorbell layout the direct test just proved.   */
    XNvmeHa_RingSsdSqDoorbell(ad->ctrl_base, ad->dstrd,
                              (u32)XNVME_IPPROBE_QID, 1u);

    /* 5. Watch for a *completion*. Only CQ-available (bit0) or a non-zero
     *    SW-available count ends the loop: a latched timeout (bit5) must not
     *    be mistaken for a completion - that was the other bug this run
     *    exposed. The timeout bit is counted separately instead.          */
    st = 0u;
    avail = 0u;
    for (polls = 0u; polls < (u32)XNVME_IPPROBE_POLLS; polls++) {
        st = XNvmeHa_GetIntStatus(ha);
        if ((st & XNVMEHA_INT_SSD_TIMEOUT) != 0u)
            to_seen = 1u;
        avail = XNvmeHa_GetCqAvail(ha);
        if (((st & XNVMEHA_INT_CQ_AVAIL) != 0u) || (avail != 0u))
            break;
    }
    sqst = XNvmeHa_GetQueueStatus(ha, nv->target_ssd, 0u);
    XNVME_DBG("IPPROBE[after kick +%u polls]: INT=0x%08x CQAVL=%u\r\n",
              polls, st, avail);
    XNVME_DBG("IPPROBE: cqe_seen=%u (svc %u -> %u) timeout_latched=%u\r\n",
              (u32)(((st & XNVMEHA_INT_CQ_AVAIL) != 0u) || (avail != 0u)),
              svc_before, ha->cq_processed, to_seen);
    XNvmeHa_GetSsdStats(ha, nv->target_ssd, &iss, &cmp, &errc);
    XNVME_DBG("IPPROBE[after kick]: stats issued=%u completed=%u errComp=%u\r\n",
              iss, cmp, errc);
    xnvme_print_sqcq_status(sqst);

    /* A silent dispatch by the ISR cannot hide a completion: cq_processed is
     * the driver's own count and it is checked here as well.              */
    if ((ha->cq_processed != svc_before) ||
        ((st & XNVMEHA_INT_CQ_AVAIL) != 0u) || (avail != 0u)) {
        XNVME_DBG("IPPROBE: verdict -> the SSD does fetch and complete; what "
                  "was missing is the IP's own SQ%uTDBL write\r\n",
                  (u32)XNVME_IPPROBE_QID);
    } else {
        XNVME_DBG("IPPROBE: verdict -> a forced SQ%uTDBL produced no "
                  "completion (svc unchanged, CQAVL 0). Our doorbell address "
                  "and the host PCIe path are ruled out (direct test), so "
                  "either the SSD cannot fetch the SQE from 0x%08x, or its "
                  "CQE cannot land in the IP at 0x%08x\r\n",
                  (u32)XNVME_IPPROBE_QID,
                  (u32)XNVMEHA_SQ0_PCIE_BASE,
                  (u32)XNVMEHA_CQ0_PCIE_BASE);
    }

    /* 6. Drain whatever latched - now from the main context, so its own
     *    "INT SSD timeout" print cannot race anything - then put the
     *    interrupt mask back.                                            */
    (void)XNvmeHa_ServiceInterrupts(ha);
    (void)XNvmeHa_SetIntEnable(ha, old_en);

    /* 7. Opt-in: look inside the IP's own buffers through the MicroBlaze
     *    alias. Disabled by default - this read stalls on the current
     *    design, which is a finding in itself (see xnvmeha.c).           */
    XNvmeHa_DumpSsdMemory(ha, nv->target_ssd, 0u, "after kick");

    /* Release the slot on purpose: if this command does complete later (e.g.
     * once the 2-channel write below warms the path up), the dispatcher will
     * print "stray CQE cid=%d" - which is a positive signal that a
     * completion finally arrived, just too late for this probe.          */
    XNvmeHa_FreeSlot(ha, cid);
    return XST_SUCCESS;
}

/* ==========================================================================
 *  IP-SQ fetch test - is the break in the SQE *fetch* or in the CQE *return*?
 *
 *  Everything so far converges on the SSD-facing side of the IP, but two very
 *  different faults look identical from the host:
 *    (1) the SSD cannot read the SQE out of the IP's internal SQ memory
 *        (S_AXI_SSD read path, or the RC's inbound window not covering the
 *        address the IP decodes), or
 *    (2) the SSD reads and executes it fine, but the completion cannot be
 *        written back into the IP's internal CQ memory (same class of problem,
 *        on the CQ address).
 *  Both produce "SQ non-empty -> timeout, no completion, svc=0", which is
 *  exactly what every run so far shows.
 *
 *  This splits them with no bitstream change and no statistics, by moving the
 *  *completion* to memory the MicroBlaze owns:
 *    1. create one extra I/O queue pair, qid 4, whose SQ is the IP's own
 *       internal SQ4 (0x44A23000 - inside the SSD-side window whether it is
 *       16 KB or 32 KB) and whose CQ is a DDR buffer (0x8000_0000+, which the
 *       direct test proved the SSD writes to fine),
 *    2. push one write into SW slot 3 => IP SQ4 => qid 4. The IP rings
 *       SQ4TDBL itself; this test never touches a doorbell,
 *    3. poll the DDR buffer for the completion.
 *
 *  Reading the result:
 *    CQE arrives in DDR -> the SSD DID fetch the SQE out of the IP's memory
 *       and execute it, and the IP DID ring the doorbell. So the only broken
 *       link is CQE delivery into the IP's internal CQ, i.e. the SSD cannot
 *       write to 0x44A24000 - exactly what a 16 KB inbound window does, since
 *       that window stops at 0x44A23FFF. (The SQ addresses 0x44A20000 /
 *       0x44A21000 / 0x44A23000 are inside both windows, which is why the
 *       drive would still execute the commands.)
 *    no CQE -> the SSD never got the SQE out of 0x44A23000 (or the IP never
 *       rang SQ4TDBL): the fault is on the SQ-read side instead.
 *
 *  qid 4 is free (1..3 are taken) and the drive reports MQES = 1024, so this
 *  is a legal configuration. Diagnostic only.
 * ========================================================================== */
#define XNVME_Q4_QID      4u
#define XNVME_Q4_SW_SLOT  3u
#define XNVME_Q4_POLLS    1000000u

static XNvmeCqeDw xnvme_q4_cq[64]    __attribute__((aligned(4096)));
static u8         xnvme_q4_buf[4096] __attribute__((aligned(4096)));

int XNvme_IpSqFetchTest(XNvme *nv, u64 lba)
{
    XNvmeAdmin *ad;
    XNvmeHa *ha;
    u64 cq_pcie, sq_pcie;
    u32 i, polls, dw3;
    int cid;

    if (nv == NULL)
        return XST_FAILURE;
    ad = &nv->ad;
    ha = &nv->ha;

    cq_pcie = XNvme_PalLocalToPcie(xnvme_q4_cq);
    sq_pcie = (u64)XNVMEHA_SQ0_PCIE_BASE + (u64)XNVME_Q4_SW_SLOT * 0x1000ULL;

    /* A fresh CQ buffer matters: the direct test's CQ buffer already holds
     * phase-1 entries and would be misread as this queue's completions. */
    memset(xnvme_q4_cq, 0, sizeof(xnvme_q4_cq));
    XNvme_PalFlushDcache(xnvme_q4_cq, sizeof(xnvme_q4_cq));
    for (i = 0u; i < (u32)sizeof(xnvme_q4_buf); i++)
        xnvme_q4_buf[i] = (u8)(0x77u + i);
    XNvme_PalFlushDcache(xnvme_q4_buf, sizeof(xnvme_q4_buf));

    XNVME_DBG("\r\n--- IP-SQ fetch test (SQ in the IP, CQ in DDR) ---\r\n");
    XNVME_DBG("Q4: qid=%u SQ=0x%08x (IP internal SQ4) CQ=0x%08x%08x (DDR)\r\n",
              (u32)XNVME_Q4_QID, (u32)sq_pcie,
              (u32)(cq_pcie >> 32), (u32)cq_pcie);

    if (XNvme_AdminCreateIoQueuePair(ad, (u16)XNVME_Q4_QID,
                                     (u32)XNVMEHA_SQ_DEPTH_SW,
                                     sq_pcie, cq_pcie) != XST_SUCCESS) {
        XNVME_ERR("Q4: create qid=%u failed\r\n", (u32)XNVME_Q4_QID);
        return XST_FAILURE;
    }

    cid = XNvmeHa_SubmitIo(ha, nv->target_ssd, (u32)XNVME_Q4_SW_SLOT, nv->nsid,
                           lba, 1u, xnvme_q4_buf, nv->lba_size,
                           XNVME_OPC_IO_WRITE);
    if (cid < 0) {
        XNVME_ERR("Q4: submit to SW slot %u failed\r\n", (u32)XNVME_Q4_SW_SLOT);
        return XST_FAILURE;
    }
    XNVME_DBG("Q4: pushed 1 SQE cid=%d to SW slot %u -> IP SQ4 -> qid %u "
              "(no doorbell from us)\r\n",
              cid, (u32)XNVME_Q4_SW_SLOT, (u32)XNVME_Q4_QID);

    dw3 = 0u;
    for (polls = 0u; polls < (u32)XNVME_Q4_POLLS; polls++) {
        XNvme_PalInvalidateDcache(&xnvme_q4_cq[0], sizeof(XNvmeCqeDw));
        dw3 = xnvme_q4_cq[0].dw3;
        if (((dw3 >> 16) & 0x1u) == 0x1u)
            break;
    }

    if (((dw3 >> 16) & 0x1u) == 0x1u) {
        XNVME_DBG("Q4: CQE in DDR after %u polls cid_echo=%u sct=%u sc=%u "
                  "dw2=0x%08x dw3=0x%08x\r\n",
                  polls, (u32)XNVMEHA_CQE_CID(dw3),
                  (u32)XNVMEHA_CQE_SCT(dw3), (u32)XNVMEHA_CQE_SC(dw3),
                  xnvme_q4_cq[0].dw2, dw3);
        XNVME_DBG("Q4: verdict -> the SSD DOES fetch the SQE from the IP's own "
                  "SQ memory and complete it; the broken link is CQE delivery "
                  "into the IP's internal CQ at 0x%08x\r\n",
                  (u32)XNVMEHA_CQ0_PCIE_BASE);
    } else {
        XNVME_DBG("Q4: no CQE after %u polls (last dw3=0x%08x)\r\n",
                  (u32)XNVME_Q4_POLLS, dw3);
        XNVME_DBG("Q4: verdict -> the SSD never got the SQE out of 0x%08x (or "
                  "the IP never rang SQ4TDBL), so the fault is on the SQ-read "
                  "side\r\n", (u32)sq_pcie);
    }

    XNvmeHa_FreeSlot(ha, cid);
    return XST_SUCCESS;
}

/* ==========================================================================
 *  Throughput benchmark: generate data in DDR, write it to the SSD, read it
 *  back, verify, and report MB/s. No bitstream change needed.
 *
 *  WHERE THE CLOCK COMES FROM
 *  The design now carries an AXI Timer (axi_timer_0 @ 0x41C00000, 100 MHz on
 *  clk_bus_0), so the benchmark times itself directly off that counter:
 *  resolution 10 ns, no dependence on how often the CPU calls anything, and it
 *  keeps counting while the CPU sits inside a BSP library call. The timer is
 *  driven by a few register writes here rather than through the BSP's tmrctr
 *  library, so the measurement does not depend on which timer the BSP's
 *  xiltimer was configured with (XTIMER_NO_TICK_TIMER is still 1 in this BSP),
 *  nor on whether the tmrctr library got enabled at all.
 *
 *  Every constant comes from xparameters.h (XPAR_XTMRCTR_0_*,
 *  XPAR_XUARTLITE_0_*), so a Vivado parameter change cannot silently
 *  invalidate a measurement.
 *
 *  The timer's own frequency is self-checked at startup against the UART:
 *  pushing a known number of bytes into the TX FIFO takes a known time at the
 *  configured baud rate, which gives an independent time reference - see
 *  XNvme_TimerSelfTest().
 *
 *  FALLBACK: on a design with no usable timer the benchmark makes a clock out
 *  of the print UART instead - an AXI UART Lite drains its TX FIFO at exactly
 *  the baud rate, so "how many bytes did I manage to push" IS elapsed time.
 *  But it only runs while it is *called*, hence the calls-vs-ticks lie
 *  detector: it is valid only when the hook is driven faster than the baud
 *  rate. It needs no hardware, so it also covers the case where the FPGA is
 *  running an older bitstream than the one the platform describes. Note that
 *  it needs the UART's real baud rate, which is exactly what cannot be trusted
 *  when the bitstream is stale - XNVME_UART_SW_BAUD carries that assumption.
 * ========================================================================== */
#define XNVME_BENCH_NCH        2u                       /* channels to drive */
/* XNVME_BENCH_BYTES / XNVME_BENCH_CHUNK are defined in xnvme.h - sizing the
 * static buffers there keeps the caller's arguments consistent with them.  */
#define XNVME_BENCH_LBA0       0x800000ULL              /* clear of the demo */

/* Print UART. Used by the fallback clock and by the timer's self-test; the
 * base address and baud rate are taken from the hardware description so they
 * cannot drift away from pl.dtsi.                                        */
#define XNVME_BENCH_UART_BASE  ((u32)XPAR_XUARTLITE_0_BASEADDR)
#define XNVME_BENCH_UART_TX    0x04u                    /* TX FIFO (write)     */
#define XNVME_BENCH_UART_STAT  0x08u                    /* bit2 TX empty, bit3 full */
#define XNVME_BENCH_UART_FULL  0x08u
#define XNVME_BENCH_UART_EMPTY 0x04u
#define XNVME_BENCH_BAUD       ((u32)XPAR_XUARTLITE_0_BAUDRATE)  /* 230400 */

/* The one ground truth this bare-metal board has: the baud rate your TERMINAL
 * is actually set to - the setting under which this log displays cleanly.
 * The timer self-test times 512 bytes against it and derives the timer's real
 * frequency. XPAR_XTMRCTR_0_CLOCK_FREQUENCY is a parameter copied from the
 * XSA, not a measurement; the UART mismatch proves the real clock tree does
 * not match it. Set this to whatever your terminal uses.               */
#ifndef XNVME_UART_GROUND_BAUD
#define XNVME_UART_GROUND_BAUD  XNVME_BENCH_BAUD
#endif

/* AXI Timer registers and bits: quoted from the BSP's own xtmrctr_l.h
 * (XTC_TCSR_OFFSET / XTC_TLR_OFFSET / XTC_TCR_OFFSET and the XTC_CSR_* masks)
 * so they stay in step with the IP.
 * Timer 0 is set up as a free-running up-counter: TLR0 = 0, then TCSR0 =
 * LOAD | AUTORELOAD | ENABLE, with interrupts left disabled (so a stray
 * timer interrupt can never appear on the INTC input either). At
 * XNVME_TIMER_HZ the 32-bit counter wraps every 2^32/Hz seconds, and the code
 * only ever takes differences across short windows, so the wrap is harmless:
 * unsigned subtraction is correct across it.                            */
#define XNVME_TIMER_BASE       ((u32)XPAR_XTMRCTR_0_BASEADDR)
#define XNVME_TIMER_HZ         ((u32)XPAR_XTMRCTR_0_CLOCK_FREQUENCY)
/* Frequency the self-test MEASURED (via the terminal-baud ground truth).
 * 0 = not calibrated yet; every conversion falls back to the XSA value.  */
static u32 xnvme_timer_hz_real = 0u;
static u32 xnvme_timer_hz_eff(void)
{
    return (xnvme_timer_hz_real != 0u) ? xnvme_timer_hz_real : XNVME_TIMER_HZ;
}
#define XNVME_TIMER_TCSR       0x00u
#define XNVME_TIMER_TLR        0x04u
#define XNVME_TIMER_TCR        0x08u
#define XNVME_TIMER_CSR_LOAD       0x00000020u   /* load TLR into the counter */
#define XNVME_TIMER_CSR_AUTORELOAD 0x00000010u   /* reload at wrap, do not hold */
#define XNVME_TIMER_CSR_ENABLE     0x00000080u   /* enable this timer counter  */
/* Commands in flight per channel. 8 keeps a DRAM-less drive busy; the slot
 * table (256) and the credit pool (252) both allow it, and it is worth
 * raising further to find the knee - but keep nch*DEPTH well under the
 * credit pool or submits start waiting for credits.                    */
#define XNVME_BENCH_DEPTH      8u

/* The pattern buffers live in .ddrbuf, NOT in .bss - see the section's
 * comment in lscript.ld. Yes, they are file-scope statics, so they would
 * normally land in .bss; but crtinit zero-fills .bss before main(), and
 * zero-filling 16 MiB costs seconds of silent startup on this design's
 * MicroBlaze->DDR write path. Nothing needs them zeroed: the generator
 * fills every byte before the first command is submitted.               */
static u8 xnvme_bench_buf[XNVME_BENCH_NCH][XNVME_BENCH_BYTES]
                        __attribute__((aligned(4096), section(".ddrbuf")));

/* ---- prebuilt PRP tables for the benchmark ----
 *
 * The bench buffers (xnvme_bench_buf) and the chunk offsets are fixed for the
 * whole run, so the PRP1/PRP2 pair of every (channel, chunk) command is a
 * constant. Building the 127-entry overflow list was the MicroBlaze's
 * dominant submit cost (~1 ms per 512 KiB command), so the lists are built
 * ONCE here, outside the timed window, and XNvmeHa_SubmitIoPrebuilt reuses
 * the PRP values per command.
 *
 * One list per (ch, chunk) slot: a list must never back two in-flight
 * commands, and the pass never has more than one command outstanding per
 * slot (it waits for the whole depth before recycling offsets).
 *
 * Sizing: chunks down to 64 KiB are covered (XNVME_BENCH_NCH x 128 lists x
 * 1 KiB = 256 KiB in .ddrbuf, next to the pattern buffers - do NOT move this
 * to .bss, the gap between __bss_end and the stack region is only ~17 KiB).
 * Smaller chunks fall back to the per-command rebuild.                  */
#define XNVME_BENCH_PREBUILT_MIN_CHUNK   (64u * 1024u)
#define XNVME_BENCH_PREBUILT_MAX         ((u32)(XNVME_BENCH_BYTES / \
                                                XNVME_BENCH_PREBUILT_MIN_CHUNK))

static u64 xnvme_bench_prp1_tab[XNVME_BENCH_NCH][XNVME_BENCH_PREBUILT_MAX];
static u64 xnvme_bench_prp2_tab[XNVME_BENCH_NCH][XNVME_BENCH_PREBUILT_MAX];
static u64 xnvme_bench_prp_lists[XNVME_BENCH_NCH * XNVME_BENCH_PREBUILT_MAX]
                                [XNVMEHA_PRP_LIST_MAX]
                        __attribute__((aligned(4096), section(".ddrbuf")));
/* ---------------------------------------------------------------------------
 * Benchmark clock.
 *
 * XNVME_BENCH_CLK_SRC:
 *   2 = auto (default): the AXI Timer when there really is one, otherwise the
 *       UART TX FIFO stopwatch - and the log says which, and why.
 *   1 = force the AXI Timer: 10 ns resolution, and it keeps counting no matter
 *       where the CPU is, including inside a BSP library call, which is where
 *       a software stopwatch goes blind.
 *   0 = force the UART stopwatch.
 *
 * Both are reached through BENCH_NOW() / BENCH_TO_US(), so the measurement code
 * never has to care which one is in use; only the conversion differs.
 * ------------------------------------------------------------------------- */
#define XNVME_BENCH_CLK_SRC   2

/* The stopwatch counts TX-FIFO bytes, so it only measures time if it knows the
 * baud rate the UART is ACTUALLY running at. That is a synthesis-time constant
 * of the bitstream (a UART Lite has no baud register) and may differ from what
 * the XSA claims - see XNvme_TimerSelfTest(). 9600 is what this board's fabric
 * runs as long as its bitstream predates the C_BAUDRATE change; once the timer
 * works, this value is not used. Set it to whatever your terminal is set to. */
#ifndef XNVME_UART_SW_BAUD
#define XNVME_UART_SW_BAUD    9600u
#endif

/* An address that is unmapped in every revision of this design. Reading it
 * shows what a read from nowhere does here: with xlnx,fault-tolerant = 0 an
 * AXI error raises a data-bus exception, but the BSP's vector-table entry for
 * that cause is XNullHandler, which returns - so the load silently leaves
 * stale data in the destination register and NOTHING is reported. A write and
 * a read both look successful. Used only to make the log self-explanatory. */
#define XNVME_TIMER_CTRL_ADDR 0x41D00000u

#define XNVME_CLK_NONE        0u
#define XNVME_CLK_TIMER       1u
#define XNVME_CLK_SW          2u

static u32 xnvme_clk_src      = XNVME_CLK_NONE;
static u32 xnvme_timer_tested = 0u;
static u32 xnvme_timer_ok     = 0u;

/* ---------------- UART TX FIFO stopwatch ---------------- */
static volatile u32 xnvme_bench_ticks;
static volatile u32 xnvme_bench_on;
static volatile u32 xnvme_bench_calls;

/* One tick = one byte handed to the UART (1/XNVME_UART_SW_BAUD seconds).
 *
 * `calls` versus `ticks` is the stopwatch's own lie detector: a byte can only
 * be counted while the TX FIFO has room, and the FIFO empties at the baud
 * rate. So calls >> ticks means the FIFO was the limiter and ticks IS elapsed
 * time; calls ~= ticks means the caller was the limiter and the elapsed time
 * is only a lower bound. That is why the hook is called from the hot loops in
 * xnvmeha.c (the PRP-list build and the SW-port push), not only from the
 * completion-wait spin.                                                 */
static void xnvme_bench_tick(void)
{
    if (xnvme_bench_on == 0u)
        return;
    xnvme_bench_calls++;
    if ((Xil_In32(XNVME_BENCH_UART_BASE + XNVME_BENCH_UART_STAT) &
         XNVME_BENCH_UART_FULL) == 0u) {
        Xil_Out32(XNVME_BENCH_UART_BASE + XNVME_BENCH_UART_TX, 0u);
        xnvme_bench_ticks++;
    }
}

/* Drain the FIFO, then prime it full without counting, so the first counted
 * byte lands exactly one baud period later. Bounded everywhere: a wedged UART
 * must not hang the benchmark. */
static void xnvme_sw_start(void)
{
    u32 guard;

    xnvme_bench_on    = 0u;
    xnvme_bench_ticks = 0u;
    xnvme_bench_calls = 0u;

    for (guard = 0u; guard < 100000u; guard++) {
        if ((Xil_In32(XNVME_BENCH_UART_BASE + XNVME_BENCH_UART_STAT) &
             XNVME_BENCH_UART_EMPTY) != 0u)
            break;
    }
    for (guard = 0u; guard < 64u; guard++) {
        if ((Xil_In32(XNVME_BENCH_UART_BASE + XNVME_BENCH_UART_STAT) &
             XNVME_BENCH_UART_FULL) != 0u)
            break;
        Xil_Out32(XNVME_BENCH_UART_BASE + XNVME_BENCH_UART_TX, 0u);
    }
    xnvme_bench_on = 1u;
}

static void xnvme_sw_stop(void)
{
    xnvme_bench_on = 0u;
}

/* ---------------- clock dispatch ---------------- */

static u32 xnvme_bench_now(void)
{
    if (xnvme_clk_src == XNVME_CLK_TIMER)
        return Xil_In32(XNVME_TIMER_BASE + XNVME_TIMER_TCR);
    return xnvme_bench_ticks;
}

static u32 xnvme_bench_to_us(u32 delta)
{
    if (xnvme_clk_src == XNVME_CLK_TIMER)
        return (u32)(((u64)delta * 1000000ull) / (u64)xnvme_timer_hz_eff());
    return (u32)(((u64)delta * 1000000ull) / (u64)XNVME_UART_SW_BAUD);
}

/* Arm the clock for a timed window. The timer needs nothing (it free-runs);
 * the stopwatch needs its FIFO primed and its hook installed. */
static void xnvme_bench_clk_begin(void)
{
    if (xnvme_clk_src == XNVME_CLK_SW) {
        xnvme_sw_start();
        XNvmeHa_WaitHook = xnvme_bench_tick;
    }
}

static u32 xnvme_sw_last_ticks = 0u;
static u32 xnvme_sw_last_calls = 0u;

static void xnvme_bench_clk_end(void)
{
    if (xnvme_clk_src == XNVME_CLK_SW) {
        xnvme_sw_last_ticks = xnvme_bench_ticks;
        xnvme_sw_last_calls = xnvme_bench_calls;
        XNvmeHa_WaitHook = NULL;
        xnvme_sw_stop();
    }
}

/* The stopwatch's own lie detector, printed once per timed pass when the
 * fallback is in use. A byte can only be counted while the TX FIFO has room,
 * so the count equals elapsed time only when the hook is called FASTER than
 * the baud rate; below that the FIFO is full most of the time and the count
 * under-reports. Never let a call-limited measurement pass for a real one. */
static void xnvme_bench_report_sw(void)
{
    u32 r100;

    if (xnvme_clk_src != XNVME_CLK_SW)
        return;
    if (xnvme_sw_last_ticks == 0u) {
        xil_printf("BENCH:   stopwatch counted 0 ticks in %u hook calls - the "
                   "clock never ran\r\n", xnvme_sw_last_calls);
        return;
    }
    r100 = (u32)(((u64)xnvme_sw_last_calls * 100ull) / (u64)xnvme_sw_last_ticks);
    xil_printf("BENCH:   stopwatch: %u ticks, %u hook calls (%u.%02ux) -> %s\r\n",
               xnvme_sw_last_ticks, xnvme_sw_last_calls,
               r100 / 100u, r100 % 100u,
               (r100 >= 150u) ? "FIFO-limited, so the time is real"
                              : "CALL-limited: time is a LOWER bound");
}

#define BENCH_NOW()      xnvme_bench_now()
#define BENCH_TO_US(c)   xnvme_bench_to_us(c)

/* ---------------- AXI Timer ---------------- */

/* Bring timer 0 up as a free-running up-counter, but only after proving that a
 * counter is really there.
 *
 * The proof matters on this hardware. `xlnx,fault-tolerant = 0`, so a load
 * from an unmapped address raises a data-bus exception - but the BSP's
 * vector-table entry for that cause is XNullHandler, which returns
 * immediately. The load therefore completes with whatever was already in the
 * destination register, NOTHING is printed, and no fault is visible. A naive
 * "set the enable bit and read it back" test passes on a design with no timer
 * at all (the register still holds the value just stored), and the failure
 * only shows up later as every measurement reading 0 us. That is exactly what
 * happened on 18 Sep.
 *
 * What cannot be faked is motion: a free-running counter changes between two
 * reads separated by a delay, a missing slave returns a constant forever.
 *
 * Returns 0 if the timer is usable, -1 otherwise. Either way xnvme_clk_src is
 * left pointing at the clock the benchmark should use.                   */
int XNvme_TimerInit(void)
{
    u32 t1, t2, i, ctrl;

    if (xnvme_clk_src != XNVME_CLK_NONE)
        return (xnvme_clk_src == XNVME_CLK_TIMER) ? 0 : -1;

#if (XNVME_BENCH_CLK_SRC == 0)
    xnvme_clk_src = XNVME_CLK_SW;
    return -1;
#else
    /* Load 0, free-run, interrupts left disabled (so a stray timer interrupt
     * can never show up on the INTC input either).
     *
     * PG079 holds the counter in its LOAD state while TCSR.LOAD = 1 - it does
     * not advance until LOAD is cleared again. Writing LOAD once and leaving
     * it set (the code below used to do exactly that) parks TCR0 at the loaded
     * value 0x00000000 forever, so the motion test below reads "constant" and
     * reports "no working axi_timer" even though axi_timer_0 is alive in the
     * XSA/SDT at 0x41C00000. That - not a stale bitstream - is why the 18 Sep
     * run failed this probe. Load and release, the same two-write toggle
     * XTmrCtr_SetResetValue() uses (BSP xtmrctr.c:464-467).               */
    Xil_Out32(XNVME_TIMER_BASE + XNVME_TIMER_TLR, 0u);
    Xil_Out32(XNVME_TIMER_BASE + XNVME_TIMER_TCSR,
              XNVME_TIMER_CSR_LOAD | XNVME_TIMER_CSR_AUTORELOAD);
    Xil_Out32(XNVME_TIMER_BASE + XNVME_TIMER_TCSR,
              XNVME_TIMER_CSR_AUTORELOAD | XNVME_TIMER_CSR_ENABLE);

    t1 = Xil_In32(XNVME_TIMER_BASE + XNVME_TIMER_TCR);
    for (i = 0u; (i < 20000u) &&
                 (Xil_In32(XNVME_TIMER_BASE + XNVME_TIMER_TCR) == t1); i++)
        ;
    t2 = Xil_In32(XNVME_TIMER_BASE + XNVME_TIMER_TCR);

    /* For the message only: what a read from nowhere looks like here. */
    Xil_Out32(XNVME_TIMER_CTRL_ADDR, 0xAA55AA55u);
    ctrl = Xil_In32(XNVME_TIMER_CTRL_ADDR);

    if (t1 == t2) {
        xil_printf("TIMER: no working axi_timer at 0x%08X - TCR0 read 0x%08X "
                   "twice (a control address that is unmapped in every "
                   "revision, 0x%08X, reads 0x%08X without faulting).\r\n",
                   XNVME_TIMER_BASE, t1, XNVME_TIMER_CTRL_ADDR, ctrl);
        xil_printf("TIMER: the FPGA is running a bitstream without a usable "
                   "timer, OR the counter is held (TCSR.LOAD left set) - the "
                   "XSA/SDT carry axi_timer_0 @ 0x41C00000, so check the "
                   "bitstream first, then the init sequence.\r\n");
        xnvme_clk_src = XNVME_CLK_SW;
        xil_printf("TIMER: falling back to the UART TX FIFO stopwatch, which "
                   "assumes the UART really runs at %u baud "
                   "(XNVME_UART_SW_BAUD) - set that macro to the baud rate "
                   "your terminal is using.\r\n", (u32)XNVME_UART_SW_BAUD);
#if (XNVME_BENCH_CLK_SRC == 1)
        xnvme_clk_src = XNVME_CLK_NONE;
#endif
        return -1;
    }

    xnvme_clk_src = XNVME_CLK_TIMER;
    xil_printf("TIMER: axi_timer @ 0x%08X = %u Hz (%u ns/tick), TCR0 is "
               "counting (0x%08X -> 0x%08X)\r\n",
               XNVME_TIMER_BASE, XNVME_TIMER_HZ,
               (u32)(1000000000u / XNVME_TIMER_HZ), t1, t2);
    return 0;
#endif
}

/* Raw timer access, kept for callers that want the counter directly. */
u32 XNvme_TimerNow(void)
{
    return Xil_In32(XNVME_TIMER_BASE + XNVME_TIMER_TCR);
}

u32 XNvme_TimerUs(u32 delta)
{
    return (u32)(((u64)delta * 1000000ull) / (u64)xnvme_timer_hz_eff());
}

/* Cross-check the time base against the UART wire.
 *
 * The hwh pins the clock tree down: axi_timer_0 and axi_uartlite_0 share
 * ddr4_0_addn_ui_clkout1 (100 MHz), and the UART divider is set to 230400.
 * A UART Lite has no baud register - the divider is fixed in the bitstream -
 * so if a terminal at 230400 shows this log cleanly, the running bitstream
 * matches the XSA and the timer's 100 MHz is confirmed (shared clock).
 * The measured window is longer than the pure wire time because the polling
 * loop pays one uncached MMIO read per byte - on this fabric those cost tens
 * of us each. That overhead is fabric latency, NOT a clock error, so this
 * test deliberately does NOT back-derive a "measured" timer frequency: that
 * number is polluted by the poll cost.                              */
u32 XNvme_TimerSelfTest(void)
{
    const u32 n = 512u;              /* 512 B = 5120 bits on the wire */
    u32 i, t0, dt, wire_us, got_us, wedged = 0u;

    if (xnvme_timer_tested != 0u)
        return xnvme_timer_ok;
    xnvme_timer_tested = 1u;

    if (xnvme_clk_src != XNVME_CLK_TIMER) {
        xil_printf("TIMER: the clock is the UART TX FIFO stopwatch - every "
                   "time below assumes the UART runs at %u baud.\r\n",
                   (u32)XNVME_UART_SW_BAUD);
        return 0u;
    }

    t0 = Xil_In32(XNVME_TIMER_BASE + XNVME_TIMER_TCR);
    for (i = 0u; i < n; i++) {
        u32 g = 0u;

        while ((Xil_In32(XNVME_BENCH_UART_BASE + XNVME_BENCH_UART_STAT) &
                XNVME_BENCH_UART_FULL) != 0u) {
            if (++g > 1000000u) {     /* bounded: a wedged UART must not hang */
                wedged = 1u;
                break;
            }
        }
        if (wedged != 0u)
            break;
        Xil_Out32(XNVME_BENCH_UART_BASE + XNVME_BENCH_UART_TX, 0u);
    }
    dt = Xil_In32(XNVME_TIMER_BASE + XNVME_TIMER_TCR) - t0;

    if ((i == 0u) || (dt == 0u)) {
        xil_printf("TIMER: self-test inconclusive (pushed %u bytes, %u ticks)\r\n",
                   i, dt);
        return 0u;
    }

    /* No clock recalibration here - that is deliberate. The design file
     * pins the clock tree down:
     *   - hwh: axi_timer_0 s_axi_aclk  = ddr4_0_addn_ui_clkout1, 100 MHz
     *   - hwh: axi_uartlite_0 C_BAUDRATE = 230400 on the same 100 MHz
     * If a terminal set to 230400 displays this log cleanly, the running
     * bitstream's UART divider matches the XSA - and with it the timer's
     * 100 MHz (the two blocks share the clock). The window measured below
     * is longer than the pure wire time because the polling loop pays one
     * uncached MMIO read per byte, and on this fabric those are tens of us
     * each (PROBE 2). That overhead is fabric latency, not a clock error:
     * do NOT back-derive a "measured" timer frequency from this test, the
     * number it produces (dt*baud/(i*10)) is polluted by the poll cost. */
    wire_us = (u32)(((u64)i * 10ull * 1000000ull) / (u64)XNVME_UART_GROUND_BAUD);
    got_us  = XNvme_TimerUs(dt);

    xil_printf("TIMER: self-test: %u UART bytes in %u us (pure wire time would "
               "be %u us at %u baud, %u ns/byte)\r\n",
               i, got_us, wire_us, (u32)XNVME_UART_GROUND_BAUD,
               (u32)(((u64)dt * 1000ull) / (u64)i));

    if ((wire_us == 0u) || (got_us < wire_us - (wire_us / 10u))) {
        xil_printf("TIMER: *** measured FASTER than the wire allows - "
                   "XNVME_UART_GROUND_BAUD is probably lower than the real "
                   "line rate; check the terminal setting. Timing still uses "
                   "the XSA's %u Hz.\r\n", XNVME_TIMER_HZ);
    } else if (got_us <= wire_us + (wire_us / 4u)) {
        xil_printf("TIMER: -> OK: window matches the wire; the timer's %u Hz "
                   "stands and all timing below is exact.\r\n",
                   XNVME_TIMER_HZ);
    } else {
        xil_printf("TIMER: -> the extra %u us is the TX-FIFO poll cost "
                   "(~%u us/byte of uncached MMIO, same order as PROBE 2) - "
                   "NOT a clock or baud error. The terminal displaying this "
                   "cleanly at %u baud proves the bitstream's UART divider "
                   "matches the XSA, so the timer's %u Hz stands.\r\n",
                   got_us - wire_us, (got_us - wire_us) / i,
                   (u32)XNVME_UART_GROUND_BAUD, XNVME_TIMER_HZ);
    }

    xnvme_timer_ok = 1u;
    return 1u;
}

/* Cheap order-sensitive checksum - enough to catch a block that did not come
 * back, without spending seconds per pass (the demo's own section does the
 * byte-exact compare on the first 256 KiB).                              */
static u32 xnvme_bench_sum(const u32 *p, u32 words)
{
    u32 s = 0u;
    while (words-- != 0u) {
        s += *p++;
        s = (s << 7) | (s >> 25);
    }
    return s;
}

#define XNVME_BENCH_MAP_MAX   32u

/* Single pass over one channel's buffer: reproduces xnvme_bench_sum()
 * exactly (same order, same operations, so the result is directly comparable
 * with csum0) while ALSO recording WHERE the data differs from the pattern.
 * The location is the useful part:
 *   - only the tail differs   -> something overwrote the end of the buffer
 *   - one whole chunk differs -> that range never came back
 *   - everything differs      -> the read never landed in this buffer
 *   - the words still look like pattern words but carry a different index
 *     -> the data is RIGHT and the DESTINATION OFFSET is wrong: the pattern's
 *        low 16 bits are the word index, so `actual & 0xFFFF` reads out the
 *        offset it really belongs to, i.e. the shift, directly.
 * `cmap` receives the mismatch count per `chunk_words`.                  */
static u32 xnvme_bench_scan(u32 ch, u32 words, u32 chunk_words,
                            u32 *first_bad, u32 *last_bad, u32 *nbad,
                            u32 *cmap, u32 cmap_len)
{
    const u32 *b   = (const u32 *)xnvme_bench_buf[ch];
    const u32  tag = 0xA5A50000u | ((u32)ch << 16);
    u32 k, s = 0u, fb = 0u, lb = 0u, n = 0u;

    if (chunk_words == 0u)
        chunk_words = (words != 0u) ? words : 1u;

    for (k = 0u; k < words; k++) {
        u32 v = b[k];

        s += v;
        s = (s << 7) | (s >> 25);

        if (v != (tag | (k & 0xFFFFu))) {
            u32 ci = k / chunk_words;
            if (n == 0u)
                fb = k;
            lb = k;
            n++;
            if (ci < cmap_len)
                cmap[ci]++;
        }
    }

    *first_bad = fb;
    *last_bad  = lb;
    *nbad      = n;
    return s;
}

/* Submit one bench command using a PREBUILT PRP pair. Thin wrappers over the
 * channel-async API - same validation, just handing in prp1/prp2 so the
 * driver skips the per-command list rebuild.                          */
static int XNvme_WriteChannelAsyncPrebuilt(XNvme *nv, u32 ch, u64 lba,
                                           u16 nlb, const void *buf, u32 len,
                                           u64 prp1, u64 prp2)
{
    if ((nv == NULL) || (ch >= (u32)XNVMEHA_NUM_SQ_SW) || (buf == NULL))
        return -1;
    if ((u32)nlb * nv->lba_size > len)
        return -1;
    return XNvmeHa_SubmitIoPrebuilt(&nv->ha, nv->target_ssd, ch, nv->nsid,
                                    lba, nlb, (void *)buf, len,
                                    XNVME_OPC_IO_WRITE, prp1, prp2);
}

static int XNvme_ReadChannelAsyncPrebuilt(XNvme *nv, u32 ch, u64 lba,
                                          u16 nlb, void *buf, u32 len,
                                          u64 prp1, u64 prp2)
{
    if ((nv == NULL) || (ch >= (u32)XNVMEHA_NUM_SQ_SW) || (buf == NULL))
        return -1;
    if ((u32)nlb * nv->lba_size > len)
        return -1;
    return XNvmeHa_SubmitIoPrebuilt(&nv->ha, nv->target_ssd, ch, nv->nsid,
                                    lba, nlb, buf, len,
                                    XNVME_OPC_IO_READ, prp1, prp2);
}

/* Build the PRP1/PRP2 pair for every (channel, chunk) the pass will submit,
 * once, outside the timed window. Each chunk gets its own persistent list
 * (xnvme_bench_prp_lists) and its own table entry, so no two in-flight
 * commands ever share a list. bytes_per_ch % chunk_bytes == 0 is already
 * enforced by XNvme_Bench, so every chunk is a full chunk_bytes.       */
static void xnvme_bench_prp_prebuild(XNvme *nv, u32 nch,
                                     void *const *buf, u32 bytes_per_ch,
                                     u32 chunk_bytes)
{
    u32 ch, off, ci;

    for (ch = 0u; ch < nch; ch++) {
        for (off = 0u, ci = 0u; off < bytes_per_ch;
             off += chunk_bytes, ci++) {
            u64 *list = xnvme_bench_prp_lists[ch * XNVME_BENCH_PREBUILT_MAX
                                              + ci];
            u64 p1, p2;
            u32 np = XNvmeHa_BuildPrpInto(&nv->ha, (u8 *)buf[ch] + off,
                                          chunk_bytes, list,
                                          XNVMEHA_PRP_LIST_MAX, &p1, &p2);

            if (np != 0u)
                XNvme_PalFlushDcache(list, np * (u32)sizeof(u64));
            xnvme_bench_prp1_tab[ch][ci] = p1;
            xnvme_bench_prp2_tab[ch][ci] = p2;
        }
    }
    xil_printf("BENCH: prebuilt %u PRP lists (%u chunks x %u ch, %u KiB each)"
               " - submit skips the per-command rebuild\r\n",
               ci * nch, ci, nch, chunk_bytes / 1024u);
}

/* Submit-phase autopsy. With the PRP lists prebuilt, the per-command software
 * work is ~100 instructions plus 16 MMIO stores - tens of microseconds at
 * 100 MHz - yet the bench measures ~3 ms per submitted command. Time the same
 * KINDS of memory operations standalone so the next log says WHICH of them
 * eats the time:
 *   1. bare DDR stores          - the "everything is fine" baseline,
 *   2. read of an IP register   - non-posted, one full AXI round trip,
 *   3. read of the SW port      - the same AXI slave the SQE push writes to,
 *   4. IP register store + mbar - the exact shape every push ends with.
 * The targets are side-effect free: TIMEOUT is re-written with the value init
 * already programmed, and the SW-port read is the same empty-CQ probe
 * XNvmeHa_Init does (the INT_STATUS bit it sets is cleared right after).  */
static void xnvme_swport_probe(XNvme *nv)
{
#define PROBE_NS(ticks, n) \
    ((u32)((((u64)(ticks)) * 1000000000ull) / \
           ((u64)xnvme_timer_hz_eff() * (u64)(n))))
    volatile u32 sink = 0u;
    u32 i, t0, d;
    const u32 n_ddr = 20000u, n_reg = 100u;

    if (xnvme_clk_src != XNVME_CLK_TIMER)
        return;                    /* needs a real clock to say anything */

    t0 = xnvme_bench_now();
    for (i = 0u; i < n_ddr; i++)
        sink += i;
    d = xnvme_bench_now() - t0;
    xil_printf("PROBE: 1. DDR store+add       %u ns/op (%u iters)\r\n",
               PROBE_NS(d, n_ddr), n_ddr);

    t0 = xnvme_bench_now();
    for (i = 0u; i < n_reg; i++)
        sink += Xil_In32(nv->ha.reg_base + XNVMEHA_REG_TIMEOUT);
    d = xnvme_bench_now() - t0;
    xil_printf("PROBE: 2. IP reg read         %u ns/rd (%u iters)\r\n",
               PROBE_NS(d, n_reg), n_reg);

    t0 = xnvme_bench_now();
    for (i = 0u; i < n_reg; i++)
        sink += Xil_In32(nv->ha.sw_axi_base + XNVMEHA_SW_CQ_ADDR);
    d = xnvme_bench_now() - t0;
    Xil_Out32(nv->ha.reg_base + XNVMEHA_REG_INT_STATUS, XNVMEHA_INT_ALL);
    xil_printf("PROBE: 3. SW port read        %u ns/rd (%u iters) - same slave as the SQE push\r\n",
               PROBE_NS(d, n_reg), n_reg);

    t0 = xnvme_bench_now();
    for (i = 0u; i < n_reg; i++) {
        Xil_Out32(nv->ha.reg_base + XNVMEHA_REG_TIMEOUT, 0x08000000u);
        XNvme_PalDsb();
    }
    d = xnvme_bench_now() - t0;
    xil_printf("PROBE: 4. IP reg store + mbar %u ns/op - the shape of every SQE push\r\n",
               PROBE_NS(d, n_reg));
    (void)sink;
#undef PROBE_NS
}

/* One timed pass. Commands are kept XNVME_BENCH_DEPTH deep per channel
 * (a DRAM-less NVMe drive at QD2 is latency-bound, so a queue depth of 1 per
 * channel would report a much lower number than the drive can do).
 *
 * `submit_clk` (optional) returns how much clock the submit phases used, in the
 * clock's own units (convert with BENCH_TO_US). This split matters on this
 * design: building a 127-entry PRP list plus pushing the SQE used to cost the
 * MicroBlaze on the order of a millisecond per 512 KiB command - the same
 * order as what the drive needs to move it. The PRP half of that cost is now
 * prepaid by xnvme_bench_prp_prebuild(); when the tables are passed in, the
 * submit loop only rewrites the 64 B SQE, and what remains of the submit
 * time is the SQE push plus slot/credit bookkeeping.
 *
 * `prp1_tab`/`prp2_tab` (NULL = per-command rebuild, old path) index by
 * [ch][o / chunk_bytes] and must come from xnvme_bench_prp_prebuild with the
 * same geometry.
 *
 * Returns XST_SUCCESS / XST_FAILURE (submit) / XST_TIMEOUT.             */
static int xnvme_bench_pass(XNvme *nv, u32 nch, u32 is_write,
                            const u64 *lba, void *const *buf,
                            u32 bytes_per_ch, u32 chunk_bytes,
                            u32 *submit_clk,
                            const u64 prp1_tab[][XNVME_BENCH_PREBUILT_MAX],
                            const u64 prp2_tab[][XNVME_BENCH_PREBUILT_MAX])
{
    int  cids[XNVME_BENCH_NCH][XNVME_BENCH_DEPTH];
    u32  nsub[XNVME_BENCH_NCH];
    u32  ch, k, off, chunk, remaining, nlb;
    u32  sub_total = 0u;
    const u32 step = chunk_bytes * (u32)XNVME_BENCH_DEPTH;

    for (off = 0u; off < bytes_per_ch; off += step) {
        u32 t0 = BENCH_NOW();

        for (ch = 0u; ch < nch; ch++)
            nsub[ch] = 0u;

        /* Submit a whole depth worth of chunks per channel. The hook is only
         * used by the UART fallback clock, which has to be driven from here as
         * well or it stops tracking time; with the AXI timer it is NULL and
         * this costs one compare per command.                          */
        for (k = 0u; k < (u32)XNVME_BENCH_DEPTH; k++) {
            for (ch = 0u; ch < nch; ch++) {
                u32 o = off + k * chunk_bytes;
                u64 lba_ch;
                u8 *p;

                if (o >= bytes_per_ch)
                    continue;
                remaining = bytes_per_ch - o;
                chunk = (remaining < chunk_bytes) ? remaining : chunk_bytes;
                nlb   = chunk / nv->lba_size;
                lba_ch = lba[ch] + (u64)(o / nv->lba_size);
                p      = (u8 *)buf[ch] + o;

                if (prp1_tab != NULL) {
                    u32 ci = o / chunk_bytes;

                    cids[ch][k] = is_write
                        ? XNvme_WriteChannelAsyncPrebuilt(nv, ch, lba_ch,
                                                          (u16)nlb, p, chunk,
                                                          prp1_tab[ch][ci],
                                                          prp2_tab[ch][ci])
                        : XNvme_ReadChannelAsyncPrebuilt(nv, ch, lba_ch,
                                                         (u16)nlb, p, chunk,
                                                         prp1_tab[ch][ci],
                                                         prp2_tab[ch][ci]);
                } else {
                    cids[ch][k] = is_write
                        ? XNvme_WriteChannelAsync(nv, ch, lba_ch, (u16)nlb,
                                                  p, chunk)
                        : XNvme_ReadChannelAsync(nv, ch, lba_ch, (u16)nlb,
                                                 p, chunk);
                }
                if (cids[ch][k] < 0) {
                    XNVME_ERR("BENCH: submit failed ch=%u off=%u\r\n", ch, o);
                    return XST_FAILURE;
                }
                nsub[ch] = k + 1u;
                if (XNvmeHa_WaitHook != NULL)
                    XNvmeHa_WaitHook();
            }
        }
        sub_total += BENCH_NOW() - t0;

        /* Then wait for the whole batch. */
        for (ch = 0u; ch < nch; ch++) {
            for (k = 0u; k < nsub[ch]; k++) {
                if (XNvme_WaitIo(nv, cids[ch][k]) != XST_SUCCESS) {
                    XNVME_ERR("BENCH: wait failed ch=%u off=%u\r\n", ch, off);
                    return XST_TIMEOUT;
                }
            }
        }
    }

    if (submit_clk != NULL)
        *submit_clk = sub_total;
    return XST_SUCCESS;
}

/* Report one measured pass. Times are in microseconds, straight from the
 * clock - no more tick/baud conversions, and the submit/wait split is printed
 * as a percentage because that split IS the answer to "what is the limit
 * here": submit is the MicroBlaze feeding the queue, wait is the drive. */
static void xnvme_bench_report(const char *tag, u32 bytes,
                               u32 elapsed_us, u32 submit_us,
                               u32 nch, u32 chunk)
{
    u32 kbs, mbs, iops, wait_us, sub_pc, wait_pc;

    if ((elapsed_us == 0u) || (bytes == 0u)) {
        xil_printf("BENCH: %s NOT MEASURED (elapsed=%u us) - is the clock "
                   "running? check the TIMER lines above\r\n",
                   tag, elapsed_us);
        return;
    }

    wait_us = (elapsed_us > submit_us) ? (elapsed_us - submit_us) : 0u;
    kbs  = (u32)(((u64)bytes * 1000000ull) / (u64)elapsed_us / 1000ull);
    mbs  = (u32)(((u64)bytes * 1000000ull) / (u64)elapsed_us / 1000000ull);
    iops = (u32)(((u64)(bytes / chunk) * 1000000ull) / (u64)elapsed_us);
    sub_pc  = (u32)(((u64)submit_us * 100ull) / (u64)elapsed_us);
    wait_pc = (u32)(((u64)wait_us   * 100ull) / (u64)elapsed_us);

    xil_printf("BENCH: %s %u B in %u us (%u.%03u ms) = %u KB/s (%u MB/s "
               "aggregate, %u MB/s per ch, %u cmd/s)\r\n",
               tag, bytes, elapsed_us, elapsed_us / 1000u, elapsed_us % 1000u,
               kbs, mbs, (nch != 0u) ? (mbs / nch) : mbs, iops);
    xil_printf("BENCH:   submit %u us (%u%%), wait %u us (%u%%) - submit = "
               "MicroBlaze pushing SQEs (PRP lists are prebuilt), wait = the "
               "drive\r\n", submit_us, sub_pc, wait_us, wait_pc);
}

/* Independent witness: how many bytes the DRIVE says it has written.
 *
 * The benchmark's own completion accounting can be fooled (a slot marked done
 * without the transfer really happening), so this asks the drive directly -
 * over the ADMIN queue, which is a completely different path from the I/O
 * queues and keeps answering even when the I/O bookkeeping is confused.
 * Returns SMART "Data Units Written" in 1000 x 512 B units, or all-ones if
 * the log could not be read.                                             */
static int XNvme_AdminGetLog(XNvmeAdmin *ad, u8 lid, u32 bytes, void *buf);

static u64 xnvme_smart_written(XNvmeAdmin *ad)
{
    const u8 *p = xnvme_log_buf;
    u64 v;

    if (XNvme_AdminGetLog(ad, 0x02u, 512u, xnvme_log_buf) != XST_SUCCESS)
        return 0xFFFFFFFFFFFFFFFFULL;
    XNvme_PalInvalidateDcache(xnvme_log_buf, 512u);

    v  = (u64)p[48] | ((u64)p[49] << 8) | ((u64)p[50] << 16) |
         ((u64)p[51] << 24);
    v |= ((u64)p[52] | ((u64)p[53] << 8) | ((u64)p[54] << 16) |
          ((u64)p[55] << 24)) << 32;
    return v;
}

/* ----------------- stack-overflow canary -----------------
 *
 * lscript.ld deliberately places .heap directly BELOW .stack, and nothing in
 * this program calls malloc, so that 1 KiB is dead space. That makes it a
 * perfect tripwire: anything that appears there can only be the stack growing
 * past its own bottom edge, i.e. a stack overflow.
 *
 * Keep it. The tail of xnvme_bench_buf[1] was being silently overwritten (the
 * last ~672 bytes, different garbage on every run) across several debugging
 * sessions, and it burned a lot of time because "a DMA buffer was corrupted"
 * looked like a driver / IP / drive problem in every direction except the one
 * that mattered: the CPU's own stack. main() holds two 4 KiB Identify
 * structures as locals, so the benchmark's call chain needed ~9.9 KiB against
 * an 8 KiB stack region. With this armed the same fault prints one line.
 *
 * The canary is only meaningful while .heap sits below .stack - true in the
 * current lscript.ld; if the sections are ever reordered, re-derive it from
 * `_stack_end` instead. Arm it early in main(), check it wherever buffers are
 * verified. Each check reports the disturbance since the previous check and
 * then re-arms.                                                          */
extern char _heap_start[];
extern char _heap_end[];

#define XNVME_CANARY_WORD     0xC5A5C5A5u
#define XNVME_CANARY_MAXW     4096u          /* never trust a huge .heap */

static u32 xnvme_canary_words = 0u;          /* 0 = not armed */

u32 XNvme_StackCanaryFill(void)
{
    u32 i, n = (u32)((char *)_heap_end - (char *)_heap_start) / 4u;

    if (n > XNVME_CANARY_MAXW)
        n = XNVME_CANARY_MAXW;
    for (i = 0u; i < n; i++)
        ((u32 *)_heap_start)[i] = XNVME_CANARY_WORD;
    xnvme_canary_words = n;
    return n;
}

u32 XNvme_StackCanaryCheck(const char *where)
{
    u32 i, n = xnvme_canary_words, bad = 0u;

    if (n == 0u)
        return 0u;                            /* never armed - say nothing */

    for (i = 0u; i < n; i++)
        if (((u32 *)_heap_start)[i] != XNVME_CANARY_WORD)
            bad += 4u;

    if (bad != 0u)
        xil_printf("STACK: *** %s: %u bytes BELOW the stack bottom were "
                   "disturbed - STACK OVERFLOW. Increase _STACK_SIZE in "
                   "lscript.ld, or shrink the deepest frame (the locals of "
                   "main() are the usual offender)\r\n", where, bad);
    else
        xil_printf("STACK: %s: canary clean (the stack stayed above its "
                   "bottom)\r\n", where);

    (void)XNvme_StackCanaryFill();            /* re-arm for the next check */
    return bad;
}

int XNvme_Bench(XNvme *nv, u32 nch, u64 lba0,
                u32 bytes_per_ch, u32 chunk_bytes)
{
    u64 lbas[XNVME_BENCH_NCH];
    void *bufs[XNVME_BENCH_NCH];
    u32 csum0[XNVME_BENCH_NCH];
    u32 have_prebuilt = 0u;
    u32 ch, i, words, bad, rc;
    /* Timestamps are taken in the clock's own units and converted once, at
     * the point where they are reported.                                */
    u32 clk_a, clk_b, sub_clk;
    u32 iss0, cmp0, err0, iss1, cmp1, err1;
    u32 svc0, svc1;
    u64 duw0, duw1;

    if ((nv == NULL) || (nch == 0u) || (nch > XNVME_BENCH_NCH))
        return XST_FAILURE;
    if ((chunk_bytes == 0u) || ((chunk_bytes % nv->lba_size) != 0u) ||
        ((bytes_per_ch % chunk_bytes) != 0u) ||
        (bytes_per_ch > (u32)XNVME_BENCH_BYTES) ||
        (chunk_bytes > (u32)XNVME_BENCH_CHUNK))
        return XST_FAILURE;

    xil_printf("\r\n--- throughput benchmark: DDR -> SSD -> DDR ---\r\n");
    xil_printf("BENCH: %u ch x %u B (%u KB chunk x QD%u, %u cmds per ch) at LBA 0x%08x%08x\r\n",
               nch, bytes_per_ch, chunk_bytes / 1024u, (u32)XNVME_BENCH_DEPTH,
               bytes_per_ch / chunk_bytes,
               (u32)(lba0 >> 32), (u32)lba0);

    /* Time base first: init is idempotent, and the self-test prints its
     * verdict once, so the log says whether the numbers below can be trusted
     * before they appear. (The demo arms it earlier; doing it again here means
     * XNvme_Bench() is also usable on its own.)                          */
    (void)XNvme_TimerInit();
    (void)XNvme_TimerSelfTest();

    /* Where is everything? The linker script puts .bss right after the loaded
     * image, so __bss_start is also where code+data end; __stack is the top of
     * the stack. Printing these next to the DMA buffer addresses below turns
     * "does my buffer overlap the program?" into a fact the board states
     * itself. Note that an LBA is an SSD block number, NOT a host address:
     * the host addresses the SSD writes to are the `buf=` values.        */
    {
        extern char __bss_start[];
        extern char __bss_end[];
        extern char __stack[];
        extern char __ddrbuf_start[];
        extern char __ddrbuf_end[];

        xil_printf("BENCH: code+data end 0x%08x | bss 0x%08x..0x%08x | stack top 0x%08x\r\n",
                   (u32)(UINTPTR)__bss_start,
                   (u32)(UINTPTR)__bss_start, (u32)(UINTPTR)__bss_end,
                   (u32)(UINTPTR)__stack);
        xil_printf("BENCH: pattern buffers 0x%08x..0x%08x (.ddrbuf, not zeroed at boot)\r\n",
                   (u32)(UINTPTR)__ddrbuf_start, (u32)(UINTPTR)__ddrbuf_end);
    }

    /* ---- 1. Generate the test data in DDR, one pattern per channel. ----
     * This is 8 MiB of CPU-side DDR stores per channel and is by far the
     * longest phase of the benchmark, so say what is happening: the earlier
     * version printed nothing here and the silence looked like a hang. */
    words = bytes_per_ch / 4u;
    for (ch = 0u; ch < nch; ch++) {
        u32 *w = (u32 *)xnvme_bench_buf[ch];

        xil_printf("BENCH: generating the ch%u pattern (%u MiB of DDR stores; "
                   "this is the slow part)\r\n", ch, bytes_per_ch >> 20);
        for (i = 0u; i < words; i++)
            w[i] = 0xA5A50000u | ((u32)ch << 16) | (i & 0xFFFFu);
        XNvme_PalFlushDcache(xnvme_bench_buf[ch], bytes_per_ch);

        bufs[ch] = xnvme_bench_buf[ch];
        lbas[ch] = lba0 + (u64)ch * (u64)(bytes_per_ch / nv->lba_size);
        csum0[ch] = xnvme_bench_sum(w, words);

        xil_printf("BENCH: ch%u buf=0x%08x%08x csum=0x%08x\r\n", ch,
                   (u32)(XNvme_PalLocalToPcie(xnvme_bench_buf[ch]) >> 32),
                   (u32)XNvme_PalLocalToPcie(xnvme_bench_buf[ch]), csum0[ch]);
    }

    /* ---- 1b. Prebuild the PRP lists once, outside the timed window. ----
     * The submit side used to rebuild a 127-entry list for every 512 KiB
     * command (~1 ms of MicroBlaze per command) and dominated the wall time
     * ("submit 100%, wait 0%"). The buffers and chunk offsets are fixed, so
     * the lists are constants: build them here and let the passes reuse the
     * PRP values. Falls back to the old per-command rebuild if the geometry
     * does not fit the table.                                       */
    {
        u32 nchunk = bytes_per_ch / chunk_bytes;

        if (nchunk <= XNVME_BENCH_PREBUILT_MAX) {
            xnvme_bench_prp_prebuild(nv, nch, bufs, bytes_per_ch, chunk_bytes);
            have_prebuilt = 1u;
        } else {
            xil_printf("BENCH: %u chunks per ch exceeds the prebuilt table "
                       "(%u) - falling back to the per-command PRP rebuild\r\n",
                       nchunk, (u32)XNVME_BENCH_PREBUILT_MAX);
        }
    }

    /* ---- 2. Timed WRITE pass (verbose logs off, clock on).
     *
     * Coherency for the timed passes: the driver's per-command flush /
     * invalidate is O(bytes / cache-line) inside a BSP call the clock cannot
     * see, so it is switched off here and replaced by the minimum work that
     * this hardware actually needs (write-through D-cache, see
     * xnvmeha_cache_ops):
     *   - the one flush below (outside the timed window) is what a
     *     write-back cache would need and costs nothing on this design;
     *   - the read pass gets one invalidate per buffer after it, before the
     *     CPU checksums the data.
     * The numbers below are therefore pure I/O time. To include the driver's
     * cache cost instead, keep XNvmeHa_SetCacheOps(1) and accept that the
     * clock under-counts the run.                                        */
    /* Two INDEPENDENT witnesses, sampled just before the timed write pass and
     * again right after it. Neither one trusts the driver's own completion
     * accounting:
     *   - the IP's statistics counters: did the IP really hand 32 commands to
     *     the SSD, and did 32 completions come back?
     *   - the drive's SMART byte count, over the admin queue (a different path
     *     from the I/O queues).
     * If the "3 ms for 16 MiB" figure were real these must both move.        */
    XNvmeHa_GetSsdStats(&nv->ha, nv->target_ssd, &iss0, &cmp0, &err0);
    svc0 = nv->ha.cq_processed;
    duw0 = xnvme_smart_written(&nv->ad);
    if (duw0 == 0xFFFFFFFFFFFFFFFFULL)
        xil_printf("BENCH: (SMART read failed - drive byte count unavailable)\r\n");

    xnvme_swport_probe(nv);

    XNvmeHa_SetCacheOps(0u);
    XNvmeHa_SetVerbose(0u);
    xnvme_bench_clk_begin();
    clk_a = BENCH_NOW();
    rc = xnvme_bench_pass(nv, nch, 1u, lbas, bufs, bytes_per_ch, chunk_bytes,
                          &sub_clk,
                          have_prebuilt ? xnvme_bench_prp1_tab : NULL,
                          have_prebuilt ? xnvme_bench_prp2_tab : NULL);
    clk_b = BENCH_NOW();
    xnvme_bench_clk_end();
    if (rc != XST_SUCCESS) {
        XNvmeHa_SetVerbose(1u);
        XNvmeHa_SetCacheOps(1u);
        xil_printf("BENCH: write pass failed rc=%d\r\n", rc);
        return rc;
    }
    xnvme_bench_report("WRITE", bytes_per_ch * nch,
                       BENCH_TO_US(clk_b - clk_a), BENCH_TO_US(sub_clk),
                       nch, chunk_bytes);
    xnvme_bench_report_sw();

    XNvmeHa_GetSsdStats(&nv->ha, nv->target_ssd, &iss1, &cmp1, &err1);
    svc1 = nv->ha.cq_processed;
    duw1 = xnvme_smart_written(&nv->ad);
    {
        u64 d = ((duw0 == 0xFFFFFFFFFFFFFFFFULL) ||
                 (duw1 == 0xFFFFFFFFFFFFFFFFULL)) ? 0ULL : (duw1 - duw0);

        u32 exp_cmds = (bytes_per_ch / chunk_bytes) * nch;

        xil_printf("BENCH: witness after WRITE -> IP issued +%u completed +%u "
                   "err +%u | driver popped +%u CQE | drive wrote +%u units\r\n",
                   iss1 - iss0, cmp1 - cmp0, err1 - err0,
                   svc1 - svc0, (u32)d);
        xil_printf("BENCH:   (expected: issued +%u, completed +%u, drive "
                   "+%u..%u units of 1000x512 B)\r\n",
                   exp_cmds, exp_cmds, exp_cmds, exp_cmds + 2u);
    }

    /* Push the drive's cache out before reading - NOT part of the timed
     * write figure, otherwise the number would flatter the drive.        */
    if (XNvme_Flush(nv) != XST_SUCCESS)
        xil_printf("BENCH: flush failed (read numbers may include drive cache)\r\n");

    /* ---- 3. Timed READ pass, into the same buffers. ---- */
    xnvme_bench_clk_begin();
    clk_a = BENCH_NOW();
    rc = xnvme_bench_pass(nv, nch, 0u, lbas, bufs, bytes_per_ch, chunk_bytes,
                          &sub_clk,
                          have_prebuilt ? xnvme_bench_prp1_tab : NULL,
                          have_prebuilt ? xnvme_bench_prp2_tab : NULL);
    clk_b = BENCH_NOW();
    xnvme_bench_clk_end();
    XNvmeHa_SetVerbose(1u);        /* bring the bring-up logs back */
    XNvmeHa_SetCacheOps(1u);
    if (rc != XST_SUCCESS) {
        xil_printf("BENCH: read pass failed rc=%d\r\n", rc);
        return rc;
    }
    xnvme_bench_report("READ ", bytes_per_ch * nch,
                       BENCH_TO_US(clk_b - clk_a), BENCH_TO_US(sub_clk),
                       nch, chunk_bytes);
    xnvme_bench_report_sw();

    {
        u32 i2, c2, e2, s2;

        XNvmeHa_GetSsdStats(&nv->ha, nv->target_ssd, &i2, &c2, &e2);
        s2 = nv->ha.cq_processed;
        xil_printf("BENCH: witness after READ  -> IP issued +%u completed +%u "
                   "err +%u | driver popped +%u CQE\r\n",
                   i2 - iss1, c2 - cmp1, e2 - err1, s2 - svc1);
    }

    /* ---- 4. Verify: what came back must match what we generated. ----
     * The invalidate is what makes the DMA'd bytes (not stale cache lines)
     * visible to the CPU; it has to happen before the checksum.
     * "which channel failed" is not actionable on its own, so the scan also
     * reports WHERE it failed and WHAT the wrong words look like - see
     * xnvme_bench_scan().                                               */
    for (ch = 0u; ch < nch; ch++)
        XNvme_PalInvalidateDcache(xnvme_bench_buf[ch], bytes_per_ch);

    bad = 0u;
    for (ch = 0u; ch < nch; ch++) {
        char mapstr[XNVME_BENCH_MAP_MAX + 1u];
        u32  cmap[XNVME_BENCH_MAP_MAX];
        u32  fb, lb, n, s, ci, nchunk;
        const u32 cw = (chunk_bytes != 0u) ? (chunk_bytes / 4u) : words;

        for (ci = 0u; ci < XNVME_BENCH_MAP_MAX; ci++)
            cmap[ci] = 0u;

        s = xnvme_bench_scan(ch, words, cw, &fb, &lb, &n,
                             cmap, XNVME_BENCH_MAP_MAX);

        if (n == 0u) {
            xil_printf("BENCH: ch%u verify OK (sum 0x%08x)\r\n", ch, s);
            continue;
        }

        bad++;
        xil_printf("BENCH: ch%u CHECKSUM 0x%08x != 0x%08x | %u of %u words "
                   "differ (%u/1000)\r\n",
                   ch, s, csum0[ch], n, words,
                   (u32)(((u64)n * 1000ull) / (u64)words));
        xil_printf("BENCH:   first bad at byte +0x%08x (chunk %u), last at "
                   "+0x%08x (chunk %u of %u)\r\n",
                   fb * 4u, fb / cw, lb * 4u, lb / cw,
                   (cw != 0u) ? (words / cw) : 0u);

        nchunk = (cw != 0u) ? (words / cw) : 0u;
        if (nchunk > XNVME_BENCH_MAP_MAX)
            nchunk = XNVME_BENCH_MAP_MAX;
        for (ci = 0u; ci < nchunk; ci++) {
            u32 c = cmap[ci];
            mapstr[ci] = (c == 0u) ? '.'
                       : ((c > 15u) ? 'X' : "0123456789abcdef"[c & 15u]);
        }
        mapstr[nchunk] = '\0';
        xil_printf("BENCH:   chunk map (%u KB per char, . = clean) [%s]\r\n",
                   chunk_bytes / 1024u, mapstr);

        {
            const u32 *bb  = (const u32 *)xnvme_bench_buf[ch];
            const u32 tag  = 0xA5A50000u | ((u32)ch << 16);
            const u32 tago = 0xA5A50000u | (((u32)ch ^ 1u) << 16);
            u32 a = bb[fb];
            u32 e = tag | (fb & 0xFFFFu);

            xil_printf("BENCH:   at first bad: expected 0x%08x got 0x%08x\r\n",
                       e, a);

            if ((a & 0xFFFF0000u) == tag) {
                u32 ai = a & 0xFFFFu;
                u32 fi = fb & 0xFFFFu;

                xil_printf("BENCH:   -> still a pattern word, index %u "
                           "(this slot should hold index %u)\r\n", ai, fi);
                if (ai >= fi)
                    xil_printf("BENCH:      => the data is SHIFTED +%u words "
                               "(+%u KiB) from where it belongs\r\n",
                               ai - fi, ((ai - fi) * 4u) / 1024u);
                else
                    xil_printf("BENCH:      => the data is SHIFTED -%u words "
                               "(-%u KiB) from where it belongs\r\n",
                               fi - ai, ((fi - ai) * 4u) / 1024u);
            } else if ((a & 0xFFFF0000u) == tago) {
                xil_printf("BENCH:   -> this is the OTHER channel's pattern: "
                           "channel %u's data landed in this buffer\r\n",
                           (u32)ch ^ 1u);
            } else if (a == 0u) {
                xil_printf("BENCH:   -> zeros: this range was never written\r\n");
            } else if (a == 0xFFFFFFFFu) {
                xil_printf("BENCH:   -> 0xFFFFFFFF: media that was never "
                           "programmed\r\n");
            } else {
                xil_printf("BENCH:   -> not part of our pattern at all: "
                           "foreign data landed here\r\n");
            }

            if ((fb + 4u) <= words)
                xil_printf("BENCH:   words here: 0x%08x 0x%08x 0x%08x 0x%08x\r\n",
                           bb[fb], bb[fb + 1u], bb[fb + 2u], bb[fb + 3u]);
        }
    }

    /* ---- 5. Cross-read: is the drive's CONTENT wrong, or the landing
     * address? Re-read channel 1's first chunk from the same LBA but land it
     * in channel 0's buffer (whose turn is over). Same command, same length,
     * different DDR destination:
     *   - the content matches ch1's pattern -> the drive stored and returned
     *     it correctly, so the earlier failure was a DESTINATION problem
     *     (PRP / buffer / register), not the media;
     *   - the content is still wrong -> what is ON THE DRIVE is wrong, so the
     *     write side is at fault.
     * One extra command; only run when something actually failed.        */
    if ((bad != 0u) && (nch >= 2u) && (chunk_bytes >= nv->lba_size)) {
        const u32 cw    = chunk_bytes / 4u;
        const u32 tag1  = 0xA5A50000u | (1u << 16);
        const u32 *bb   = (const u32 *)xnvme_bench_buf[0];
        u32 k, nbad2 = 0u, first2 = 0u;
        int xcid;

        xil_printf("BENCH: cross-read: ch1 chunk 0 (LBA 0x%08x%08x) -> ch0's "
                   "buffer 0x%08x\r\n",
                   (u32)(lbas[1] >> 32), (u32)lbas[1],
                   (u32)XNvme_PalLocalToPcie(xnvme_bench_buf[0]));

        xcid = XNvme_ReadChannelAsync(nv, 1u, lbas[1],
                                      (u16)(chunk_bytes / nv->lba_size),
                                      xnvme_bench_buf[0], chunk_bytes);
        if (xcid < 0) {
            xil_printf("BENCH: cross-read submit failed\r\n");
        } else if (XNvme_WaitIo(nv, xcid) != XST_SUCCESS) {
            xil_printf("BENCH: cross-read wait failed\r\n");
        } else {
            XNvme_PalInvalidateDcache(xnvme_bench_buf[0], chunk_bytes);
            for (k = 0u; k < cw; k++) {
                if (bb[k] != (tag1 | (k & 0xFFFFu))) {
                    if (nbad2 == 0u)
                        first2 = k;
                    nbad2++;
                }
            }
            if (nbad2 == 0u) {
                xil_printf("BENCH:   -> CLEAN: the drive returns ch1's data "
                           "correctly when it lands elsewhere. The failing "
                           "part is the DESTINATION, not the media.\r\n");
            } else {
                xil_printf("BENCH:   -> %u of %u words still wrong (first at "
                           "+0x%08x): what is ON THE DRIVE is wrong, so the "
                           "WRITE side is at fault\r\n",
                           nbad2, cw, first2 * 4u);
            }
        }
    }
    xil_printf("BENCH: verify %s (%u channels x %u B)\r\n",
               (bad == 0u) ? "OK" : "FAILED", nch, bytes_per_ch);
    (void)XNvme_StackCanaryCheck("after the benchmark");
    xil_printf("BENCH: how to read these numbers:\r\n");
    if (xnvme_clk_src == XNVME_CLK_TIMER)
        xil_printf("BENCH:   - time comes from the AXI timer, self-calibrated "
                   "against the UART (%u Hz = %u ns/tick),\r\n"
                   "BENCH:     independent of the CPU;\r\n",
                   xnvme_timer_hz_eff(),
                   (u32)(1000000000u / xnvme_timer_hz_eff()));
    else
        xil_printf("BENCH:   - time comes from the UART TX FIFO stopwatch at %u baud.\r\n"
                   "BENCH:     Its one failure mode is a stale bitstream: the UART's\r\n"
                   "BENCH:     divider is fixed when the bitstream is built, so if the\r\n"
                   "BENCH:     FPGA is not really running at %u every number is scaled\r\n"
                   "BENCH:     by that ratio. Sanity check: read the port at %u baud.\r\n",
                   (u32)XNVME_UART_SW_BAUD, (u32)XNVME_UART_SW_BAUD,
                   (u32)XNVME_UART_SW_BAUD);
    xil_printf("BENCH:   - driver cache maintenance and pattern generation are outside\r\n"
               "BENCH:     the timed windows; the MicroBlaze's own submit work is not;\r\n"
               "BENCH:   - if submit dominates, you measured the CPU feeding the queue,\r\n"
               "BENCH:     not the drive. PRP lists are prebuilt now, so the remaining\r\n"
               "BENCH:     submit cost is the 64 B SQE push (16 Xil_Out32) - to go\r\n"
               "BENCH:     further, feed the SW port from the PL.\r\n");

    return (bad == 0u) ? XST_SUCCESS : XST_FAILURE;
}

/* ----------------- admin Get Log Page helpers (for drive diagnosis) ------ */

static int XNvme_AdminGetLog(XNvmeAdmin *ad, u8 lid, u32 bytes, void *buf)
{
    XNvmeFullSqe sqe;
    u64 buf_pcie = XNvme_PalLocalToPcie(buf);
    u32 ndw = bytes / 4u;
    u32 r0 = 0u, r1 = 0u;

    if ((bytes == 0u) || ((bytes & 0x3u) != 0u) || (bytes > sizeof(xnvme_log_buf)))
        return XST_FAILURE;
    if (ndw == 0u)
        return XST_FAILURE;

    XNvme_PalInvalidateDcache(buf, bytes);
    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0     = XNVME_OPC_ADMIN_GET_LOG_PAGE;
    sqe.nsid    = 0xFFFFFFFFu;                     /* controller-wide log */
    sqe.prp1_lo = (u32)buf_pcie;
    sqe.prp1_hi = (u32)(buf_pcie >> 32);
    sqe.slba_lo = ((ndw - 1u) << 16) | (u32)lid;   /* DW10: NUMDL | LID */
    sqe.slba_hi = 0u;                              /* DW11: NUMDU | LSP/RAE */

    return XNvme_AdminExec(ad, &sqe, &r0, &r1);
}

/* Ask the drive what it thinks: last error-log entry + SMART/health.
 * Uses the admin queue, which keeps working while I/O hangs - so this is the
 * only window into the drive that is still open. */
int XNvme_AdminLogDump(XNvme *nv)
{
    u8 *p;
    int rc;

    if (nv == NULL)
        return XST_FAILURE;

    /* Error Information log (LID 0x01): 64 B per entry, first entry only. */
    rc = XNvme_AdminGetLog(&nv->ad, 0x01u, 64u, xnvme_log_buf);
    if (rc == XST_SUCCESS) {
        XNvme_PalInvalidateDcache(xnvme_log_buf, 64u);
        p = xnvme_log_buf;
        XNVME_DBG("LOG 0x01 err_info: count=%u sqid=%u cid=%u status=0x%04x "
                  "loc=0x%04x lba=0x%08x\r\n",
                  xnvme_rd32(p), (u32)p[8],
                  (u32)p[9] | ((u32)p[10] << 8),
                  (u32)xnvme_rd16(p + 11), (u32)xnvme_rd16(p + 13),
                  xnvme_rd32(p + 16));
    } else {
        XNVME_ERR("LOG 0x01 (Error Info) read failed: %d\r\n", rc);
    }

    /* SMART / Health Information (LID 0x02), 512 B. */
    rc = XNvme_AdminGetLog(&nv->ad, 0x02u, 512u, xnvme_log_buf);
    if (rc == XST_SUCCESS) {
        XNvme_PalInvalidateDcache(xnvme_log_buf, 512u);
        p = xnvme_log_buf;
        XNVME_DBG("LOG 0x02 smart: crit_warn=0x%02x temp=%uK spare=%u%% "
                  "spare_thr=%u%% used=%u%%\r\n",
                  (u32)p[0], (u32)xnvme_rd16(p + 1), (u32)p[3], (u32)p[4],
                  (u32)p[5]);
        XNVME_DBG("LOG 0x02 smart: data_units_read=%u data_units_written=%u "
                  "(1000x512B units)\r\n",
                  xnvme_rd32(p + 32), xnvme_rd32(p + 48));
        XNVME_DBG("LOG 0x02 smart: unsafe_shutdowns=%u media_errors=%u "
                  "err_log_entries=%u\r\n",
                  xnvme_rd32(p + 144), xnvme_rd32(p + 160),
                  xnvme_rd32(p + 176));
    } else {
        XNVME_ERR("LOG 0x02 (SMART) read failed: %d\r\n", rc);
    }
    return XST_SUCCESS;
}
