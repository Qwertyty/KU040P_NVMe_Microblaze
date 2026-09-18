/*
 * xnvmeha_hw.h
 *
 * Hardware constants for the Xilinx NVMe Host Accelerator (PG328):
 *  - AXI4-Lite register map
 *  - SW_AXI4 memory-mapped queue map
 *  - SQE / CQE on-the-wire formats
 *
 * All offsets below are taken verbatim from PG328 (v1.0) Chapter 3.
 */

#ifndef XNVMEHA_HW_H_
#define XNVMEHA_HW_H_

#include "xil_types.h"
#include "xnvmeha_config.h"

/* =========================================================================
 *  AXI4-Lite register offsets  (PG328 Table 9)
 *  All offsets are relative to XNVMEHA_REG_BASE.
 * ========================================================================= */

/* SSDn PCIe base address lower/upper : 0x0000 + n*8 / 0x0004 + n*8 */
#define XNVMEHA_REG_PCIE_BASE_LO(n)    (0x0000u + (u32)(n) * 8u)
#define XNVMEHA_REG_PCIE_BASE_HI(n)    (0x0004u + (u32)(n) * 8u)

/* Per (SSDn, SQm) 16-byte register block:
 *   base = 0x100 + n*(MAX_SQ*16) + m*16
 *   +0x0  SQm base address     (only n=0,m=0 is RW, rest RO/auto)
 *   +0x4  CQm base address     (only n=0,m=0 is RW, rest RO/auto)
 *   +0x8  SQm flush enable     (RW, bit0)
 *   +0xC  SQm flush done       (RO, bit0)
 */
#define XNVMEHA_QBLOCK_STRIDE          (16u * (u32)XNVMEHA_MAX_SQ)
#define XNVMEHA_QBLOCK(n, m)           (0x100u + (u32)(n) * XNVMEHA_QBLOCK_STRIDE \
                                              + (u32)(m) * 16u)
#define XNVMEHA_REG_SQ_BASE(n,m)       (XNVMEHA_QBLOCK(n,m) + 0x0u)
#define XNVMEHA_REG_CQ_BASE(n,m)       (XNVMEHA_QBLOCK(n,m) + 0x4u)
#define XNVMEHA_REG_SQ_FLUSH(n,m)      (XNVMEHA_QBLOCK(n,m) + 0x8u)
#define XNVMEHA_REG_SQ_FLUSH_DONE(n,m) (XNVMEHA_QBLOCK(n,m) + 0xCu)

#define XNVMEHA_REG_INT_ENABLE         0x8104u
#define XNVMEHA_REG_INT_STATUS         0x8108u
#define XNVMEHA_REG_CQ_AVAIL_COUNT     0x810Cu
#define XNVMEHA_REG_CMD_ERR_COUNT      0x8110u
#define XNVMEHA_REG_TIMEOUT            0x8114u

#define XNVMEHA_REG_SSD_ISSUED(n)      (0x8200u + (u32)(n) * 16u)
#define XNVMEHA_REG_SSD_ERRCOMP(n)     (0x8204u + (u32)(n) * 16u)
#define XNVMEHA_REG_SSD_COMPLETED(n)   (0x8208u + (u32)(n) * 16u)

#define XNVMEHA_REG_SQCQ_STATUS(n,m)   (0x8400u + (u32)(n) * (4u*(u32)XNVMEHA_MAX_SQ) \
                                              + (u32)(m) * 4u)

/* Interrupt enable / status bit definitions (PG328 Tables 18/19) */
#define XNVMEHA_INT_CQ_AVAIL           (1u << 0)  /* CQ available              */
#define XNVMEHA_INT_ADDR_ERR           (1u << 1)  /* SW AXI / HW AXIS bad addr */
#define XNVMEHA_INT_CREDIT_ERR         (1u << 2)  /* SW SQE without credit     */
#define XNVMEHA_INT_RD_NO_CQ           (1u << 3)  /* SW CQ read when none avail*/
#define XNVMEHA_INT_HW_NO_CREDIT       (1u << 4)  /* HW SQE without credit     */
#define XNVMEHA_INT_SSD_TIMEOUT        (1u << 5)  /* SSD response timeout      */
#define XNVMEHA_INT_ALL_ERRORS         (0x3Eu)
#define XNVMEHA_INT_ALL                (0x3Fu)

/* SQ/CQ FIFO status register bits (PG328 Table 26) */
#define XNVMEHA_STAT_SQ_FULL           (1u << 0)
#define XNVMEHA_STAT_SQ_EMPTY          (1u << 1)
#define XNVMEHA_STAT_CQ_FULL           (1u << 2)
#define XNVMEHA_STAT_CQ_EMPTY          (1u << 3)
#define XNVMEHA_STAT_SQ_AVAIL(val)     (((val) >> 4) & 0x1FFu)
#define XNVMEHA_STAT_TIMEOUT           (1u << 13)


/* =========================================================================
 *  SW_AXI4 memory-mapped queue map  (PG328 Table 1)
 *  Offsets are relative to XNVMEHA_SW_AXI_BASE.
 *  - Write a (partial 32B or full 64B) SQE to push it into SSDn/SQm.
 *  - Read 32 bytes from the CQ location to pop one completion.
 * ========================================================================= */

#define XNVMEHA_SW_SSD_STRIDE          0x1000u   /* 64 SQs * 0x40 per SSD   */
#define XNVMEHA_SW_SQ_STRIDE           0x40u     /* 64-byte slot             */
#define XNVMEHA_SW_SQ_ADDR(n, m)       ((u32)(n) * XNVMEHA_SW_SSD_STRIDE \
                                       + (u32)(m) * XNVMEHA_SW_SQ_STRIDE)
#define XNVMEHA_SW_CQ_ADDR             0x30000u  /* single CQ pop location   */

/* Size of one SQE / CQE in bytes, depending on the data-width config. */
#if (XNVMEHA_SW_FULL_CMD == 1)
#define XNVMEHA_SQE_BYTES              64u
#else
#define XNVMEHA_SQE_BYTES              32u
#endif

#define XNVMEHA_CQE_BYTES              32u       /* CQE + appended header    */

/* Number of 32-bit words per SQE/CQE (used by the push/pop loops; the
 * 64-bit data-width path packs pairs of words into 64-bit beats).       */
#define XNVMEHA_SQE_WORDS              (XNVMEHA_SQE_BYTES / 4u)
#define XNVMEHA_CQE_WORDS              (XNVMEHA_CQE_BYTES / 4u)

#if (XNVMEHA_AXI_MEM_DWIDTH != 32) && (XNVMEHA_AXI_MEM_DWIDTH != 64)
#error "XNVMEHA_AXI_MEM_DWIDTH must be 32 or 64"
#endif


/* =========================================================================
 *  On-the-wire command / completion formats
 * ========================================================================= */

/*
 * Partial (32-byte) Submission Queue Entry (PG328 Figure 3).
 * The IP fills the unused fields with 0 and builds a full NVMe command.
 *
 *   DW0 : [31:16] Command ID | [15:10] reserved | [9:8] FUSE | [7:0] Opcode
 *   DW1 : Namespace Identifier
 *   DW2 : SGL Entry Address [31:0]
 *   DW3 : SGL Entry Address [63:32]
 *   DW4 : SGL Entry Length (bytes)
 *   DW5 : [31:24] SGL ident (type/subtype) | [23:16] reserved | [15:0] NLB-1
 *   DW6 : Starting LBA [63:32]
 *   DW7 : Starting LBA [31:0]
 *
 * NVMe SGL Data-Block descriptor identifier byte = 0x01
 * (subtype=0, type=1).  Placed in the most-significant byte of DW5.
 */
typedef struct {
    u32 dw0;        /* opcode / fuse / CID                 */
    u32 nsid;       /* DW1 namespace id                    */
    u32 sgl_addr_lo;/* DW2 SGL address low                 */
    u32 sgl_addr_hi;/* DW3 SGL address high                */
    u32 sgl_len;    /* DW4 SGL length in bytes             */
    u32 sgl_id_nlb; /* DW5 {SGL ident[31:24], NLB-1[15:0]} */
    u32 slba_hi;    /* DW6 starting LBA high               */
    u32 slba_lo;    /* DW7 starting LBA low                */
} XNvmePartialSqe;

#define XNVMEHA_SGL_DATA_BLOCK         0x01000000u  /* ident byte 0x01 << 24 */

/* Full (64-byte) NVMe Submission Queue Entry (PG328 Figure 2). */
typedef struct {
    u32 dw0;        /* opcode / fuse / PSDT / CID          */
    u32 nsid;       /* DW1 namespace id                    */
    u32 reserved2;  /* DW2                                 */
    u32 reserved3;  /* DW3                                 */
    u32 mptr_lo;    /* DW4 metadata pointer low            */
    u32 mptr_hi;    /* DW5 metadata pointer high           */
    u32 prp1_lo;    /* DW6 PRP1 / SGL addr low             */
    u32 prp1_hi;    /* DW7 PRP1 / SGL addr high            */
    u32 prp2_lo;    /* DW8 PRP2 / SGL len                  */
    u32 prp2_hi;    /* DW9 PRP2 / SGL ident                */
    u32 slba_lo;    /* DW10 starting LBA low               */
    u32 slba_hi;    /* DW11 starting LBA high              */
    u32 nlb;        /* DW12 NLB-1[15:0] + control bits     */
    u32 dsm;        /* DW13                                */
    u32 eilbrt;     /* DW14                                */
    u32 elbat;      /* DW15                                */
} XNvmeFullSqe;

/*
 * Completion entry as seen on the SW_AXI4 read interface (32 bytes total).
 *
 * 64-bit data width (4 beats, PG328 Figure 5):
 *   [0]  CQE[63:0]
 *   [1]  CQE[127:64]
 *   [2]  {48'h0, Credit[15:5], SSD_Number[4:0]}
 *   [3]  reserved
 *
 * 32-bit data width (8 beats, PG328 Figure 4):
 *   [0..3]  16-byte NVMe CQE (DW0..DW3)
 *   [4]     {16'h0, Credit[15:5], SSD_Number[4:0]}     <- header
 *   [5..7]  zero
 *
 * The 32-bit layout and the header position were VERIFIED on hardware on
 * 17 Sep 2026 - a real pop reads back as e.g.
 *   00000000 00000000 00010001 00010000 | 00000020 00000000 00000000 00000000
 *   (qid 1, CID 0, phase 1, credit 1, SSD 0).
 * Two traps, both hit during bring-up:
 *   - the whole record must be read by WALKING the address
 *     (addr, addr+4, ... addr+28). Reading one address eight times returns
 *     eight zeros while the IP still reports a CQE as available and does
 *     NOT set RD_NO_CQ - see XNVMEHA_POP_ADDR_INC in xnvmeha.c.
 *   - the header is at word index 4. Reading it from 6 yields 0, so the
 *     software credit pool is never replenished.
 *
 * Credit / SSD_Number encoding matches the streaming TUSER format
 * (PG328 Figure 7):  SSD_Number = bits[4:0], Credit = bits[15:5].
 */
#define XNVMEHA_CQE_SSD_MASK           0x001Fu
#define XNVMEHA_CQE_SSD_SHIFT          0
#define XNVMEHA_CQE_CREDIT_MASK        0xFFE0u
#define XNVMEHA_CQE_CREDIT_SHIFT       5

#if (XNVMEHA_AXI_MEM_DWIDTH == 64)
#define XNVMEHA_CQE_HDR_INDEX          2u        /* 64-bit beat index */
#else
#define XNVMEHA_CQE_HDR_INDEX          4u        /* 32-bit word index */
#endif

/* 16-byte NVMe completion entry (DW0..DW3). */
typedef struct {
    u32 dw0;        /* result[31:0]                        */
    u32 dw1;        /* result[63:32]                       */
    u32 dw2;        /* [31:16] SQ ID  | [15:0] SQ HD       */
    u32 dw3;        /* [15:0] CID | [16] phase | [28:17] status */
} XNvmeCqeDw;

/* DW3 field helpers */
#define XNVMEHA_CQE_CID(dw3)           ((u16)((dw3) & 0xFFFFu))
#define XNVMEHA_CQE_PHASE(dw3)         (((dw3) >> 16) & 0x1u)
#define XNVMEHA_CQE_SC(dw3)            (((dw3) >> 17) & 0xFFu)
#define XNVMEHA_CQE_SCT(dw3)           (((dw3) >> 25) & 0x7u)
#define XNVMEHA_CQE_DNR(dw3)           (((dw3) >> 28) & 0x1u)
/* DW2 field helpers */
#define XNVMEHA_CQE_SQID(dw2)          ((u16)(((dw2) >> 16) & 0xFFFFu))
#define XNVMEHA_CQE_SQHD(dw2)          ((u16)((dw2) & 0xFFFFu))


/* =========================================================================
 *  NVMe opcodes (subset)
 * ========================================================================= */
#define XNVME_OPC_ADMIN_DELETE_SQ      0x00u
#define XNVME_OPC_ADMIN_CREATE_SQ      0x01u
#define XNVME_OPC_ADMIN_GET_LOG_PAGE   0x02u
#define XNVME_OPC_ADMIN_DELETE_CQ      0x04u
#define XNVME_OPC_ADMIN_CREATE_CQ      0x05u
#define XNVME_OPC_ADMIN_IDENTIFY       0x06u
#define XNVME_OPC_ADMIN_ABORT          0x08u
#define XNVME_OPC_ADMIN_SET_FEATURES   0x09u
#define XNVME_OPC_ADMIN_GET_FEATURES   0x0Au
#define XNVME_OPC_ADMIN_ASYNC_EVENT    0x0Cu
#define XNVME_OPC_ADMIN_NS_MGMT        0x0Du
#define XNVME_OPC_ADMIN_FW_ACTIVATE    0x10u
#define XNVME_OPC_ADMIN_NS_ATTACH      0x15u

#define XNVME_OPC_IO_FLUSH             0x00u
#define XNVME_OPC_IO_WRITE             0x01u
#define XNVME_OPC_IO_READ              0x02u
#define XNVME_OPC_IO_WRITE_UNCORR      0x04u
#define XNVME_OPC_IO_COMPARE           0x05u
#define XNVME_OPC_IO_WRITE_ZEROES      0x08u
#define XNVME_OPC_IO_DATASET_MGMT      0x09u

/* NVMe controller register offsets (at the SSD PCIe base / BAR0). */
#define XNVME_CTRL_CAP                0x00u   /* 64-bit RO */
#define XNVME_CTRL_VS                 0x08u
#define XNVME_CTRL_INTMS              0x0Cu
#define XNVME_CTRL_INTMC              0x10u
#define XNVME_CTRL_CC                 0x14u
#define XNVME_CTRL_CSTS               0x1Cu
#define XNVME_CTRL_AQA                0x24u
#define XNVME_CTRL_ASQ                0x28u   /* 64-bit RW */
#define XNVME_CTRL_ACQ                0x30u   /* 64-bit RW */

/* Doorbell registers (at the SSD PCIe base / BAR0).
 *
 * NVMe Base Spec layout: SQyTDBL = 0x1000 + (2y    * (4 << DSTRD))
 *                         CQyHDBL = 0x1000 + ((2y+1) * (4 << DSTRD))
 * i.e. each queue owns 8 bytes - one SQ tail and one CQ head register.
 * With DSTRD = 0:
 *     0x1000 SQ0TDBL   0x1004 CQ0HDBL
 *     0x1008 SQ1TDBL   0x100C CQ1HDBL
 *     0x1010 SQ2TDBL   0x1014 CQ2HDBL
 *     0x1018 SQ3TDBL   0x101C CQ3HDBL
 *
 * The previous definition used a 4-byte stride, which is only correct for
 * qid 0. Every I/O queue's tail doorbell therefore went to the wrong
 * register: qid 1 wrote CQ0HDBL (the admin CQ head), qid 2 wrote 0x1008
 * (SQ1TDBL), and so on - so the SSD was never told that a new entry had
 * been posted to an I/O submission queue. The admin queue (qid 0, offsets
 * 0x1000/0x1004) was unaffected, which is why only I/O hung. */
#define XNVME_DB_SQ(n, dstrd)  (0x1000u + (u32)(n) * 2u * (4u << (dstrd)))
#define XNVME_DB_CQ(n, dstrd)  (0x1000u + ((u32)(n) * 2u + 1u) * (4u << (dstrd)))

/* CC register fields */
#define XNVME_CC_EN                   (1u << 0)
#define XNVME_CC_CSS_SHIFT            4u
#define XNVME_CC_MPS_SHIFT            7u
#define XNVME_CC_AMS_SHIFT            11u
#define XNVME_CC_SHN_SHIFT            14u
#define XNVME_CC_IOCQES_SHIFT         20u
#define XNVME_CC_IOSQES_SHIFT         16u

/* CSTS register fields */
#define XNVME_CSTS_RDY                (1u << 0)
#define XNVME_CSTS_SHST_MASK          (3u << 2)
#define XNVME_CSTS_CFS                (1u << 1)

/* CAP register fields */
#define XNVME_CAP_DSTRD(cap)          (u32)(((cap) >> 32) & 0xFu)
#define XNVME_CAP_TO(cap)             (u32)(((cap) >> 24) & 0xFFu)
#define XNVME_CAP_MPSMIN(cap)         (u32)(((cap) >> 48) & 0xFu)
#define XNVME_CAP_MPSMAX(cap)         (u32)(((cap) >> 52) & 0xFu)
#define XNVME_CAP_MQES(cap)           (u16)((cap) & 0xFFFFu)

/* Memory page size for a given MPS value: 2^(12+MPS) bytes. */
#define XNVME_PAGE_SIZE(mps)          (4096u << (mps))

/* PRP list capacity PER COMMAND, in 8-byte entries.
 *
 * There is one list per command slot (see xnvmeha_prp_list in xnvmeha.c), and
 * the biggest single command is XNVMEHA_MAX_XFER (512 KiB = 128 pages), which
 * needs 126 entries (PRP1 + PRP2 cover the first two pages inline in the SQE).
 * 128 leaves a little headroom.
 *
 * This also bounds the PRP-mode transfer size in XNvme_MaxBlocksPerCmd:
 * (128 * 4096) / 512 = 1024 LBAs = 512 KiB, which is exactly max_xfer - so the
 * two limits stay consistent.                                          */
#define XNVMEHA_PRP_LIST_MAX          128u

#endif /* XNVMEHA_HW_H_ */
