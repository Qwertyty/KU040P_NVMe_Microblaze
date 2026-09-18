/*
 * xnvme_admin.c
 *
 * NVMe admin-queue implementation. The admin queue lives in host DDR and
 * is accessed by the SSD over PCIe, so its memory must be PCIe-reachable
 * (mapped through the PCIe Root Port outbound window). The doorbells and
 * controller registers are accessed through xnvme_pal.
 */

#include "xnvme_admin.h"
#include "xnvmeha_config.h"
#include "xil_io.h"
#include "xil_mem.h"
#include "xil_cache.h"
#include "xil_printf.h"

#if (XNVMEHA_DEBUG >= 2)
#define XNVME_ADBG(...)  xil_printf(__VA_ARGS__)
#else
#define XNVME_ADBG(...)  do {} while (0)
#endif
#if (XNVMEHA_DEBUG >= 1)
#define XNVME_AERR(...)  xil_printf(__VA_ARGS__)
#else
#define XNVME_AERR(...)  do {} while (0)
#endif

/* Admin queue depths (must be <= MQES+1 from CAP; 16 is universally OK). */
#define ADMIN_Q_DEPTH   XNVMEHA_ADMIN_SQ_DEPTH
#define ADMIN_SQ_BYTES  (ADMIN_Q_DEPTH * 64u)   /* 64-byte SQE  */
#define ADMIN_CQ_BYTES  (ADMIN_Q_DEPTH * 16u)   /* 16-byte CQE  */

/* Admin queue memory. NVMe requires ASQ/ACQ to be aligned to the memory
 * page size (2^(12+MPS)). MPS is chosen = CAP.MPSMIN at runtime, so align
 * to an upper bound covering MPSMIN up to 1 (8 KiB pages). If your
 * controller reports MPSMIN >= 2, raise XNVMEHA_MAX_PAGE_ALIGN (the
 * MicroBlaze ELF supports larger alignments than this host build cap). */
#define XNVMEHA_MAX_PAGE_ALIGN  8192u

static XNvmeFullSqe   admin_sq[ADMIN_Q_DEPTH] __attribute__((aligned(XNVMEHA_MAX_PAGE_ALIGN)));
static XNvmeCqeDw     admin_cq[ADMIN_Q_DEPTH] __attribute__((aligned(XNVMEHA_MAX_PAGE_ALIGN)));

/* Scratch buffer for Identify (4 KiB), DMA'd by the SSD. */
static u8              admin_identify_buf[4096] __attribute__((aligned(XNVMEHA_MAX_PAGE_ALIGN)));

/* ----------------- helpers ----------------- */

static inline u32 ARegRd(XNvmeAdmin *ad, u32 off)
{
    return XNvme_PalPcieRead32(ad->ctrl_base + off);
}

static inline void ARegWr(XNvmeAdmin *ad, u32 off, u32 val)
{
    XNvme_PalPcieWrite32(ad->ctrl_base + off, val);
}

static void ARingSqDb(XNvmeAdmin *ad)
{
    XNvme_PalPcieWrite32(ad->ctrl_base + XNVME_DB_SQ(0, ad->dstrd),
                         ad->sq_tail);
}

static void ARingCqDb(XNvmeAdmin *ad)
{
    XNvme_PalPcieWrite32(ad->ctrl_base + XNVME_DB_CQ(0, ad->dstrd),
                         ad->cq_head);
}

/* ----------------- generic admin command ----------------- */

int XNvme_AdminExec(XNvmeAdmin *ad, XNvmeFullSqe *sqe,
                    u32 *result0, u32 *result1)
{
    u32 timeout;
    u32 head;
    u32 phase;
    u32 dw3;
    XNvmeCqeDw *cqe;

    if ((ad == NULL) || (sqe == NULL))
        return XST_FAILURE;

    /* Place the command in the admin SQ, set CID, then ring the doorbell. */
    sqe->dw0 = (sqe->dw0 & 0x0000FFFFu) | ((u32)ad->next_cid << 16);
    ad->next_cid++;

    /* Ensure the SSD sees the SQE (flush before doorbell). */
    XNvme_PalFlushDcache(&admin_sq[ad->sq_tail], sizeof(XNvmeFullSqe));
    admin_sq[ad->sq_tail] = *sqe;
    XNvme_PalFlushDcache(&admin_sq[ad->sq_tail], sizeof(XNvmeFullSqe));

    ad->sq_tail = (ad->sq_tail + 1u) % ADMIN_Q_DEPTH;
    XNvme_PalDsb();
    ARingSqDb(ad);

    /* Poll the admin CQ for a new entry with the expected phase tag. */
    head   = ad->cq_head;
    phase  = ad->phase;
    timeout = XNVMEHA_SYNC_TIMEOUT;
    for (;;) {
        XNvme_PalInvalidateDcache(&admin_cq[head], sizeof(XNvmeCqeDw));
        cqe = &admin_cq[head];
        dw3 = cqe->dw3;
        if (XNVMEHA_CQE_PHASE(dw3) == (u32)phase)
            break;
        if (--timeout == 0u) {
            XNVME_AERR("ADM: timeout cid=%u opc=0x%x\r\n",
                       (u16)(sqe->dw0 >> 16), sqe->dw0 & 0xFFu);
            return XST_TIMEOUT;
        }
    }

    /* Consume the CQE and ring the CQ head doorbell. */
    if (result0) *result0 = cqe->dw0;
    if (result1) *result1 = cqe->dw1;

    head = (head + 1u) % ADMIN_Q_DEPTH;
    ad->cq_head = head;
    if (head == 0u)
        ad->phase ^= 1u;        /* phase toggles on CQ wrap */
    XNvme_PalDsb();
    ARingCqDb(ad);

    {
        u32 sc  = XNVMEHA_CQE_SC(dw3);
        u32 sct = XNVMEHA_CQE_SCT(dw3);
        if (sc != 0u || sct != 0u) {
            XNVME_AERR("ADM: cmd fail opc=0x%x sct=%u sc=%u dnr=%u cid=%u\r\n"
                       "     CQE dw0=0x%08x dw2=0x%08x dw3=0x%08x prp1=0x%08x%08x\r\n",
                       sqe->dw0 & 0xFFu, sct, sc, XNVMEHA_CQE_DNR(dw3),
                       XNVMEHA_CQE_CID(dw3),
                       cqe->dw0, cqe->dw2, dw3,
                       sqe->prp1_hi, sqe->prp1_lo);
            return XST_FAILURE;
        }
    }
    XNVME_ADBG("ADM: ok opc=0x%x res0=0x%x\r\n", sqe->dw0 & 0xFFu,
               result0 ? *result0 : 0u);
    return XST_SUCCESS;
}

/* ----------------- controller bring-up ----------------- */

int XNvme_AdminInit(XNvmeAdmin *ad, u64 ctrl_base)
{
    u64 cap;
    u32 timeout;
    u32 to_cycles;
    u32 aqa, cc;
    u64 sq_pcie, cq_pcie;

    if (ad == NULL)
        return XST_FAILURE;

    /* 1. ctrl_base为ssd bar0的基地址 */
    ad->ctrl_base = ctrl_base;
    ad->sq_tail   = 0;
    ad->cq_head   = 0;
    ad->phase     = 1;        /* NVMe starts CQ phase at 1 */
    ad->next_cid  = 0;

    /* 2. 读取cap */
    /* Read CAP (64-bit) for doorbell stride + timeout + memory page size.
     * CC.MPS must be >= CAP.MPSMIN (and <= MPSMAX). We pick MPSMIN, the
     * smallest supported page, so the controller always accepts it and so
     * MDTS (reported in MPSMIN-page units) maps directly to page_size.   */
    cap = XNvme_PalPcieRead64(ctrl_base + XNVME_CTRL_CAP);
    ad->dstrd     = XNVME_CAP_DSTRD(cap);   // Doorbell 步长
    ad->mps       = XNVME_CAP_MPSMIN(cap);  // CC.MPS 必须大于或等于 CAP.MPSMIN（且小于或等于 MPSMAX）。选择 MPSMIN，
    ad->page_size = XNVME_PAGE_SIZE(ad->mps);

    /* 3. 写 CC=0，再循环等待 CSTS.RDY==0 */
    ARegWr(ad, XNVME_CTRL_CC, 0u);
    to_cycles = (XNVME_CAP_TO(cap) + 1u) * 2000000u; /* coarse loop count */
    timeout = to_cycles;
    while ((ARegRd(ad, XNVME_CTRL_CSTS) & XNVME_CSTS_RDY) != 0u) {
        if (--timeout == 0u) {
            XNVME_AERR("ADM: CSTS.RDY did not clear\r\n");
            break;
        }
    }

    /* 4. 设置 AQA的 PCIe 地址，深度取 ADMIN_Q_DEPTH。 */
    aqa = (((u32)ADMIN_Q_DEPTH - 1u) << 16) | ((u32)ADMIN_Q_DEPTH - 1u);
    ARegWr(ad, XNVME_CTRL_AQA, aqa);

    /* 5. 设置 ASQ/ACQ PCIe 地址  */
    sq_pcie = XNvme_PalAdminMemToPcie(admin_sq);
    cq_pcie = XNvme_PalAdminMemToPcie(admin_cq);
    XNvme_PalPcieWrite64(ctrl_base + XNVME_CTRL_ASQ, sq_pcie);
    XNvme_PalPcieWrite64(ctrl_base + XNVME_CTRL_ACQ, cq_pcie);
    XNVME_ADBG("ADM: ASQ=0x%08x%08x ACQ=0x%08x%08x dstrd=%u mps=%u page=%u\r\n",
               (u32)(sq_pcie >> 32), (u32)sq_pcie,
               (u32)(cq_pcie >> 32), (u32)cq_pcie, ad->dstrd,
               ad->mps, ad->page_size);
    /* Controller limits. MQES is the largest legal queue size (0-based), so
     * it bounds what Create I/O SQ/CQ may legally ask for; TO/DSTRD/MPS*
     * complete the geometry picture. */
    XNVME_ADBG("ADM: CAP=0x%08x%08x MQES=%u TO=%u MPSMIN=%u MPSMAX=%u\r\n",
               (u32)(cap >> 32), (u32)cap,
               (u32)XNVME_CAP_MQES(cap) + 1u, (u32)XNVME_CAP_TO(cap),
               (u32)XNVME_CAP_MPSMIN(cap), (u32)XNVME_CAP_MPSMAX(cap));

    /* 6.配置 CC.AMS , CC.MPS , CC.CSS , 设置 CC.EN=1 */
    cc = XNVME_CC_EN
       | (0u << XNVME_CC_CSS_SHIFT)
       | (ad->mps << XNVME_CC_MPS_SHIFT)
       | (0u << XNVME_CC_AMS_SHIFT)
       | (6u << XNVME_CC_IOSQES_SHIFT)
       | (4u << XNVME_CC_IOCQES_SHIFT);
    ARegWr(ad, XNVME_CTRL_CC, cc);

    /* 等待 CSTS.RDY=1 */
    timeout = to_cycles;
    while ((ARegRd(ad, XNVME_CTRL_CSTS) & XNVME_CSTS_RDY) == 0u) {
        if (--timeout == 0u) {
            XNVME_AERR("ADM: CSTS.RDY did not set; CSTS=0x%x\r\n",
                       ARegRd(ad, XNVME_CTRL_CSTS));
            return XST_TIMEOUT;
        }
    }

    XNVME_ADBG("ADM: controller ready\r\n");
    return XST_SUCCESS;
}

int XNvme_AdminShutdown(XNvmeAdmin *ad)
{
    u32 timeout = XNVMEHA_SYNC_TIMEOUT;
    ARegWr(ad, XNVME_CTRL_CC,
           ARegRd(ad, XNVME_CTRL_CC) & ~XNVME_CC_EN);
    while ((ARegRd(ad, XNVME_CTRL_CSTS) & XNVME_CSTS_RDY) != 0u) {
        if (--timeout == 0u)
            return XST_TIMEOUT;
    }
    return XST_SUCCESS;
}

/* ----------------- Identify ----------------- */

int XNvme_AdminIdentifyController(XNvmeAdmin *ad, XNvmeIdentifyController *out)
{
    XNvmeFullSqe sqe;
    u64 buf_pcie;
    int rc;
    u32 r0, r1;

    XNvme_PalInvalidateDcache(admin_identify_buf, sizeof(admin_identify_buf));
    buf_pcie = XNvme_PalLocalToPcie(admin_identify_buf);

    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0    = XNVME_OPC_ADMIN_IDENTIFY;
    sqe.nsid   = 0;
    sqe.prp1_lo = (u32)buf_pcie;
    sqe.prp1_hi = (u32)(buf_pcie >> 32);
    sqe.slba_lo = 0x00000001u;   /* DW10: CNS = 1 (Identify Controller) */

    rc = XNvme_AdminExec(ad, &sqe, &r0, &r1);
    if (rc != XST_SUCCESS)
        return rc;

    XNvme_PalInvalidateDcache(admin_identify_buf, sizeof(admin_identify_buf));
    if (out)
        Xil_MemCpy(out, admin_identify_buf, sizeof(*out));
    return XST_SUCCESS;
}

int XNvme_AdminIdentifyNamespace(XNvmeAdmin *ad, u32 nsid,
                                 XNvmeIdentifyNamespace *out)
{
    XNvmeFullSqe sqe;
    u64 buf_pcie;
    int rc;
    u32 r0, r1;

    XNvme_PalInvalidateDcache(admin_identify_buf, sizeof(admin_identify_buf));
    buf_pcie = XNvme_PalLocalToPcie(admin_identify_buf);

    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0    = XNVME_OPC_ADMIN_IDENTIFY;
    sqe.nsid   = nsid;
    sqe.prp1_lo = (u32)buf_pcie;
    sqe.prp1_hi = (u32)(buf_pcie >> 32);
    sqe.slba_lo = 0x00000000u;   /* DW10: CNS = 0 (Identify Namespace) */

    rc = XNvme_AdminExec(ad, &sqe, &r0, &r1);
    if (rc != XST_SUCCESS)
        return rc;

    XNvme_PalInvalidateDcache(admin_identify_buf, sizeof(admin_identify_buf));
    if (out)
        Xil_MemCpy(out, admin_identify_buf, sizeof(*out));
    return XST_SUCCESS;
}

/* ----------------- Create I/O Queues ----------------- */

int XNvme_AdminCreateIoQueuePair(XNvmeAdmin *ad, u16 qid, u32 depth,
                                 u64 sq_pcie_addr, u64 cq_pcie_addr)
{
    XNvmeFullSqe sqe;
    u32 r0, r1;
    int rc;

    /* Create I/O Completion Queue (opcode 0x05).
     *   DW10: CQID[15:0] | (QSIZE[31:16])   QSIZE = depth-1
     *   DW11: PC=1 (contiguous), IEN=0 (no NVMe interrupt; IP manages it),
     *         IV=0
     *   PRP1: CQ PCIe address                                            */
    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0     = XNVME_OPC_ADMIN_CREATE_CQ;
    sqe.prp1_lo = (u32)cq_pcie_addr;
    sqe.prp1_hi = (u32)(cq_pcie_addr >> 32);
    sqe.slba_lo = ((u32)(depth - 1u) << 16) | (u32)qid;   /* DW10 */
    sqe.slba_hi = 0x1u;                                     /* DW11: PC=1 */
    rc = XNvme_AdminExec(ad, &sqe, &r0, &r1);
    if (rc != XST_SUCCESS) {
        XNVME_AERR("ADM: Create CQ%u failed\r\n", qid);
        return rc;
    }

    /* Create I/O Submission Queue (opcode 0x01).
     *   DW10: SQID[15:0] | (QSIZE[31:16])
     *   DW11: PC=1 | (CQID[15:0] << 16)
     *   PRP1: SQ PCIe address                                            */
    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0     = XNVME_OPC_ADMIN_CREATE_SQ;
    sqe.prp1_lo = (u32)sq_pcie_addr;
    sqe.prp1_hi = (u32)(sq_pcie_addr >> 32);
    sqe.slba_lo = ((u32)(depth - 1u) << 16) | (u32)qid;   /* DW10 */
    sqe.slba_hi = 0x1u | ((u32)qid << 16);                 /* DW11: PC=1, CQID */
    rc = XNvme_AdminExec(ad, &sqe, &r0, &r1);
    if (rc != XST_SUCCESS) {
        XNVME_AERR("ADM: Create SQ%u failed\r\n", qid);
        return rc;
    }

    XNVME_ADBG("ADM: created I/O Q pair qid=%u depth=%u\r\n", qid, depth);
    return XST_SUCCESS;
}

/* ----------------- Set Features: Number of Queues ----------------- */

int XNvme_AdminSetNumQueues(XNvmeAdmin *ad, u32 req_sq, u32 req_cq,
                            u32 *granted_sq, u32 *granted_cq)
{
    XNvmeFullSqe sqe;
    u32 r0, r1;
    int rc;

    memset(&sqe, 0, sizeof(sqe));
    sqe.dw0     = XNVME_OPC_ADMIN_SET_FEATURES;
    sqe.slba_lo = 0x07u;                 /* DW10: feature id = Num Queues */
    /* DW11: NSQ[31:16] | NCQ[15:0] (0-based values) */
    sqe.slba_hi = (((req_sq - 1u) & 0xFFFFu) << 16) | ((req_cq - 1u) & 0xFFFFu);

    rc = XNvme_AdminExec(ad, &sqe, &r0, &r1);
    if (rc != XST_SUCCESS)
        return rc;

    /* Granted counts are in CQE DW0 (0-based). */
    if (granted_cq) *granted_cq = (r0 & 0xFFFFu) + 1u;
    if (granted_sq) *granted_sq = ((r0 >> 16) & 0xFFFFu) + 1u;
    return XST_SUCCESS;
}
