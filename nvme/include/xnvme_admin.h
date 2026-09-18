/*
 * xnvme_admin.h
 *
 * NVMe admin-queue management. The NVMe Host Accelerator only offloads the
 * I/O queues; the admin queue is managed entirely by software over PCIe.
 * This module performs:
 *   - controller bring-up (CC.EN / CSTS.RDY handshake),
 *   - Identify Controller / Identify Namespace,
 *   - Create I/O Completion/Submission Queues (so the IP's queues exist
 *     on the SSD),
 *   - Set Features (number of queues).
 *
 * It accesses the SSD's NVMe controller registers and doorbells directly
 * through the platform abstraction (xnvme_pal).
 */

#ifndef XNVME_ADMIN_H_
#define XNVME_ADMIN_H_

#include "xil_types.h"
#include "xstatus.h"
#include "xnvmeha_hw.h"
#include "xnvme_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Identify Controller data (4 KiB raw). Field offsets per NVMe spec
 * (verified against the Linux kernel nvme_id_ctrl layout):
 *   SN @4, MN @24, FR @64, MDTS @77, ONCS @520, SGLS @536.            */
typedef struct {
    u8 raw[4096];
} XNvmeIdentifyController;

#define XNVME_IDC_VID(p)     (*(u16 const *)((p)->raw + 0))
#define XNVME_IDC_SSVID(p)   (*(u16 const *)((p)->raw + 2))
#define XNVME_IDC_SN(p)      ((char const *)((p)->raw + 4))
#define XNVME_IDC_MN(p)      ((char const *)((p)->raw + 24))
#define XNVME_IDC_FR(p)      ((char const *)((p)->raw + 64))
#define XNVME_IDC_MDTS(p)    ((p)->raw[77])                 /* 0 = no limit */
#define XNVME_IDC_VER(p)     (*(u32 const *)((p)->raw + 80)) /* NVMe revision */
#define XNVME_IDC_ONCS(p)    (*(u16 const *)((p)->raw + 520))
#define XNVME_IDC_SGLS(p)    (*(u32 const *)((p)->raw + 536)) /* bit0 = SGL ok */

/* Capacity cross-check fields. TNVMCAP/UNVMCAP are 128-bit little-endian
 * byte counts; the low 64 bits cover any realistic drive. A healthy drive
 * reports its true total media size here even if NS1 is formatted small,
 * so TNVMCAP is the decisive way to tell "namespace formatted small" from
 * "controller does not know its own media" (SMI ROM/safe mode).         */
#define XNVME_IDC_TNVMCAP_LO(p) (*(u64 const *)((p)->raw + 280))
#define XNVME_IDC_TNVMCAP_HI(p) (*(u64 const *)((p)->raw + 288))
#define XNVME_IDC_UNVMCAP_LO(p) (*(u64 const *)((p)->raw + 296))
#define XNVME_IDC_NN(p)      (*(u32 const *)((p)->raw + 516)) /* # namespaces */
#define XNVME_IDC_FNA(p)     ((p)->raw[524])                  /* Format attrs */

/* Identify Namespace data (4 KiB raw). Key offsets:
 *   NSZE @0, NLBAF @25, FLBAS @26, LBAF[64] @128 (4B each, DS at byte 2). */
typedef struct {
    u8 raw[4096];
} XNvmeIdentifyNamespace;

#define XNVME_IDN_NSZE(p)    (*(u64 const *)((p)->raw + 0))
#define XNVME_IDN_NCAP(p)    (*(u64 const *)((p)->raw + 8))
#define XNVME_IDN_NUSE(p)    (*(u64 const *)((p)->raw + 16))
#define XNVME_IDN_NSFEAT(p)  ((p)->raw[24])
#define XNVME_IDN_NLBAF(p)   ((p)->raw[25])   /* # of LBA formats, 0-based */
#define XNVME_IDN_FLBAS(p)   ((p)->raw[26])
#define XNVME_IDN_NVMCAP_LO(p) (*(u64 const *)((p)->raw + 48)) /* NS bytes */
#define XNVME_IDN_LBAF_DS(p,i) ((p)->raw[128 + (u32)(i) * 4u + 2u]) /* 2^ds bytes */
#define XNVME_IDN_LBAF_MS(p,i) (*(u16 const *)((p)->raw + 128 + (u32)(i) * 4u))
#define XNVME_IDN_RAW(p)     ((u8 const *)((p)->raw))

/* Admin-queue state. One instance per SSD (init is sequential, so a
 * single static buffer set is reused).                                 */
typedef struct {
    u64 ctrl_base;     /* PCIe base of the NVMe controller (SSD BAR0) */
    u32 dstrd;         /* doorbell stride from CAP                    */
    u32 mps;           /* chosen MPS value (= CAP.MPSMIN)            */
    u32 page_size;     /* memory page size = 2^(12+mps) bytes        */
    u32 sq_tail;       /* next admin SQ slot to write                 */
    u32 cq_head;       /* next admin CQ slot to read                  */
    u32 phase;         /* expected phase tag of new CQEs              */
    u16 next_cid;      /* next admin command id                       */
} XNvmeAdmin;

/* ----------------- lifecycle ----------------- */

/* Initialise the admin queue: read CAP, configure AQA/ASQ/ACQ, enable
 * the controller and wait for CSTS.RDY. Returns XST_SUCCESS on ready. */
int XNvme_AdminInit(XNvmeAdmin *ad, u64 ctrl_base);

/* Disable the controller (CC.EN=0, wait CSTS.RDY=0). */
int XNvme_AdminShutdown(XNvmeAdmin *ad);

/* ----------------- generic admin command ----------------- */

/* Execute one admin command synchronously. `sqe` is a full 64-byte NVMe
 * SQE with opcode/NSID/DW10..15 already filled (PRP/CID are set here).
 * Returns 0 on success, or a negative value on timeout; on success the
 * CQE result DW0/DW1 are returned via result0 and result1.            */
int XNvme_AdminExec(XNvmeAdmin *ad, XNvmeFullSqe *sqe,
                    u32 *result0, u32 *result1);

/* ----------------- high-level admin commands ----------------- */

int XNvme_AdminIdentifyController(XNvmeAdmin *ad, XNvmeIdentifyController *out);
int XNvme_AdminIdentifyNamespace(XNvmeAdmin *ad, u32 nsid,
                                 XNvmeIdentifyNamespace *out);

/* Create one I/O CQ + I/O SQ pair (NVMe qid = qid) at the given PCIe
 * addresses, with the given depth. PC=1 (contiguous), interrupts
 * disabled (the IP manages completion notification itself).            */
int XNvme_AdminCreateIoQueuePair(XNvmeAdmin *ad, u16 qid, u32 depth,
                                 u64 sq_pcie_addr, u64 cq_pcie_addr);

/* Set Features: Number of Queues. Returns the granted NCQ/NSQ in the
 * out pointers (may be smaller than requested). Optional but recommended
 * before creating I/O queues.                                          */
int XNvme_AdminSetNumQueues(XNvmeAdmin *ad, u32 req_sq, u32 req_cq,
                            u32 *granted_sq, u32 *granted_cq);

#ifdef __cplusplus
}
#endif

#endif /* XNVME_ADMIN_H_ */
