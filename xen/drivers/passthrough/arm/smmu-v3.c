// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU API for ARM architected SMMUv3 implementations.
 *
 * Copyright (C) 2015 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 *
 * This driver is powered by bad coffee and bombay mix.
 */

#include <linux/acpi.h>
#include <linux/acpi_iort.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/crash_dump.h>
#include <linux/delay.h>
#include <linux/dma-iommu.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io-pgtable.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/pci-ats.h>
#include <linux/platform_device.h>

#include <linux/amba/bus.h>

/* MMIO registers */
#define ARM_SMMU_IDR0			0x0
#define IDR0_ST_LVL			GENMASK(28, 27)
#define IDR0_ST_LVL_2LVL		1
#define IDR0_STALL_MODEL		GENMASK(25, 24)
#define IDR0_STALL_MODEL_STALL		0
#define IDR0_STALL_MODEL_FORCE		2
#define IDR0_TTENDIAN			GENMASK(22, 21)
#define IDR0_TTENDIAN_MIXED		0
#define IDR0_TTENDIAN_LE		2
#define IDR0_TTENDIAN_BE		3
#define IDR0_CD2L			(1 << 19)
#define IDR0_VMID16			(1 << 18)
#define IDR0_PRI			(1 << 16)
#define IDR0_SEV			(1 << 14)
#define IDR0_MSI			(1 << 13)
#define IDR0_ASID16			(1 << 12)
#define IDR0_ATS			(1 << 10)
#define IDR0_HYP			(1 << 9)
#define IDR0_COHACC			(1 << 4)
#define IDR0_TTF			GENMASK(3, 2)
#define IDR0_TTF_AARCH64		2
#define IDR0_TTF_AARCH32_64		3
#define IDR0_S1P			(1 << 1)
#define IDR0_S2P			(1 << 0)

#define ARM_SMMU_IDR1			0x4
#define IDR1_TABLES_PRESET		(1 << 30)
#define IDR1_QUEUES_PRESET		(1 << 29)
#define IDR1_REL			(1 << 28)
#define IDR1_CMDQS			GENMASK(25, 21)
#define IDR1_EVTQS			GENMASK(20, 16)
#define IDR1_PRIQS			GENMASK(15, 11)
#define IDR1_SSIDSIZE			GENMASK(10, 6)
#define IDR1_SIDSIZE			GENMASK(5, 0)

#define ARM_SMMU_IDR5			0x14
#define IDR5_STALL_MAX			GENMASK(31, 16)
#define IDR5_GRAN64K			(1 << 6)
#define IDR5_GRAN16K			(1 << 5)
#define IDR5_GRAN4K			(1 << 4)
#define IDR5_OAS			GENMASK(2, 0)
#define IDR5_OAS_32_BIT			0
#define IDR5_OAS_36_BIT			1
#define IDR5_OAS_40_BIT			2
#define IDR5_OAS_42_BIT			3
#define IDR5_OAS_44_BIT			4
#define IDR5_OAS_48_BIT			5
#define IDR5_OAS_52_BIT			6
#define IDR5_VAX			GENMASK(11, 10)
#define IDR5_VAX_52_BIT			1

#define ARM_SMMU_CR0			0x20
#define CR0_ATSCHK			(1 << 4)
#define CR0_CMDQEN			(1 << 3)
#define CR0_EVTQEN			(1 << 2)
#define CR0_PRIQEN			(1 << 1)
#define CR0_SMMUEN			(1 << 0)

#define ARM_SMMU_CR0ACK			0x24

#define ARM_SMMU_CR1			0x28
#define CR1_TABLE_SH			GENMASK(11, 10)
#define CR1_TABLE_OC			GENMASK(9, 8)
#define CR1_TABLE_IC			GENMASK(7, 6)
#define CR1_QUEUE_SH			GENMASK(5, 4)
#define CR1_QUEUE_OC			GENMASK(3, 2)
#define CR1_QUEUE_IC			GENMASK(1, 0)
/* CR1 cacheability fields don't quite follow the usual TCR-style encoding */
#define CR1_CACHE_NC			0
#define CR1_CACHE_WB			1
#define CR1_CACHE_WT			2

#define ARM_SMMU_CR2			0x2c
#define CR2_PTM				(1 << 2)
#define CR2_RECINVSID			(1 << 1)
#define CR2_E2H				(1 << 0)

#define ARM_SMMU_GBPA			0x44
#define GBPA_UPDATE			(1 << 31)
#define GBPA_ABORT			(1 << 20)

#define ARM_SMMU_IRQ_CTRL		0x50
#define IRQ_CTRL_EVTQ_IRQEN		(1 << 2)
#define IRQ_CTRL_PRIQ_IRQEN		(1 << 1)
#define IRQ_CTRL_GERROR_IRQEN		(1 << 0)

#define ARM_SMMU_IRQ_CTRLACK		0x54

#define ARM_SMMU_GERROR			0x60
#define GERROR_SFM_ERR			(1 << 8)
#define GERROR_MSI_GERROR_ABT_ERR	(1 << 7)
#define GERROR_MSI_PRIQ_ABT_ERR		(1 << 6)
#define GERROR_MSI_EVTQ_ABT_ERR		(1 << 5)
#define GERROR_MSI_CMDQ_ABT_ERR		(1 << 4)
#define GERROR_PRIQ_ABT_ERR		(1 << 3)
#define GERROR_EVTQ_ABT_ERR		(1 << 2)
#define GERROR_CMDQ_ERR			(1 << 0)
#define GERROR_ERR_MASK			0xfd

#define ARM_SMMU_GERRORN		0x64

#define ARM_SMMU_GERROR_IRQ_CFG0	0x68
#define ARM_SMMU_GERROR_IRQ_CFG1	0x70
#define ARM_SMMU_GERROR_IRQ_CFG2	0x74

#define ARM_SMMU_STRTAB_BASE		0x80
#define STRTAB_BASE_RA			(1UL << 62)
#define STRTAB_BASE_ADDR_MASK		GENMASK_ULL(51, 6)

#define ARM_SMMU_STRTAB_BASE_CFG	0x88
#define STRTAB_BASE_CFG_FMT		GENMASK(17, 16)
#define STRTAB_BASE_CFG_FMT_LINEAR	0
#define STRTAB_BASE_CFG_FMT_2LVL	1
#define STRTAB_BASE_CFG_SPLIT		GENMASK(10, 6)
#define STRTAB_BASE_CFG_LOG2SIZE	GENMASK(5, 0)

#define ARM_SMMU_CMDQ_BASE		0x90
#define ARM_SMMU_CMDQ_PROD		0x98
#define ARM_SMMU_CMDQ_CONS		0x9c

#define ARM_SMMU_EVTQ_BASE		0xa0
#define ARM_SMMU_EVTQ_PROD		0x100a8
#define ARM_SMMU_EVTQ_CONS		0x100ac
#define ARM_SMMU_EVTQ_IRQ_CFG0		0xb0
#define ARM_SMMU_EVTQ_IRQ_CFG1		0xb8
#define ARM_SMMU_EVTQ_IRQ_CFG2		0xbc

#define ARM_SMMU_PRIQ_BASE		0xc0
#define ARM_SMMU_PRIQ_PROD		0x100c8
#define ARM_SMMU_PRIQ_CONS		0x100cc
#define ARM_SMMU_PRIQ_IRQ_CFG0		0xd0
#define ARM_SMMU_PRIQ_IRQ_CFG1		0xd8
#define ARM_SMMU_PRIQ_IRQ_CFG2		0xdc

#define ARM_SMMU_REG_SZ			0xe00

/* Common MSI config fields */
#define MSI_CFG0_ADDR_MASK		GENMASK_ULL(51, 2)
#define MSI_CFG2_SH			GENMASK(5, 4)
#define MSI_CFG2_MEMATTR		GENMASK(3, 0)

/* Common memory attribute values */
#define ARM_SMMU_SH_NSH			0
#define ARM_SMMU_SH_OSH			2
#define ARM_SMMU_SH_ISH			3
#define ARM_SMMU_MEMATTR_DEVICE_nGnRE	0x1
#define ARM_SMMU_MEMATTR_OIWB		0xf

#define Q_IDX(llq, p)			((p) & ((1 << (llq)->max_n_shift) - 1))
#define Q_WRP(llq, p)			((p) & (1 << (llq)->max_n_shift))
#define Q_OVERFLOW_FLAG			(1 << 31)
#define Q_OVF(p)			((p) & Q_OVERFLOW_FLAG)
#define Q_ENT(q, p)			((q)->base +			\
					 Q_IDX(&((q)->llq), p) *	\
					 (q)->ent_dwords)

#define Q_BASE_RWA			(1UL << 62)
#define Q_BASE_ADDR_MASK		GENMASK_ULL(51, 5)
#define Q_BASE_LOG2SIZE			GENMASK(4, 0)

/* Ensure DMA allocations are naturally aligned */
#ifdef CONFIG_CMA_ALIGNMENT
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + CONFIG_CMA_ALIGNMENT)
#else
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + MAX_ORDER - 1)
#endif

/*
 * Stream table.
 *
 * Linear: Enough to cover 1 << IDR1.SIDSIZE entries
 * 2lvl: 128k L1 entries,
 *       256 lazy entries per table (each table covers a PCI bus)
 */
#define STRTAB_L1_SZ_SHIFT		20
#define STRTAB_SPLIT			8

#define STRTAB_L1_DESC_DWORDS		1
#define STRTAB_L1_DESC_SPAN		GENMASK_ULL(4, 0)
#define STRTAB_L1_DESC_L2PTR_MASK	GENMASK_ULL(51, 6)

#define STRTAB_STE_DWORDS		8
#define STRTAB_STE_0_V			(1UL << 0)
#define STRTAB_STE_0_CFG		GENMASK_ULL(3, 1)
#define STRTAB_STE_0_CFG_ABORT		0
#define STRTAB_STE_0_CFG_BYPASS		4
#define STRTAB_STE_0_CFG_S1_TRANS	5
#define STRTAB_STE_0_CFG_S2_TRANS	6

#define STRTAB_STE_0_S1FMT		GENMASK_ULL(5, 4)
#define STRTAB_STE_0_S1FMT_LINEAR	0
#define STRTAB_STE_0_S1FMT_64K_L2	2
#define STRTAB_STE_0_S1CTXPTR_MASK	GENMASK_ULL(51, 6)
#define STRTAB_STE_0_S1CDMAX		GENMASK_ULL(63, 59)

#define STRTAB_STE_1_S1DSS		GENMASK_ULL(1, 0)
#define STRTAB_STE_1_S1DSS_TERMINATE	0x0
#define STRTAB_STE_1_S1DSS_BYPASS	0x1
#define STRTAB_STE_1_S1DSS_SSID0	0x2

#define STRTAB_STE_1_S1C_CACHE_NC	0UL
#define STRTAB_STE_1_S1C_CACHE_WBRA	1UL
#define STRTAB_STE_1_S1C_CACHE_WT	2UL
#define STRTAB_STE_1_S1C_CACHE_WB	3UL
#define STRTAB_STE_1_S1CIR		GENMASK_ULL(3, 2)
#define STRTAB_STE_1_S1COR		GENMASK_ULL(5, 4)
#define STRTAB_STE_1_S1CSH		GENMASK_ULL(7, 6)

#define STRTAB_STE_1_S1STALLD		(1UL << 27)

#define STRTAB_STE_1_EATS		GENMASK_ULL(29, 28)
#define STRTAB_STE_1_EATS_ABT		0UL
#define STRTAB_STE_1_EATS_TRANS		1UL
#define STRTAB_STE_1_EATS_S1CHK		2UL

#define STRTAB_STE_1_STRW		GENMASK_ULL(31, 30)
#define STRTAB_STE_1_STRW_NSEL1		0UL
#define STRTAB_STE_1_STRW_EL2		2UL

#define STRTAB_STE_1_SHCFG		GENMASK_ULL(45, 44)
#define STRTAB_STE_1_SHCFG_INCOMING	1UL

#define STRTAB_STE_2_S2VMID		GENMASK_ULL(15, 0)
#define STRTAB_STE_2_VTCR		GENMASK_ULL(50, 32)
#define STRTAB_STE_2_VTCR_S2T0SZ	GENMASK_ULL(5, 0)
#define STRTAB_STE_2_VTCR_S2SL0		GENMASK_ULL(7, 6)
#define STRTAB_STE_2_VTCR_S2IR0		GENMASK_ULL(9, 8)
#define STRTAB_STE_2_VTCR_S2OR0		GENMASK_ULL(11, 10)
#define STRTAB_STE_2_VTCR_S2SH0		GENMASK_ULL(13, 12)
#define STRTAB_STE_2_VTCR_S2TG		GENMASK_ULL(15, 14)
#define STRTAB_STE_2_VTCR_S2PS		GENMASK_ULL(18, 16)
#define STRTAB_STE_2_S2AA64		(1UL << 51)
#define STRTAB_STE_2_S2ENDI		(1UL << 52)
#define STRTAB_STE_2_S2PTW		(1UL << 54)
#define STRTAB_STE_2_S2R		(1UL << 58)

#define STRTAB_STE_3_S2TTB_MASK		GENMASK_ULL(51, 4)

/*
 * Context descriptors.
 *
 * Linear: when less than 1024 SSIDs are supported
 * 2lvl: at most 1024 L1 entries,
 *       1024 lazy entries per table.
 */
#define CTXDESC_SPLIT			10
#define CTXDESC_L2_ENTRIES		(1 << CTXDESC_SPLIT)

#define CTXDESC_L1_DESC_DWORDS		1
#define CTXDESC_L1_DESC_V		(1UL << 0)
#define CTXDESC_L1_DESC_L2PTR_MASK	GENMASK_ULL(51, 12)

#define CTXDESC_CD_DWORDS		8
#define CTXDESC_CD_0_TCR_T0SZ		GENMASK_ULL(5, 0)
#define CTXDESC_CD_0_TCR_TG0		GENMASK_ULL(7, 6)
#define CTXDESC_CD_0_TCR_IRGN0		GENMASK_ULL(9, 8)
#define CTXDESC_CD_0_TCR_ORGN0		GENMASK_ULL(11, 10)
#define CTXDESC_CD_0_TCR_SH0		GENMASK_ULL(13, 12)
#define CTXDESC_CD_0_TCR_EPD0		(1ULL << 14)
#define CTXDESC_CD_0_TCR_EPD1		(1ULL << 30)

#define CTXDESC_CD_0_ENDI		(1UL << 15)
#define CTXDESC_CD_0_V			(1UL << 31)

#define CTXDESC_CD_0_TCR_IPS		GENMASK_ULL(34, 32)
#define CTXDESC_CD_0_TCR_TBI0		(1ULL << 38)

#define CTXDESC_CD_0_AA64		(1UL << 41)
#define CTXDESC_CD_0_S			(1UL << 44)
#define CTXDESC_CD_0_R			(1UL << 45)
#define CTXDESC_CD_0_A			(1UL << 46)
#define CTXDESC_CD_0_ASET		(1UL << 47)
#define CTXDESC_CD_0_ASID		GENMASK_ULL(63, 48)

#define CTXDESC_CD_1_TTB0_MASK		GENMASK_ULL(51, 4)

/*
 * When the SMMU only supports linear context descriptor tables, pick a
 * reasonable size limit (64kB).
 */
#define CTXDESC_LINEAR_CDMAX		ilog2(SZ_64K / (CTXDESC_CD_DWORDS << 3))

/* Command queue */
#define CMDQ_ENT_SZ_SHIFT		4
#define CMDQ_ENT_DWORDS			((1 << CMDQ_ENT_SZ_SHIFT) >> 3)
#define CMDQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - CMDQ_ENT_SZ_SHIFT)

#define CMDQ_CONS_ERR			GENMASK(30, 24)
#define CMDQ_ERR_CERROR_NONE_IDX	0
#define CMDQ_ERR_CERROR_ILL_IDX		1
#define CMDQ_ERR_CERROR_ABT_IDX		2
#define CMDQ_ERR_CERROR_ATC_INV_IDX	3

#define CMDQ_0_OP			GENMASK_ULL(7, 0)
#define CMDQ_0_SSV			(1UL << 11)

#define CMDQ_PREFETCH_0_SID		GENMASK_ULL(63, 32)
#define CMDQ_PREFETCH_1_SIZE		GENMASK_ULL(4, 0)
#define CMDQ_PREFETCH_1_ADDR_MASK	GENMASK_ULL(63, 12)

#define CMDQ_CFGI_0_SSID		GENMASK_ULL(31, 12)
#define CMDQ_CFGI_0_SID			GENMASK_ULL(63, 32)
#define CMDQ_CFGI_1_LEAF		(1UL << 0)
#define CMDQ_CFGI_1_RANGE		GENMASK_ULL(4, 0)

#define CMDQ_TLBI_0_VMID		GENMASK_ULL(47, 32)
#define CMDQ_TLBI_0_ASID		GENMASK_ULL(63, 48)
#define CMDQ_TLBI_1_LEAF		(1UL << 0)
#define CMDQ_TLBI_1_VA_MASK		GENMASK_ULL(63, 12)
#define CMDQ_TLBI_1_IPA_MASK		GENMASK_ULL(51, 12)

#define CMDQ_ATC_0_SSID			GENMASK_ULL(31, 12)
#define CMDQ_ATC_0_SID			GENMASK_ULL(63, 32)
#define CMDQ_ATC_0_GLOBAL		(1UL << 9)
#define CMDQ_ATC_1_SIZE			GENMASK_ULL(5, 0)
#define CMDQ_ATC_1_ADDR_MASK		GENMASK_ULL(63, 12)

#define CMDQ_PRI_0_SSID			GENMASK_ULL(31, 12)
#define CMDQ_PRI_0_SID			GENMASK_ULL(63, 32)
#define CMDQ_PRI_1_GRPID		GENMASK_ULL(8, 0)
#define CMDQ_PRI_1_RESP			GENMASK_ULL(13, 12)

#define CMDQ_SYNC_0_CS			GENMASK_ULL(13, 12)
#define CMDQ_SYNC_0_CS_NONE		0
#define CMDQ_SYNC_0_CS_IRQ		1
#define CMDQ_SYNC_0_CS_SEV		2
#define CMDQ_SYNC_0_MSH			GENMASK_ULL(23, 22)
#define CMDQ_SYNC_0_MSIATTR		GENMASK_ULL(27, 24)
#define CMDQ_SYNC_0_MSIDATA		GENMASK_ULL(63, 32)
#define CMDQ_SYNC_1_MSIADDR_MASK	GENMASK_ULL(51, 2)

/* Event queue */
#define EVTQ_ENT_SZ_SHIFT		5
#define EVTQ_ENT_DWORDS			((1 << EVTQ_ENT_SZ_SHIFT) >> 3)
#define EVTQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - EVTQ_ENT_SZ_SHIFT)

#define EVTQ_0_ID			GENMASK_ULL(7, 0)

/* PRI queue */
#define PRIQ_ENT_SZ_SHIFT		4
#define PRIQ_ENT_DWORDS			((1 << PRIQ_ENT_SZ_SHIFT) >> 3)
#define PRIQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - PRIQ_ENT_SZ_SHIFT)

#define PRIQ_0_SID			GENMASK_ULL(31, 0)
#define PRIQ_0_SSID			GENMASK_ULL(51, 32)
#define PRIQ_0_PERM_PRIV		(1UL << 58)
#define PRIQ_0_PERM_EXEC		(1UL << 59)
#define PRIQ_0_PERM_READ		(1UL << 60)
#define PRIQ_0_PERM_WRITE		(1UL << 61)
#define PRIQ_0_PRG_LAST			(1UL << 62)
#define PRIQ_0_SSID_V			(1UL << 63)

#define PRIQ_1_PRG_IDX			GENMASK_ULL(8, 0)
#define PRIQ_1_ADDR_MASK		GENMASK_ULL(63, 12)

/* High-level queue structures */
#define ARM_SMMU_POLL_TIMEOUT_US	100
#define ARM_SMMU_CMDQ_SYNC_TIMEOUT_US	1000000 /* 1s! */
#define ARM_SMMU_CMDQ_SYNC_SPIN_COUNT	10

static bool disable_bypass = 1;

enum pri_resp {
	PRI_RESP_DENY = 0,
	PRI_RESP_FAIL = 1,
	PRI_RESP_SUCC = 2,
};

enum arm_smmu_msi_index {
	EVTQ_MSI_INDEX,
	GERROR_MSI_INDEX,
	PRIQ_MSI_INDEX,
	ARM_SMMU_MAX_MSIS,
};

static phys_addr_t arm_smmu_msi_cfg[ARM_SMMU_MAX_MSIS][3] = {
	[EVTQ_MSI_INDEX] = {
		ARM_SMMU_EVTQ_IRQ_CFG0,
		ARM_SMMU_EVTQ_IRQ_CFG1,
		ARM_SMMU_EVTQ_IRQ_CFG2,
	},
	[GERROR_MSI_INDEX] = {
		ARM_SMMU_GERROR_IRQ_CFG0,
		ARM_SMMU_GERROR_IRQ_CFG1,
		ARM_SMMU_GERROR_IRQ_CFG2,
	},
	[PRIQ_MSI_INDEX] = {
		ARM_SMMU_PRIQ_IRQ_CFG0,
		ARM_SMMU_PRIQ_IRQ_CFG1,
		ARM_SMMU_PRIQ_IRQ_CFG2,
	},
};

struct arm_smmu_cmdq_ent {
	/* Common fields */
	u8				opcode;
	bool				substream_valid;

	/* Command-specific fields */
	union {
		#define CMDQ_OP_PREFETCH_CFG	0x1
		struct {
			u32			sid;
			u8			size;
			u64			addr;
		} prefetch;

		#define CMDQ_OP_CFGI_STE	0x3
		#define CMDQ_OP_CFGI_ALL	0x4
		struct {
			u32			sid;
			union {
				bool		leaf;
				u8		span;
			};
		} cfgi;

		#define CMDQ_OP_TLBI_EL2_ALL	0x20
		#define CMDQ_OP_TLBI_S12_VMALL	0x28
		#define CMDQ_OP_TLBI_S2_IPA	0x2a
		#define CMDQ_OP_TLBI_NSNH_ALL	0x30
		struct {
			u16			asid;
			u16			vmid;
			bool			leaf;
			u64			addr;
		} tlbi;

		#define CMDQ_OP_ATC_INV		0x40
		#define ATC_INV_SIZE_ALL	52
		struct {
			u32			sid;
			u32			ssid;
			u64			addr;
			u8			size;
			bool			global;
		} atc;

		#define CMDQ_OP_PRI_RESP	0x41
		struct {
			u32			sid;
			u32			ssid;
			u16			grpid;
			enum pri_resp		resp;
		} pri;

		#define CMDQ_OP_CMD_SYNC	0x46
		struct {
			u32			msidata;
			u64			msiaddr;
		} sync;
	};
};

struct arm_smmu_ll_queue {
	u32				prod;
	u32				cons;
	u32				max_n_shift;
};

struct arm_smmu_queue {
	struct arm_smmu_ll_queue	llq;
	int				irq; /* Wired interrupt */

	__le64				*base;
	dma_addr_t			base_dma;
	u64				q_base;

	size_t				ent_dwords;

	u32 __iomem			*prod_reg;
	u32 __iomem			*cons_reg;
};

struct arm_smmu_cmdq {
	struct arm_smmu_queue		q;
	spinlock_t			lock;
};

struct arm_smmu_evtq {
	struct arm_smmu_queue		q;
	u32				max_stalls;
};

struct arm_smmu_priq {
	struct arm_smmu_queue		q;
};

/* High-level stream table and context descriptor structures */
struct arm_smmu_strtab_l1_desc {
	u8				span;

	__le64				*l2ptr;
	dma_addr_t			l2ptr_dma;
};

struct arm_smmu_s2_cfg {
	u16				vmid;
	u64				vttbr;
	u64				vtcr;
};

struct arm_smmu_strtab_cfg {
	__le64				*strtab;
	dma_addr_t			strtab_dma;
	struct arm_smmu_strtab_l1_desc	*l1_desc;
	unsigned int			num_l1_ents;

	u64				strtab_base;
	u32				strtab_base_cfg;
};

struct arm_lpae_s2_cfg {
	u64			vttbr;
	struct {
		u32			ps:3;
		u32			tg:2;
		u32			sh:2;
		u32			orgn:2;
		u32			irgn:2;
		u32			sl:2;
		u32			tsz:6;
	} vtcr;
};

/* An SMMUv3 instance */
struct arm_smmu_device {
	struct device			*dev;
	void __iomem			*base;
	void __iomem			*page1;

#define ARM_SMMU_FEAT_2_LVL_STRTAB	(1 << 0)
#define ARM_SMMU_FEAT_2_LVL_CDTAB	(1 << 1)
#define ARM_SMMU_FEAT_TT_LE		(1 << 2)
#define ARM_SMMU_FEAT_TT_BE		(1 << 3)
#define ARM_SMMU_FEAT_PRI		(1 << 4)
#define ARM_SMMU_FEAT_ATS		(1 << 5)
#define ARM_SMMU_FEAT_SEV		(1 << 6)
#define ARM_SMMU_FEAT_MSI		(1 << 7)
#define ARM_SMMU_FEAT_COHERENCY		(1 << 8)
#define ARM_SMMU_FEAT_TRANS_S1		(1 << 9)
#define ARM_SMMU_FEAT_TRANS_S2		(1 << 10)
#define ARM_SMMU_FEAT_STALLS		(1 << 11)
#define ARM_SMMU_FEAT_HYP		(1 << 12)
#define ARM_SMMU_FEAT_STALL_FORCE	(1 << 13)
#define ARM_SMMU_FEAT_VAX		(1 << 14)
	u32				features;

#define ARM_SMMU_OPT_SKIP_PREFETCH	(1 << 0)
#define ARM_SMMU_OPT_PAGE0_REGS_ONLY	(1 << 1)
	u32				options;

	struct arm_smmu_cmdq		cmdq;
	struct arm_smmu_evtq		evtq;
	struct arm_smmu_priq		priq;

	int				gerr_irq;
	int				combined_irq;
	u32				sync_nr;
	u8				prev_cmd_opcode;

	unsigned long			ias; /* IPA */
	unsigned long			oas; /* PA */
	unsigned long			pgsize_bitmap;

#define ARM_SMMU_MAX_VMIDS		(1 << 16)
	unsigned int			vmid_bits;
	DECLARE_BITMAP(vmid_map, ARM_SMMU_MAX_VMIDS);

	unsigned int			sid_bits;

	struct arm_smmu_strtab_cfg	strtab_cfg;

	/* Hi16xx adds an extra 32 bits of goodness to its MSI payload */
	union {
		u32			sync_count;
		u64			padding;
	};

	/* IOMMU core code handle */
	struct iommu_device		iommu;
};

/* SMMU private data for each master */
struct arm_smmu_master {
	struct arm_smmu_device		*smmu;
	struct device			*dev;
	struct arm_smmu_domain		*domain;
	struct list_head		domain_head;
	u32				*sids;
	unsigned int			num_sids;
	bool				ats_enabled;
};

/* SMMU private data for an IOMMU domain */
enum arm_smmu_domain_stage {
	ARM_SMMU_DOMAIN_S1 = 0,
	ARM_SMMU_DOMAIN_S2,
	ARM_SMMU_DOMAIN_NESTED,
	ARM_SMMU_DOMAIN_BYPASS,
};

struct arm_smmu_domain {
	struct arm_smmu_device		*smmu;
	struct mutex			init_mutex; /* Protects smmu pointer */

	bool				non_strict;
	atomic_t			nr_ats_masters;

	enum arm_smmu_domain_stage	stage;
	struct arm_smmu_s2_cfg	s2_cfg;

	struct iommu_domain		domain;

	struct list_head		devices;
	spinlock_t			devices_lock;
};

struct arm_smmu_option_prop {
	u32 opt;
	const char *prop;
};

static struct arm_smmu_option_prop arm_smmu_options[] = {
	{ ARM_SMMU_OPT_SKIP_PREFETCH, "hisilicon,broken-prefetch-cmd" },
	{ ARM_SMMU_OPT_PAGE0_REGS_ONLY, "cavium,cn9900-broken-page1-regspace"},
	{ 0, NULL},
};

static inline void __iomem *arm_smmu_page1_fixup(unsigned long offset,
						 struct arm_smmu_device *smmu)
{
	if (offset > SZ_64K)
		return smmu->page1 + offset - SZ_64K;

	return smmu->base + offset;
}

static struct arm_smmu_domain *to_smmu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_domain, domain);
}

static void parse_driver_options(struct arm_smmu_device *smmu)
{
	int i = 0;

	do {
		if (of_property_read_bool(smmu->dev->of_node,
						arm_smmu_options[i].prop)) {
			smmu->options |= arm_smmu_options[i].opt;
			dev_notice(smmu->dev, "option %s\n",
				arm_smmu_options[i].prop);
		}
	} while (arm_smmu_options[++i].opt);
}

/* Low-level queue manipulation functions */
static bool queue_full(struct arm_smmu_ll_queue *q)
{
	return Q_IDX(q, q->prod) == Q_IDX(q, q->cons) &&
	       Q_WRP(q, q->prod) != Q_WRP(q, q->cons);
}

static bool queue_empty(struct arm_smmu_ll_queue *q)
{
	return Q_IDX(q, q->prod) == Q_IDX(q, q->cons) &&
	       Q_WRP(q, q->prod) == Q_WRP(q, q->cons);
}

static void queue_sync_cons_in(struct arm_smmu_queue *q)
{
	q->llq.cons = readl_relaxed(q->cons_reg);
}

static void queue_sync_cons_out(struct arm_smmu_queue *q)
{
	/*
	 * Ensure that all CPU accesses (reads and writes) to the queue
	 * are complete before we update the cons pointer.
	 */
	mb();
	writel_relaxed(q->llq.cons, q->cons_reg);
}

static void queue_inc_cons(struct arm_smmu_ll_queue *q)
{
	u32 cons = (Q_WRP(q, q->cons) | Q_IDX(q, q->cons)) + 1;
	q->cons = Q_OVF(q->cons) | Q_WRP(q, cons) | Q_IDX(q, cons);
}

static int queue_sync_prod_in(struct arm_smmu_queue *q)
{
	int ret = 0;
	u32 prod = readl_relaxed(q->prod_reg);

	if (Q_OVF(prod) != Q_OVF(q->llq.prod))
		ret = -EOVERFLOW;

	q->llq.prod = prod;
	return ret;
}

static void queue_sync_prod_out(struct arm_smmu_queue *q)
{
	writel(q->llq.prod, q->prod_reg);
}

static void queue_inc_prod(struct arm_smmu_ll_queue *q)
{
	u32 prod = (Q_WRP(q, q->prod) | Q_IDX(q, q->prod)) + 1;
	q->prod = Q_OVF(q->prod) | Q_WRP(q, prod) | Q_IDX(q, prod);
}

/*
 * Wait for the SMMU to consume items. If sync is true, wait until the queue
 * is empty. Otherwise, wait until there is at least one free slot.
 */
static int queue_poll_cons(struct arm_smmu_queue *q, bool sync, bool wfe)
{
	ktime_t timeout;
	unsigned int delay = 1, spin_cnt = 0;

	/* Wait longer if it's a CMD_SYNC */
	timeout = ktime_add_us(ktime_get(), sync ?
					    ARM_SMMU_CMDQ_SYNC_TIMEOUT_US :
					    ARM_SMMU_POLL_TIMEOUT_US);

	while (queue_sync_cons_in(q),
	      (sync ? !queue_empty(&q->llq) : queue_full(&q->llq))) {
		if (ktime_compare(ktime_get(), timeout) > 0)
			return -ETIMEDOUT;

		if (wfe) {
			wfe();
		} else if (++spin_cnt < ARM_SMMU_CMDQ_SYNC_SPIN_COUNT) {
			cpu_relax();
			continue;
		} else {
			udelay(delay);
			delay *= 2;
			spin_cnt = 0;
		}
	}

	return 0;
}

static void queue_write(__le64 *dst, u64 *src, size_t n_dwords)
{
	int i;

	for (i = 0; i < n_dwords; ++i)
		*dst++ = cpu_to_le64(*src++);
}

static int queue_insert_raw(struct arm_smmu_queue *q, u64 *ent)
{
	if (queue_full(&q->llq))
		return -ENOSPC;

	queue_write(Q_ENT(q, q->llq.prod), ent, q->ent_dwords);
	queue_inc_prod(&q->llq);
	queue_sync_prod_out(q);
	return 0;
}

static void queue_read(__le64 *dst, u64 *src, size_t n_dwords)
{
	int i;

	for (i = 0; i < n_dwords; ++i)
		*dst++ = le64_to_cpu(*src++);
}

static int queue_remove_raw(struct arm_smmu_queue *q, u64 *ent)
{
	if (queue_empty(&q->llq))
		return -EAGAIN;

	queue_read(ent, Q_ENT(q, q->llq.cons), q->ent_dwords);
	queue_inc_cons(&q->llq);
	queue_sync_cons_out(q);
	return 0;
}

/* High-level queue accessors */
static int arm_smmu_cmdq_build_cmd(u64 *cmd, struct arm_smmu_cmdq_ent *ent)
{
	memset(cmd, 0, 1 << CMDQ_ENT_SZ_SHIFT);
	cmd[0] |= FIELD_PREP(CMDQ_0_OP, ent->opcode);

	switch (ent->opcode) {
	case CMDQ_OP_TLBI_EL2_ALL:
	case CMDQ_OP_TLBI_NSNH_ALL:
		break;
	case CMDQ_OP_PREFETCH_CFG:
		cmd[0] |= FIELD_PREP(CMDQ_PREFETCH_0_SID, ent->prefetch.sid);
		cmd[1] |= FIELD_PREP(CMDQ_PREFETCH_1_SIZE, ent->prefetch.size);
		cmd[1] |= ent->prefetch.addr & CMDQ_PREFETCH_1_ADDR_MASK;
		break;
	case CMDQ_OP_CFGI_STE:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_LEAF, ent->cfgi.leaf);
		break;
	case CMDQ_OP_CFGI_ALL:
		/* Cover the entire SID range */
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_RANGE, 31);
		break;
	case CMDQ_OP_TLBI_S2_IPA:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_IPA_MASK;
		break;
	case CMDQ_OP_TLBI_S12_VMALL:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		break;
	case CMDQ_OP_ATC_INV:
		cmd[0] |= FIELD_PREP(CMDQ_0_SSV, ent->substream_valid);
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_GLOBAL, ent->atc.global);
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_SSID, ent->atc.ssid);
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_SID, ent->atc.sid);
		cmd[1] |= FIELD_PREP(CMDQ_ATC_1_SIZE, ent->atc.size);
		cmd[1] |= ent->atc.addr & CMDQ_ATC_1_ADDR_MASK;
		break;
	case CMDQ_OP_PRI_RESP:
		cmd[0] |= FIELD_PREP(CMDQ_0_SSV, ent->substream_valid);
		cmd[0] |= FIELD_PREP(CMDQ_PRI_0_SSID, ent->pri.ssid);
		cmd[0] |= FIELD_PREP(CMDQ_PRI_0_SID, ent->pri.sid);
		cmd[1] |= FIELD_PREP(CMDQ_PRI_1_GRPID, ent->pri.grpid);
		switch (ent->pri.resp) {
		case PRI_RESP_DENY:
		case PRI_RESP_FAIL:
		case PRI_RESP_SUCC:
			break;
		default:
			return -EINVAL;
		}
		cmd[1] |= FIELD_PREP(CMDQ_PRI_1_RESP, ent->pri.resp);
		break;
	case CMDQ_OP_CMD_SYNC:
		if (ent->sync.msiaddr)
			cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_IRQ);
		else
			cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_MSH, ARM_SMMU_SH_ISH);
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_MSIATTR, ARM_SMMU_MEMATTR_OIWB);
		/*
		 * Commands are written little-endian, but we want the SMMU to
		 * receive MSIData, and thus write it back to memory, in CPU
		 * byte order, so big-endian needs an extra byteswap here.
		 */
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_MSIDATA,
				     cpu_to_le32(ent->sync.msidata));
		cmd[1] |= ent->sync.msiaddr & CMDQ_SYNC_1_MSIADDR_MASK;
		break;
	default:
		return -ENOENT;
	}

	return 0;
}

static void arm_smmu_cmdq_skip_err(struct arm_smmu_device *smmu)
{
	static const char *cerror_str[] = {
		[CMDQ_ERR_CERROR_NONE_IDX]	= "No error",
		[CMDQ_ERR_CERROR_ILL_IDX]	= "Illegal command",
		[CMDQ_ERR_CERROR_ABT_IDX]	= "Abort on command fetch",
		[CMDQ_ERR_CERROR_ATC_INV_IDX]	= "ATC invalidate timeout",
	};

	int i;
	u64 cmd[CMDQ_ENT_DWORDS];
	struct arm_smmu_queue *q = &smmu->cmdq.q;
	u32 cons = readl_relaxed(q->cons_reg);
	u32 idx = FIELD_GET(CMDQ_CONS_ERR, cons);
	struct arm_smmu_cmdq_ent cmd_sync = {
		.opcode = CMDQ_OP_CMD_SYNC,
	};

	dev_err(smmu->dev, "CMDQ error (cons 0x%08x): %s\n", cons,
		idx < ARRAY_SIZE(cerror_str) ?  cerror_str[idx] : "Unknown");

	switch (idx) {
	case CMDQ_ERR_CERROR_ABT_IDX:
		dev_err(smmu->dev, "retrying command fetch\n");
	case CMDQ_ERR_CERROR_NONE_IDX:
		return;
	case CMDQ_ERR_CERROR_ATC_INV_IDX:
		/*
		 * ATC Invalidation Completion timeout. CONS is still pointing
		 * at the CMD_SYNC. Attempt to complete other pending commands
		 * by repeating the CMD_SYNC, though we might well end up back
		 * here since the ATC invalidation may still be pending.
		 */
		return;
	case CMDQ_ERR_CERROR_ILL_IDX:
	default:
		break;
	}

	/*
	 * We may have concurrent producers, so we need to be careful
	 * not to touch any of the shadow cmdq state.
	 */
	queue_read(cmd, Q_ENT(q, cons), q->ent_dwords);
	dev_err(smmu->dev, "skipping command in error state:\n");
	for (i = 0; i < ARRAY_SIZE(cmd); ++i)
		dev_err(smmu->dev, "\t0x%016llx\n", (unsigned long long)cmd[i]);

	/* Convert the erroneous command into a CMD_SYNC */
	if (arm_smmu_cmdq_build_cmd(cmd, &cmd_sync)) {
		dev_err(smmu->dev, "failed to convert to CMD_SYNC\n");
		return;
	}

	queue_write(Q_ENT(q, cons), cmd, q->ent_dwords);
}

static void arm_smmu_cmdq_insert_cmd(struct arm_smmu_device *smmu, u64 *cmd)
{
	struct arm_smmu_queue *q = &smmu->cmdq.q;
	bool wfe = !!(smmu->features & ARM_SMMU_FEAT_SEV);

	smmu->prev_cmd_opcode = FIELD_GET(CMDQ_0_OP, cmd[0]);

	while (queue_insert_raw(q, cmd) == -ENOSPC) {
		if (queue_poll_cons(q, false, wfe))
			dev_err_ratelimited(smmu->dev, "CMDQ timeout\n");
	}
}

static void arm_smmu_cmdq_issue_cmd(struct arm_smmu_device *smmu,
				    struct arm_smmu_cmdq_ent *ent)
{
	u64 cmd[CMDQ_ENT_DWORDS];
	unsigned long flags;

	if (arm_smmu_cmdq_build_cmd(cmd, ent)) {
		dev_warn(smmu->dev, "ignoring unknown CMDQ opcode 0x%x\n",
			 ent->opcode);
		return;
	}

	spin_lock_irqsave(&smmu->cmdq.lock, flags);
	arm_smmu_cmdq_insert_cmd(smmu, cmd);
	spin_unlock_irqrestore(&smmu->cmdq.lock, flags);
}

/*
 * The difference between val and sync_idx is bounded by the maximum size of
 * a queue at 2^20 entries, so 32 bits is plenty for wrap-safe arithmetic.
 */
static int __arm_smmu_sync_poll_msi(struct arm_smmu_device *smmu, u32 sync_idx)
{
	ktime_t timeout;
	u32 val;

	timeout = ktime_add_us(ktime_get(), ARM_SMMU_CMDQ_SYNC_TIMEOUT_US);
	val = smp_cond_load_acquire(&smmu->sync_count,
				    (int)(VAL - sync_idx) >= 0 ||
				    !ktime_before(ktime_get(), timeout));

	return (int)(val - sync_idx) < 0 ? -ETIMEDOUT : 0;
}

static int __arm_smmu_cmdq_issue_sync_msi(struct arm_smmu_device *smmu)
{
	u64 cmd[CMDQ_ENT_DWORDS];
	unsigned long flags;
	struct arm_smmu_cmdq_ent ent = {
		.opcode = CMDQ_OP_CMD_SYNC,
		.sync	= {
			.msiaddr = virt_to_phys(&smmu->sync_count),
		},
	};

	spin_lock_irqsave(&smmu->cmdq.lock, flags);

	/* Piggy-back on the previous command if it's a SYNC */
	if (smmu->prev_cmd_opcode == CMDQ_OP_CMD_SYNC) {
		ent.sync.msidata = smmu->sync_nr;
	} else {
		ent.sync.msidata = ++smmu->sync_nr;
		arm_smmu_cmdq_build_cmd(cmd, &ent);
		arm_smmu_cmdq_insert_cmd(smmu, cmd);
	}

	spin_unlock_irqrestore(&smmu->cmdq.lock, flags);

	return __arm_smmu_sync_poll_msi(smmu, ent.sync.msidata);
}

static int __arm_smmu_cmdq_issue_sync(struct arm_smmu_device *smmu)
{
	u64 cmd[CMDQ_ENT_DWORDS];
	unsigned long flags;
	bool wfe = !!(smmu->features & ARM_SMMU_FEAT_SEV);
	struct arm_smmu_cmdq_ent ent = { .opcode = CMDQ_OP_CMD_SYNC };
	int ret;

	arm_smmu_cmdq_build_cmd(cmd, &ent);

	spin_lock_irqsave(&smmu->cmdq.lock, flags);
	arm_smmu_cmdq_insert_cmd(smmu, cmd);
	ret = queue_poll_cons(&smmu->cmdq.q, true, wfe);
	spin_unlock_irqrestore(&smmu->cmdq.lock, flags);

	return ret;
}

static int arm_smmu_cmdq_issue_sync(struct arm_smmu_device *smmu)
{
	int ret;
	bool msi = (smmu->features & ARM_SMMU_FEAT_MSI) &&
		   (smmu->features & ARM_SMMU_FEAT_COHERENCY);

	ret = msi ? __arm_smmu_cmdq_issue_sync_msi(smmu)
		  : __arm_smmu_cmdq_issue_sync(smmu);
	if (ret)
		dev_err_ratelimited(smmu->dev, "CMD_SYNC timeout\n");
	return ret;
}

/* Stream table manipulation functions */
static void
arm_smmu_write_strtab_l1_desc(__le64 *dst, struct arm_smmu_strtab_l1_desc *desc)
{
	u64 val = 0;

	val |= FIELD_PREP(STRTAB_L1_DESC_SPAN, desc->span);
	val |= desc->l2ptr_dma & STRTAB_L1_DESC_L2PTR_MASK;

	/* See comment in arm_smmu_write_ctx_desc() */
	WRITE_ONCE(*dst, cpu_to_le64(val));
}

static void arm_smmu_sync_ste_for_sid(struct arm_smmu_device *smmu, u32 sid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode	= CMDQ_OP_CFGI_STE,
		.cfgi	= {
			.sid	= sid,
			.leaf	= true,
		},
	};

	arm_smmu_cmdq_issue_cmd(smmu, &cmd);
	arm_smmu_cmdq_issue_sync(smmu);
}

static void arm_smmu_write_strtab_ent(struct arm_smmu_master *master, u32 sid,
				      __le64 *dst)
{
	/*
	 * This is hideously complicated, but we only really care about
	 * three cases at the moment:
	 *
	 * 1. Invalid (all zero) -> bypass/fault (init)
	 * 2. Bypass/fault -> translation/bypass (attach)
	 * 3. Translation/bypass -> bypass/fault (detach)
	 *
	 * Given that we can't update the STE atomically and the SMMU
	 * doesn't read the thing in a defined order, that leaves us
	 * with the following maintenance requirements:
	 *
	 * 1. Update Config, return (init time STEs aren't live)
	 * 2. Write everything apart from dword 0, sync, write dword 0, sync
	 * 3. Update Config, sync
	 */
	u64 val = le64_to_cpu(dst[0]);
	bool ste_live = false;
	struct arm_smmu_device *smmu = NULL;
	struct arm_smmu_s2_cfg *s2_cfg = NULL;
	struct arm_smmu_domain *smmu_domain = NULL;
	struct arm_smmu_cmdq_ent prefetch_cmd = {
		.opcode		= CMDQ_OP_PREFETCH_CFG,
		.prefetch	= {
			.sid	= sid,
		},
	};

	if (master) {
		smmu_domain = master->domain;
		smmu = master->smmu;
	}

	if (smmu_domain)
		s2_cfg = &smmu_domain->s2_cfg;

	if (val & STRTAB_STE_0_V) {
		switch (FIELD_GET(STRTAB_STE_0_CFG, val)) {
		case STRTAB_STE_0_CFG_BYPASS:
			break;
		case STRTAB_STE_0_CFG_S2_TRANS:
			ste_live = true;
			break;
		case STRTAB_STE_0_CFG_ABORT:
			BUG_ON(!disable_bypass);
			break;
		default:
			BUG(); /* STE corruption */
		}
	}

	/* Nuke the existing STE_0 value, as we're going to rewrite it */
	val = STRTAB_STE_0_V;

	/* Bypass/fault */
	if (!smmu_domain || !(s2_cfg)) {
		if (!smmu_domain && disable_bypass)
			val |= FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_ABORT);
		else
			val |= FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_BYPASS);

		dst[0] = cpu_to_le64(val);
		dst[1] = cpu_to_le64(FIELD_PREP(STRTAB_STE_1_SHCFG,
						STRTAB_STE_1_SHCFG_INCOMING));
		dst[2] = 0; /* Nuke the VMID */
		/*
		 * The SMMU can perform negative caching, so we must sync
		 * the STE regardless of whether the old value was live.
		 */
		if (smmu)
			arm_smmu_sync_ste_for_sid(smmu, sid);
		return;
	}

	if (s2_cfg) {
		BUG_ON(ste_live);
		dst[2] = cpu_to_le64(
			 FIELD_PREP(STRTAB_STE_2_S2VMID, s2_cfg->vmid) |
			 FIELD_PREP(STRTAB_STE_2_VTCR, s2_cfg->vtcr) |
#ifdef __BIG_ENDIAN
			 STRTAB_STE_2_S2ENDI |
#endif
			 STRTAB_STE_2_S2PTW | STRTAB_STE_2_S2AA64 |
			 STRTAB_STE_2_S2R);

		dst[3] = cpu_to_le64(s2_cfg->vttbr & STRTAB_STE_3_S2TTB_MASK);

		val |= FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S2_TRANS);
	}

	if (master->ats_enabled)
		dst[1] |= cpu_to_le64(FIELD_PREP(STRTAB_STE_1_EATS,
						 STRTAB_STE_1_EATS_TRANS));

	arm_smmu_sync_ste_for_sid(smmu, sid);
	WRITE_ONCE(dst[0], cpu_to_le64(val));
	arm_smmu_sync_ste_for_sid(smmu, sid);

	/* It's likely that we'll want to use the new STE soon */
	if (!(smmu->options & ARM_SMMU_OPT_SKIP_PREFETCH))
		arm_smmu_cmdq_issue_cmd(smmu, &prefetch_cmd);
}

static void arm_smmu_init_bypass_stes(u64 *strtab, unsigned int nent)
{
	unsigned int i;

	for (i = 0; i < nent; ++i) {
		arm_smmu_write_strtab_ent(NULL, -1, strtab);
		strtab += STRTAB_STE_DWORDS;
	}
}

static int arm_smmu_init_l2_strtab(struct arm_smmu_device *smmu, u32 sid)
{
	size_t size;
	void *strtab;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	struct arm_smmu_strtab_l1_desc *desc = &cfg->l1_desc[sid >> STRTAB_SPLIT];

	if (desc->l2ptr)
		return 0;

	size = 1 << (STRTAB_SPLIT + ilog2(STRTAB_STE_DWORDS) + 3);
	strtab = &cfg->strtab[(sid >> STRTAB_SPLIT) * STRTAB_L1_DESC_DWORDS];

	desc->span = STRTAB_SPLIT + 1;
	desc->l2ptr = dmam_alloc_coherent(smmu->dev, size, &desc->l2ptr_dma,
					  GFP_KERNEL);
	if (!desc->l2ptr) {
		dev_err(smmu->dev,
			"failed to allocate l2 stream table for SID %u\n",
			sid);
		return -ENOMEM;
	}

	arm_smmu_init_bypass_stes(desc->l2ptr, 1 << STRTAB_SPLIT);
	arm_smmu_write_strtab_l1_desc(strtab, desc);
	return 0;
}

/* IRQ and event handlers */
static irqreturn_t arm_smmu_evtq_thread(int irq, void *dev)
{
	int i;
	struct arm_smmu_device *smmu = dev;
	struct arm_smmu_queue *q = &smmu->evtq.q;
	struct arm_smmu_ll_queue *llq = &q->llq;
	u64 evt[EVTQ_ENT_DWORDS];

	do {
		while (!queue_remove_raw(q, evt)) {
			u8 id = FIELD_GET(EVTQ_0_ID, evt[0]);

			dev_info(smmu->dev, "event 0x%02x received:\n", id);
			for (i = 0; i < ARRAY_SIZE(evt); ++i)
				dev_info(smmu->dev, "\t0x%016llx\n",
					 (unsigned long long)evt[i]);

		}

		/*
		 * Not much we can do on overflow, so scream and pretend we're
		 * trying harder.
		 */
		if (queue_sync_prod_in(q) == -EOVERFLOW)
			dev_err(smmu->dev, "EVTQ overflow detected -- events lost\n");
	} while (!queue_empty(llq));

	/* Sync our overflow flag, as we believe we're up to speed */
	llq->cons = Q_OVF(llq->prod) | Q_WRP(llq, llq->cons) |
		    Q_IDX(llq, llq->cons);
	return IRQ_HANDLED;
}

static void arm_smmu_handle_ppr(struct arm_smmu_device *smmu, u64 *evt)
{
	u32 sid, ssid;
	u16 grpid;
	bool ssv, last;

	sid = FIELD_GET(PRIQ_0_SID, evt[0]);
	ssv = FIELD_GET(PRIQ_0_SSID_V, evt[0]);
	ssid = ssv ? FIELD_GET(PRIQ_0_SSID, evt[0]) : 0;
	last = FIELD_GET(PRIQ_0_PRG_LAST, evt[0]);
	grpid = FIELD_GET(PRIQ_1_PRG_IDX, evt[1]);

	dev_info(smmu->dev, "unexpected PRI request received:\n");
	dev_info(smmu->dev,
		 "\tsid 0x%08x.0x%05x: [%u%s] %sprivileged %s%s%s access at iova 0x%016llx\n",
		 sid, ssid, grpid, last ? "L" : "",
		 evt[0] & PRIQ_0_PERM_PRIV ? "" : "un",
		 evt[0] & PRIQ_0_PERM_READ ? "R" : "",
		 evt[0] & PRIQ_0_PERM_WRITE ? "W" : "",
		 evt[0] & PRIQ_0_PERM_EXEC ? "X" : "",
		 evt[1] & PRIQ_1_ADDR_MASK);

	if (last) {
		struct arm_smmu_cmdq_ent cmd = {
			.opcode			= CMDQ_OP_PRI_RESP,
			.substream_valid	= ssv,
			.pri			= {
				.sid	= sid,
				.ssid	= ssid,
				.grpid	= grpid,
				.resp	= PRI_RESP_DENY,
			},
		};

		arm_smmu_cmdq_issue_cmd(smmu, &cmd);
	}
}

static irqreturn_t arm_smmu_priq_thread(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;
	struct arm_smmu_queue *q = &smmu->priq.q;
	struct arm_smmu_ll_queue *llq = &q->llq;
	u64 evt[PRIQ_ENT_DWORDS];

	do {
		while (!queue_remove_raw(q, evt))
			arm_smmu_handle_ppr(smmu, evt);

		if (queue_sync_prod_in(q) == -EOVERFLOW)
			dev_err(smmu->dev, "PRIQ overflow detected -- requests lost\n");
	} while (!queue_empty(llq));

	/* Sync our overflow flag, as we believe we're up to speed */
	llq->cons = Q_OVF(llq->prod) | Q_WRP(llq, llq->cons) |
		      Q_IDX(llq, llq->cons);
	queue_sync_cons_out(q);
	return IRQ_HANDLED;
}

static int arm_smmu_device_disable(struct arm_smmu_device *smmu);

static irqreturn_t arm_smmu_gerror_handler(int irq, void *dev)
{
	u32 gerror, gerrorn, active;
	struct arm_smmu_device *smmu = dev;

	gerror = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	gerrorn = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);

	active = gerror ^ gerrorn;
	if (!(active & GERROR_ERR_MASK))
		return IRQ_NONE; /* No errors pending */

	dev_warn(smmu->dev,
		 "unexpected global error reported (0x%08x), this could be serious\n",
		 active);

	if (active & GERROR_SFM_ERR) {
		dev_err(smmu->dev, "device has entered Service Failure Mode!\n");
		arm_smmu_device_disable(smmu);
	}

	if (active & GERROR_MSI_GERROR_ABT_ERR)
		dev_warn(smmu->dev, "GERROR MSI write aborted\n");

	if (active & GERROR_MSI_PRIQ_ABT_ERR)
		dev_warn(smmu->dev, "PRIQ MSI write aborted\n");

	if (active & GERROR_MSI_EVTQ_ABT_ERR)
		dev_warn(smmu->dev, "EVTQ MSI write aborted\n");

	if (active & GERROR_MSI_CMDQ_ABT_ERR)
		dev_warn(smmu->dev, "CMDQ MSI write aborted\n");

	if (active & GERROR_PRIQ_ABT_ERR)
		dev_err(smmu->dev, "PRIQ write aborted -- events may have been lost\n");

	if (active & GERROR_EVTQ_ABT_ERR)
		dev_err(smmu->dev, "EVTQ write aborted -- events may have been lost\n");

	if (active & GERROR_CMDQ_ERR)
		arm_smmu_cmdq_skip_err(smmu);

	writel(gerror, smmu->base + ARM_SMMU_GERRORN);
	return IRQ_HANDLED;
}

static irqreturn_t arm_smmu_combined_irq_thread(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;

	arm_smmu_evtq_thread(irq, dev);
	if (smmu->features & ARM_SMMU_FEAT_PRI)
		arm_smmu_priq_thread(irq, dev);

	return IRQ_HANDLED;
}

static irqreturn_t arm_smmu_combined_irq_handler(int irq, void *dev)
{
	arm_smmu_gerror_handler(irq, dev);
	return IRQ_WAKE_THREAD;
}

static void
arm_smmu_atc_inv_to_cmd(int ssid, unsigned long iova, size_t size,
			struct arm_smmu_cmdq_ent *cmd)
{
	size_t log2_span;
	size_t span_mask;
	/* ATC invalidates are always on 4096-bytes pages */
	size_t inval_grain_shift = 12;
	unsigned long page_start, page_end;

	*cmd = (struct arm_smmu_cmdq_ent) {
		.opcode			= CMDQ_OP_ATC_INV,
		.substream_valid	= !!ssid,
		.atc.ssid		= ssid,
	};

	if (!size) {
		cmd->atc.size = ATC_INV_SIZE_ALL;
		return;
	}

	page_start	= iova >> inval_grain_shift;
	page_end	= (iova + size - 1) >> inval_grain_shift;

	/*
	 * In an ATS Invalidate Request, the address must be aligned on the
	 * range size, which must be a power of two number of page sizes. We
	 * thus have to choose between grossly over-invalidating the region, or
	 * splitting the invalidation into multiple commands. For simplicity
	 * we'll go with the first solution, but should refine it in the future
	 * if multiple commands are shown to be more efficient.
	 *
	 * Find the smallest power of two that covers the range. The most
	 * significant differing bit between the start and end addresses,
	 * fls(start ^ end), indicates the required span. For example:
	 *
	 * We want to invalidate pages [8; 11]. This is already the ideal range:
	 *		x = 0b1000 ^ 0b1011 = 0b11
	 *		span = 1 << fls(x) = 4
	 *
	 * To invalidate pages [7; 10], we need to invalidate [0; 15]:
	 *		x = 0b0111 ^ 0b1010 = 0b1101
	 *		span = 1 << fls(x) = 16
	 */
	log2_span	= fls_long(page_start ^ page_end);
	span_mask	= (1ULL << log2_span) - 1;

	page_start	&= ~span_mask;

	cmd->atc.addr	= page_start << inval_grain_shift;
	cmd->atc.size	= log2_span;
}

static int arm_smmu_atc_inv_master(struct arm_smmu_master *master,
				   struct arm_smmu_cmdq_ent *cmd)
{
	int i;

	if (!master->ats_enabled)
		return 0;

	for (i = 0; i < master->num_sids; i++) {
		cmd->atc.sid = master->sids[i];
		arm_smmu_cmdq_issue_cmd(master->smmu, cmd);
	}

	return arm_smmu_cmdq_issue_sync(master->smmu);
}

static int arm_smmu_atc_inv_domain(struct arm_smmu_domain *smmu_domain,
				   int ssid, unsigned long iova, size_t size)
{
	int ret = 0;
	unsigned long flags;
	struct arm_smmu_cmdq_ent cmd;
	struct arm_smmu_master *master;

	if (!(smmu_domain->smmu->features & ARM_SMMU_FEAT_ATS))
		return 0;

	/*
	 * Ensure that we've completed prior invalidation of the main TLBs
	 * before we read 'nr_ats_masters' in case of a concurrent call to
	 * arm_smmu_enable_ats():
	 *
	 *	// unmap()			// arm_smmu_enable_ats()
	 *	TLBI+SYNC			atomic_inc(&nr_ats_masters);
	 *	smp_mb();			[...]
	 *	atomic_read(&nr_ats_masters);	pci_enable_ats() // writel()
	 *
	 * Ensures that we always see the incremented 'nr_ats_masters' count if
	 * ATS was enabled at the PCI device before completion of the TLBI.
	 */
	smp_mb();
	if (!atomic_read(&smmu_domain->nr_ats_masters))
		return 0;

	arm_smmu_atc_inv_to_cmd(ssid, iova, size, &cmd);

	spin_lock_irqsave(&smmu_domain->devices_lock, flags);
	list_for_each_entry(master, &smmu_domain->devices, domain_head)
		ret |= arm_smmu_atc_inv_master(master, &cmd);
	spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);

	return ret ? -ETIMEDOUT : 0;
}

static void arm_smmu_tlb_inv_context(void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_cmdq_ent cmd;

	cmd.opcode	= CMDQ_OP_TLBI_S12_VMALL;
	cmd.tlbi.vmid	= smmu_domain->s2_cfg.vmid;

	/*
	 * NOTE: when io-pgtable is in non-strict mode, we may get here with
	 * PTEs previously cleared by unmaps on the current CPU not yet visible
	 * to the SMMU. We are relying on the DSB implicit in
	 * queue_sync_prod_out() to guarantee those are observed before the
	 * TLBI. Do be careful, 007.
	 */
	arm_smmu_cmdq_issue_cmd(smmu, &cmd);
	arm_smmu_cmdq_issue_sync(smmu);
}

static struct iommu_domain *arm_smmu_domain_alloc(void)
{
	struct arm_smmu_domain *smmu_domain;

	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
	if (!smmu_domain)
		return NULL;

	mutex_init(&smmu_domain->init_mutex);
	INIT_LIST_HEAD(&smmu_domain->devices);
	spin_lock_init(&smmu_domain->devices_lock);

	return &smmu_domain->domain;
}

static int arm_smmu_bitmap_alloc(unsigned long *map, int span)
{
	int idx, size = 1 << span;

	do {
		idx = find_first_zero_bit(map, size);
		if (idx == size)
			return -ENOSPC;
	} while (test_and_set_bit(idx, map));

	return idx;
}

static void arm_smmu_bitmap_free(unsigned long *map, int idx)
{
	clear_bit(idx, map);
}

static void arm_smmu_domain_free(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_s2_cfg *cfg = &smmu_domain->s2_cfg;

	if (cfg->vmid)
		arm_smmu_bitmap_free(smmu->vmid_map, cfg->vmid);

	kfree(smmu_domain);
}


static int arm_smmu_domain_finalise_s2(struct arm_smmu_domain *smmu_domain,
				       struct arm_smmu_master *master)
{
	int vmid;
	struct arm_lpae_s2_cfg arm_lpae_s2_cfg;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_s2_cfg *cfg = &smmu_domain->s2_cfg;
	typeof(&arm_lpae_s2_cfg.vtcr) vtcr = &arm_lpae_s2_cfg.vtcr;

	vmid = arm_smmu_bitmap_alloc(smmu->vmid_map, smmu->vmid_bits);
	if (vmid < 0)
		return vmid;

	cfg->vmid	= (u16)vmid;
	cfg->vttbr	= arm_lpae_s2_cfg.vttbr;
	cfg->vtcr	= FIELD_PREP(STRTAB_STE_2_VTCR_S2T0SZ, vtcr->tsz) |
			  FIELD_PREP(STRTAB_STE_2_VTCR_S2SL0, vtcr->sl) |
			  FIELD_PREP(STRTAB_STE_2_VTCR_S2IR0, vtcr->irgn) |
			  FIELD_PREP(STRTAB_STE_2_VTCR_S2OR0, vtcr->orgn) |
			  FIELD_PREP(STRTAB_STE_2_VTCR_S2SH0, vtcr->sh) |
			  FIELD_PREP(STRTAB_STE_2_VTCR_S2TG, vtcr->tg) |
			  FIELD_PREP(STRTAB_STE_2_VTCR_S2PS, vtcr->ps);
	return 0;
}

static int arm_smmu_domain_finalise(struct iommu_domain *domain,
				    struct arm_smmu_master *master)
{
	int ret;
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);

	/* Restrict the stage to what we can actually support */
	smmu_domain->stage = ARM_SMMU_DOMAIN_S2;

	ret = arm_smmu_domain_finalise_s2(smmu_domain, master);
	if (ret < 0)
		return ret;

	return 0;
}

static __le64 *arm_smmu_get_step_for_sid(struct arm_smmu_device *smmu, u32 sid)
{
	__le64 *step;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		struct arm_smmu_strtab_l1_desc *l1_desc;
		int idx;

		/* Two-level walk */
		idx = (sid >> STRTAB_SPLIT) * STRTAB_L1_DESC_DWORDS;
		l1_desc = &cfg->l1_desc[idx];
		idx = (sid & ((1 << STRTAB_SPLIT) - 1)) * STRTAB_STE_DWORDS;
		step = &l1_desc->l2ptr[idx];
	} else {
		/* Simple linear lookup */
		step = &cfg->strtab[sid * STRTAB_STE_DWORDS];
	}

	return step;
}

static void arm_smmu_install_ste_for_dev(struct arm_smmu_master *master)
{
	int i, j;
	struct arm_smmu_device *smmu = master->smmu;

	for (i = 0; i < master->num_sids; ++i) {
		u32 sid = master->sids[i];
		__le64 *step = arm_smmu_get_step_for_sid(smmu, sid);

		/* Bridged PCI devices may end up with duplicated IDs */
		for (j = 0; j < i; j++)
			if (master->sids[j] == sid)
				break;
		if (j < i)
			continue;

		arm_smmu_write_strtab_ent(master, sid, step);
	}
}

static bool arm_smmu_ats_supported(struct arm_smmu_master *master)
{
	struct device *dev = master->dev;
	struct arm_smmu_device *smmu = master->smmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!(smmu->features & ARM_SMMU_FEAT_ATS))
		return false;

	if (!(fwspec->flags & IOMMU_FWSPEC_PCI_RC_ATS))
		return false;

	return dev_is_pci(dev) && pci_ats_supported(to_pci_dev(dev));
}

static void arm_smmu_enable_ats(struct arm_smmu_master *master)
{
	size_t stu;
	struct pci_dev *pdev;
	struct arm_smmu_device *smmu = master->smmu;
	struct arm_smmu_domain *smmu_domain = master->domain;

	/* Don't enable ATS at the endpoint if it's not enabled in the STE */
	if (!master->ats_enabled)
		return;

	/* Smallest Translation Unit: log2 of the smallest supported granule */
	stu = __ffs(smmu->pgsize_bitmap);
	pdev = to_pci_dev(master->dev);

	atomic_inc(&smmu_domain->nr_ats_masters);
	arm_smmu_atc_inv_domain(smmu_domain, 0, 0, 0);
	if (pci_enable_ats(pdev, stu))
		dev_err(master->dev, "Failed to enable ATS (STU %zu)\n", stu);
}

static void arm_smmu_disable_ats(struct arm_smmu_master *master)
{
	struct arm_smmu_cmdq_ent cmd;
	struct arm_smmu_domain *smmu_domain = master->domain;

	if (!master->ats_enabled)
		return;

	pci_disable_ats(to_pci_dev(master->dev));
	/*
	 * Ensure ATS is disabled at the endpoint before we issue the
	 * ATC invalidation via the SMMU.
	 */
	wmb();
	arm_smmu_atc_inv_to_cmd(0, 0, 0, &cmd);
	arm_smmu_atc_inv_master(master, &cmd);
	atomic_dec(&smmu_domain->nr_ats_masters);
}

static int arm_smmu_enable_pasid(struct arm_smmu_master *master)
{
	int ret;
	int features;
	int num_pasids;
	struct pci_dev *pdev;

	if (!dev_is_pci(master->dev))
		return -ENODEV;

	pdev = to_pci_dev(master->dev);

	features = pci_pasid_features(pdev);
	if (features < 0)
		return features;

	num_pasids = pci_max_pasids(pdev);
	if (num_pasids <= 0)
		return num_pasids;

	ret = pci_enable_pasid(pdev, features);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PASID\n");
		return ret;
	}

	return 0;
}

static void arm_smmu_disable_pasid(struct arm_smmu_master *master)
{
	struct pci_dev *pdev;

	if (!dev_is_pci(master->dev))
		return;

	pdev = to_pci_dev(master->dev);

	if (!pdev->pasid_enabled)
		return;

	pci_disable_pasid(pdev);
}

static void arm_smmu_detach_dev(struct arm_smmu_master *master)
{
	unsigned long flags;
	struct arm_smmu_domain *smmu_domain = master->domain;

	if (!smmu_domain)
		return;

	arm_smmu_disable_ats(master);

	spin_lock_irqsave(&smmu_domain->devices_lock, flags);
	list_del(&master->domain_head);
	spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);

	master->domain = NULL;
	master->ats_enabled = false;
	arm_smmu_install_ste_for_dev(master);
}

static int arm_smmu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	int ret = 0;
	unsigned long flags;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_device *smmu;
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_master *master;

	if (!fwspec)
		return -ENOENT;

	master = dev_iommu_priv_get(dev);
	smmu = master->smmu;

	arm_smmu_detach_dev(master);

	mutex_lock(&smmu_domain->init_mutex);

	if (!smmu_domain->smmu) {
		smmu_domain->smmu = smmu;
		ret = arm_smmu_domain_finalise(domain, master);
		if (ret) {
			smmu_domain->smmu = NULL;
			goto out_unlock;
		}
	} else if (smmu_domain->smmu != smmu) {
		dev_err(dev,
			"cannot attach to SMMU %s (upstream of %s)\n",
			dev_name(smmu_domain->smmu->dev),
			dev_name(smmu->dev));
		ret = -ENXIO;
		goto out_unlock;
	}

	master->domain = smmu_domain;

	if (smmu_domain->stage != ARM_SMMU_DOMAIN_BYPASS)
		master->ats_enabled = arm_smmu_ats_supported(master);

	arm_smmu_install_ste_for_dev(master);

	spin_lock_irqsave(&smmu_domain->devices_lock, flags);
	list_add(&master->domain_head, &smmu_domain->devices);
	spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);

	arm_smmu_enable_ats(master);

out_unlock:
	mutex_unlock(&smmu_domain->init_mutex);
	return ret;
}

static bool arm_smmu_sid_in_range(struct arm_smmu_device *smmu, u32 sid)
{
	unsigned long limit = smmu->strtab_cfg.num_l1_ents;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)
		limit *= 1UL << STRTAB_SPLIT;

	return sid < limit;
}

static struct iommu_device *arm_smmu_probe_device(struct device *dev)
{
	int i, ret;
	struct arm_smmu_device *smmu;
	struct arm_smmu_master *master;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!fwspec)
		return ERR_PTR(-ENODEV);

	if (WARN_ON_ONCE(dev_iommu_priv_get(dev)))
		return ERR_PTR(-EBUSY);

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);

	master->dev = dev;
	master->smmu = smmu;
	master->sids = fwspec->ids;
	master->num_sids = fwspec->num_ids;
	dev_iommu_priv_set(dev, master);

	/* Check the SIDs are in range of the SMMU and our stream table */
	for (i = 0; i < master->num_sids; i++) {
		u32 sid = master->sids[i];

		if (!arm_smmu_sid_in_range(smmu, sid)) {
			ret = -ERANGE;
			goto err_free_master;
		}

		/* Ensure l2 strtab is initialised */
		if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
			ret = arm_smmu_init_l2_strtab(smmu, sid);
			if (ret)
				goto err_free_master;
		}
	}

	/*
	 * Note that PASID must be enabled before, and disabled after ATS:
	 * PCI Express Base 4.0r1.0 - 10.5.1.3 ATS Control Register
	 *
	 *   Behavior is undefined if this bit is Set and the value of the PASID
	 *   Enable, Execute Requested Enable, or Privileged Mode Requested bits
	 *   are changed.
	 */
	arm_smmu_enable_pasid(master);

	return &smmu->iommu;

err_free_master:
	kfree(master);
	dev_iommu_priv_set(dev, NULL);
	return ERR_PTR(ret);
}

static int arm_smmu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

/* Probing and initialisation functions */
static int arm_smmu_init_one_queue(struct arm_smmu_device *smmu,
				   struct arm_smmu_queue *q,
				   unsigned long prod_off,
				   unsigned long cons_off,
				   size_t dwords, const char *name)
{
	size_t qsz;

	do {
		qsz = ((1 << q->llq.max_n_shift) * dwords) << 3;
		q->base = dmam_alloc_coherent(smmu->dev, qsz, &q->base_dma,
					      GFP_KERNEL);
		if (q->base || qsz < PAGE_SIZE)
			break;

		q->llq.max_n_shift--;
	} while (1);

	if (!q->base) {
		dev_err(smmu->dev,
			"failed to allocate queue (0x%zx bytes) for %s\n",
			qsz, name);
		return -ENOMEM;
	}

	if (!WARN_ON(q->base_dma & (qsz - 1))) {
		dev_info(smmu->dev, "allocated %u entries for %s\n",
			 1 << q->llq.max_n_shift, name);
	}

	q->prod_reg	= arm_smmu_page1_fixup(prod_off, smmu);
	q->cons_reg	= arm_smmu_page1_fixup(cons_off, smmu);
	q->ent_dwords	= dwords;

	q->q_base  = Q_BASE_RWA;
	q->q_base |= q->base_dma & Q_BASE_ADDR_MASK;
	q->q_base |= FIELD_PREP(Q_BASE_LOG2SIZE, q->llq.max_n_shift);

	q->llq.prod = q->llq.cons = 0;
	return 0;
}

static int arm_smmu_init_queues(struct arm_smmu_device *smmu)
{
	int ret;

	/* cmdq */
	spin_lock_init(&smmu->cmdq.lock);
	ret = arm_smmu_init_one_queue(smmu, &smmu->cmdq.q, ARM_SMMU_CMDQ_PROD,
				      ARM_SMMU_CMDQ_CONS, CMDQ_ENT_DWORDS,
				      "cmdq");
	if (ret)
		return ret;

	/* evtq */
	ret = arm_smmu_init_one_queue(smmu, &smmu->evtq.q, ARM_SMMU_EVTQ_PROD,
				      ARM_SMMU_EVTQ_CONS, EVTQ_ENT_DWORDS,
				      "evtq");
	if (ret)
		return ret;

	/* priq */
	if (!(smmu->features & ARM_SMMU_FEAT_PRI))
		return 0;

	return arm_smmu_init_one_queue(smmu, &smmu->priq.q, ARM_SMMU_PRIQ_PROD,
				       ARM_SMMU_PRIQ_CONS, PRIQ_ENT_DWORDS,
				       "priq");
}

static int arm_smmu_init_l1_strtab(struct arm_smmu_device *smmu)
{
	unsigned int i;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	size_t size = sizeof(*cfg->l1_desc) * cfg->num_l1_ents;
	void *strtab = smmu->strtab_cfg.strtab;

	cfg->l1_desc = devm_kzalloc(smmu->dev, size, GFP_KERNEL);
	if (!cfg->l1_desc) {
		dev_err(smmu->dev, "failed to allocate l1 stream table desc\n");
		return -ENOMEM;
	}

	for (i = 0; i < cfg->num_l1_ents; ++i) {
		arm_smmu_write_strtab_l1_desc(strtab, &cfg->l1_desc[i]);
		strtab += STRTAB_L1_DESC_DWORDS << 3;
	}

	return 0;
}

static int arm_smmu_init_strtab_2lvl(struct arm_smmu_device *smmu)
{
	void *strtab;
	u64 reg;
	u32 size, l1size;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

	/* Calculate the L1 size, capped to the SIDSIZE. */
	size = STRTAB_L1_SZ_SHIFT - (ilog2(STRTAB_L1_DESC_DWORDS) + 3);
	size = min(size, smmu->sid_bits - STRTAB_SPLIT);
	cfg->num_l1_ents = 1 << size;

	size += STRTAB_SPLIT;
	if (size < smmu->sid_bits)
		dev_warn(smmu->dev,
			 "2-level strtab only covers %u/%u bits of SID\n",
			 size, smmu->sid_bits);

	l1size = cfg->num_l1_ents * (STRTAB_L1_DESC_DWORDS << 3);
	strtab = dmam_alloc_coherent(smmu->dev, l1size, &cfg->strtab_dma,
				     GFP_KERNEL);
	if (!strtab) {
		dev_err(smmu->dev,
			"failed to allocate l1 stream table (%u bytes)\n",
			size);
		return -ENOMEM;
	}
	cfg->strtab = strtab;

	/* Configure strtab_base_cfg for 2 levels */
	reg  = FIELD_PREP(STRTAB_BASE_CFG_FMT, STRTAB_BASE_CFG_FMT_2LVL);
	reg |= FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE, size);
	reg |= FIELD_PREP(STRTAB_BASE_CFG_SPLIT, STRTAB_SPLIT);
	cfg->strtab_base_cfg = reg;

	return arm_smmu_init_l1_strtab(smmu);
}

static int arm_smmu_init_strtab_linear(struct arm_smmu_device *smmu)
{
	void *strtab;
	u64 reg;
	u32 size;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

	size = (1 << smmu->sid_bits) * (STRTAB_STE_DWORDS << 3);
	strtab = dmam_alloc_coherent(smmu->dev, size, &cfg->strtab_dma,
				     GFP_KERNEL);
	if (!strtab) {
		dev_err(smmu->dev,
			"failed to allocate linear stream table (%u bytes)\n",
			size);
		return -ENOMEM;
	}
	cfg->strtab = strtab;
	cfg->num_l1_ents = 1 << smmu->sid_bits;

	/* Configure strtab_base_cfg for a linear table covering all SIDs */
	reg  = FIELD_PREP(STRTAB_BASE_CFG_FMT, STRTAB_BASE_CFG_FMT_LINEAR);
	reg |= FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE, smmu->sid_bits);
	cfg->strtab_base_cfg = reg;

	arm_smmu_init_bypass_stes(strtab, cfg->num_l1_ents);
	return 0;
}

static int arm_smmu_init_strtab(struct arm_smmu_device *smmu)
{
	u64 reg;
	int ret;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)
		ret = arm_smmu_init_strtab_2lvl(smmu);
	else
		ret = arm_smmu_init_strtab_linear(smmu);

	if (ret)
		return ret;

	/* Set the strtab base address */
	reg  = smmu->strtab_cfg.strtab_dma & STRTAB_BASE_ADDR_MASK;
	reg |= STRTAB_BASE_RA;
	smmu->strtab_cfg.strtab_base = reg;

	/* Allocate the first VMID for stage-2 bypass STEs */
	set_bit(0, smmu->vmid_map);
	return 0;
}

static int arm_smmu_init_structures(struct arm_smmu_device *smmu)
{
	int ret;

	ret = arm_smmu_init_queues(smmu);
	if (ret)
		return ret;

	return arm_smmu_init_strtab(smmu);
}

static int arm_smmu_write_reg_sync(struct arm_smmu_device *smmu, u32 val,
				   unsigned int reg_off, unsigned int ack_off)
{
	u32 reg;

	writel_relaxed(val, smmu->base + reg_off);
	return readl_relaxed_poll_timeout(smmu->base + ack_off, reg, reg == val,
					  1, ARM_SMMU_POLL_TIMEOUT_US);
}

/* GBPA is "special" */
static int arm_smmu_update_gbpa(struct arm_smmu_device *smmu, u32 set, u32 clr)
{
	int ret;
	u32 reg, __iomem *gbpa = smmu->base + ARM_SMMU_GBPA;

	ret = readl_relaxed_poll_timeout(gbpa, reg, !(reg & GBPA_UPDATE),
					 1, ARM_SMMU_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	reg &= ~clr;
	reg |= set;
	writel_relaxed(reg | GBPA_UPDATE, gbpa);
	ret = readl_relaxed_poll_timeout(gbpa, reg, !(reg & GBPA_UPDATE),
					 1, ARM_SMMU_POLL_TIMEOUT_US);

	if (ret)
		dev_err(smmu->dev, "GBPA not responding to update\n");
	return ret;
}

static void arm_smmu_free_msis(void *data)
{
	struct device *dev = data;
	platform_msi_domain_free_irqs(dev);
}

static void arm_smmu_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	phys_addr_t doorbell;
	struct device *dev = msi_desc_to_dev(desc);
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	phys_addr_t *cfg = arm_smmu_msi_cfg[desc->platform.msi_index];

	doorbell = (((u64)msg->address_hi) << 32) | msg->address_lo;
	doorbell &= MSI_CFG0_ADDR_MASK;

	writeq_relaxed(doorbell, smmu->base + cfg[0]);
	writel_relaxed(msg->data, smmu->base + cfg[1]);
	writel_relaxed(ARM_SMMU_MEMATTR_DEVICE_nGnRE, smmu->base + cfg[2]);
}

static void arm_smmu_setup_msis(struct arm_smmu_device *smmu)
{
	struct msi_desc *desc;
	int ret, nvec = ARM_SMMU_MAX_MSIS;
	struct device *dev = smmu->dev;

	/* Clear the MSI address regs */
	writeq_relaxed(0, smmu->base + ARM_SMMU_GERROR_IRQ_CFG0);
	writeq_relaxed(0, smmu->base + ARM_SMMU_EVTQ_IRQ_CFG0);

	if (smmu->features & ARM_SMMU_FEAT_PRI)
		writeq_relaxed(0, smmu->base + ARM_SMMU_PRIQ_IRQ_CFG0);
	else
		nvec--;

	if (!(smmu->features & ARM_SMMU_FEAT_MSI))
		return;

	if (!dev->msi_domain) {
		dev_info(smmu->dev, "msi_domain absent - falling back to wired irqs\n");
		return;
	}

	/* Allocate MSIs for evtq, gerror and priq. Ignore cmdq */
	ret = platform_msi_domain_alloc_irqs(dev, nvec, arm_smmu_write_msi_msg);
	if (ret) {
		dev_warn(dev, "failed to allocate MSIs - falling back to wired irqs\n");
		return;
	}

	for_each_msi_entry(desc, dev) {
		switch (desc->platform.msi_index) {
		case EVTQ_MSI_INDEX:
			smmu->evtq.q.irq = desc->irq;
			break;
		case GERROR_MSI_INDEX:
			smmu->gerr_irq = desc->irq;
			break;
		case PRIQ_MSI_INDEX:
			smmu->priq.q.irq = desc->irq;
			break;
		default:	/* Unknown */
			continue;
		}
	}

	/* Add callback to free MSIs on teardown */
	devm_add_action(dev, arm_smmu_free_msis, dev);
}

static void arm_smmu_setup_unique_irqs(struct arm_smmu_device *smmu)
{
	int irq, ret;

	arm_smmu_setup_msis(smmu);

	/* Request interrupt lines */
	irq = smmu->evtq.q.irq;
	if (irq) {
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
						arm_smmu_evtq_thread,
						IRQF_ONESHOT,
						"arm-smmu-v3-evtq", smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable evtq irq\n");
	} else {
		dev_warn(smmu->dev, "no evtq irq - events will not be reported!\n");
	}

	irq = smmu->gerr_irq;
	if (irq) {
		ret = devm_request_irq(smmu->dev, irq, arm_smmu_gerror_handler,
				       0, "arm-smmu-v3-gerror", smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable gerror irq\n");
	} else {
		dev_warn(smmu->dev, "no gerr irq - errors will not be reported!\n");
	}

	if (smmu->features & ARM_SMMU_FEAT_PRI) {
		irq = smmu->priq.q.irq;
		if (irq) {
			ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
							arm_smmu_priq_thread,
							IRQF_ONESHOT,
							"arm-smmu-v3-priq",
							smmu);
			if (ret < 0)
				dev_warn(smmu->dev,
					 "failed to enable priq irq\n");
		} else {
			dev_warn(smmu->dev, "no priq irq - PRI will be broken\n");
		}
	}
}

static int arm_smmu_setup_irqs(struct arm_smmu_device *smmu)
{
	int ret, irq;
	u32 irqen_flags = IRQ_CTRL_EVTQ_IRQEN | IRQ_CTRL_GERROR_IRQEN;

	/* Disable IRQs first */
	ret = arm_smmu_write_reg_sync(smmu, 0, ARM_SMMU_IRQ_CTRL,
				      ARM_SMMU_IRQ_CTRLACK);
	if (ret) {
		dev_err(smmu->dev, "failed to disable irqs\n");
		return ret;
	}

	irq = smmu->combined_irq;
	if (irq) {
		/*
		 * Cavium ThunderX2 implementation doesn't support unique irq
		 * lines. Use a single irq line for all the SMMUv3 interrupts.
		 */
		ret = devm_request_threaded_irq(smmu->dev, irq,
					arm_smmu_combined_irq_handler,
					arm_smmu_combined_irq_thread,
					IRQF_ONESHOT,
					"arm-smmu-v3-combined-irq", smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable combined irq\n");
	} else
		arm_smmu_setup_unique_irqs(smmu);

	if (smmu->features & ARM_SMMU_FEAT_PRI)
		irqen_flags |= IRQ_CTRL_PRIQ_IRQEN;

	/* Enable interrupt generation on the SMMU */
	ret = arm_smmu_write_reg_sync(smmu, irqen_flags,
				      ARM_SMMU_IRQ_CTRL, ARM_SMMU_IRQ_CTRLACK);
	if (ret)
		dev_warn(smmu->dev, "failed to enable irqs\n");

	return 0;
}

static int arm_smmu_device_disable(struct arm_smmu_device *smmu)
{
	int ret;

	ret = arm_smmu_write_reg_sync(smmu, 0, ARM_SMMU_CR0, ARM_SMMU_CR0ACK);
	if (ret)
		dev_err(smmu->dev, "failed to clear cr0\n");

	return ret;
}

static int arm_smmu_device_reset(struct arm_smmu_device *smmu, bool bypass)
{
	int ret;
	u32 reg, enables;
	struct arm_smmu_cmdq_ent cmd;

	/* Clear CR0 and sync (disables SMMU and queue processing) */
	reg = readl_relaxed(smmu->base + ARM_SMMU_CR0);
	if (reg & CR0_SMMUEN) {
		dev_warn(smmu->dev, "SMMU currently enabled! Resetting...\n");
		WARN_ON(is_kdump_kernel() && !disable_bypass);
		arm_smmu_update_gbpa(smmu, GBPA_ABORT, 0);
	}

	ret = arm_smmu_device_disable(smmu);
	if (ret)
		return ret;

	/* CR1 (table and queue memory attributes) */
	reg = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
	writel_relaxed(reg, smmu->base + ARM_SMMU_CR1);

	/* CR2 (random crap) */
	reg = CR2_PTM | CR2_RECINVSID | CR2_E2H;
	writel_relaxed(reg, smmu->base + ARM_SMMU_CR2);

	/* Stream table */
	writeq_relaxed(smmu->strtab_cfg.strtab_base,
		       smmu->base + ARM_SMMU_STRTAB_BASE);
	writel_relaxed(smmu->strtab_cfg.strtab_base_cfg,
		       smmu->base + ARM_SMMU_STRTAB_BASE_CFG);

	/* Command queue */
	writeq_relaxed(smmu->cmdq.q.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);
	writel_relaxed(smmu->cmdq.q.llq.prod, smmu->base + ARM_SMMU_CMDQ_PROD);
	writel_relaxed(smmu->cmdq.q.llq.cons, smmu->base + ARM_SMMU_CMDQ_CONS);

	enables = CR0_CMDQEN;
	ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,
				      ARM_SMMU_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to enable command queue\n");
		return ret;
	}

	/* Invalidate any cached configuration */
	cmd.opcode = CMDQ_OP_CFGI_ALL;
	arm_smmu_cmdq_issue_cmd(smmu, &cmd);
	arm_smmu_cmdq_issue_sync(smmu);

	/* Invalidate any stale TLB entries */
	if (smmu->features & ARM_SMMU_FEAT_HYP) {
		cmd.opcode = CMDQ_OP_TLBI_EL2_ALL;
		arm_smmu_cmdq_issue_cmd(smmu, &cmd);
	}

	cmd.opcode = CMDQ_OP_TLBI_NSNH_ALL;
	arm_smmu_cmdq_issue_cmd(smmu, &cmd);
	arm_smmu_cmdq_issue_sync(smmu);

	/* Event queue */
	writeq_relaxed(smmu->evtq.q.q_base, smmu->base + ARM_SMMU_EVTQ_BASE);
	writel_relaxed(smmu->evtq.q.llq.prod,
		       arm_smmu_page1_fixup(ARM_SMMU_EVTQ_PROD, smmu));
	writel_relaxed(smmu->evtq.q.llq.cons,
		       arm_smmu_page1_fixup(ARM_SMMU_EVTQ_CONS, smmu));

	enables |= CR0_EVTQEN;
	ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,
				      ARM_SMMU_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to enable event queue\n");
		return ret;
	}

	/* PRI queue */
	if (smmu->features & ARM_SMMU_FEAT_PRI) {
		writeq_relaxed(smmu->priq.q.q_base,
			       smmu->base + ARM_SMMU_PRIQ_BASE);
		writel_relaxed(smmu->priq.q.llq.prod,
			       arm_smmu_page1_fixup(ARM_SMMU_PRIQ_PROD, smmu));
		writel_relaxed(smmu->priq.q.llq.cons,
			       arm_smmu_page1_fixup(ARM_SMMU_PRIQ_CONS, smmu));

		enables |= CR0_PRIQEN;
		ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,
					      ARM_SMMU_CR0ACK);
		if (ret) {
			dev_err(smmu->dev, "failed to enable PRI queue\n");
			return ret;
		}
	}

	if (smmu->features & ARM_SMMU_FEAT_ATS) {
		enables |= CR0_ATSCHK;
		ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,
					      ARM_SMMU_CR0ACK);
		if (ret) {
			dev_err(smmu->dev, "failed to enable ATS check\n");
			return ret;
		}
	}

	ret = arm_smmu_setup_irqs(smmu);
	if (ret) {
		dev_err(smmu->dev, "failed to setup irqs\n");
		return ret;
	}

	if (is_kdump_kernel())
		enables &= ~(CR0_EVTQEN | CR0_PRIQEN);

	/* Enable the SMMU interface, or ensure bypass */
	if (!bypass || disable_bypass) {
		enables |= CR0_SMMUEN;
	} else {
		ret = arm_smmu_update_gbpa(smmu, 0, GBPA_ABORT);
		if (ret)
			return ret;
	}
	ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,
				      ARM_SMMU_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to enable SMMU interface\n");
		return ret;
	}

	return 0;
}

static int arm_smmu_device_hw_probe(struct arm_smmu_device *smmu)
{
	u32 reg;
	bool coherent = smmu->features & ARM_SMMU_FEAT_COHERENCY;

	/* IDR0 */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR0);

	/* 2-level structures */
	if (FIELD_GET(IDR0_ST_LVL, reg) == IDR0_ST_LVL_2LVL)
		smmu->features |= ARM_SMMU_FEAT_2_LVL_STRTAB;

	if (reg & IDR0_CD2L)
		smmu->features |= ARM_SMMU_FEAT_2_LVL_CDTAB;

	/*
	 * Translation table endianness.
	 * We currently require the same endianness as the CPU, but this
	 * could be changed later by adding a new IO_PGTABLE_QUIRK.
	 */
	switch (FIELD_GET(IDR0_TTENDIAN, reg)) {
	case IDR0_TTENDIAN_MIXED:
		smmu->features |= ARM_SMMU_FEAT_TT_LE | ARM_SMMU_FEAT_TT_BE;
		break;
#ifdef __BIG_ENDIAN
	case IDR0_TTENDIAN_BE:
		smmu->features |= ARM_SMMU_FEAT_TT_BE;
		break;
#else
	case IDR0_TTENDIAN_LE:
		smmu->features |= ARM_SMMU_FEAT_TT_LE;
		break;
#endif
	default:
		dev_err(smmu->dev, "unknown/unsupported TT endianness!\n");
		return -ENXIO;
	}

	/* Boolean feature flags */
	if (IS_ENABLED(CONFIG_PCI_PRI) && reg & IDR0_PRI)
		smmu->features |= ARM_SMMU_FEAT_PRI;

	if (IS_ENABLED(CONFIG_PCI_ATS) && reg & IDR0_ATS)
		smmu->features |= ARM_SMMU_FEAT_ATS;

	if (reg & IDR0_SEV)
		smmu->features |= ARM_SMMU_FEAT_SEV;

	if (reg & IDR0_MSI)
		smmu->features |= ARM_SMMU_FEAT_MSI;

	if (reg & IDR0_HYP)
		smmu->features |= ARM_SMMU_FEAT_HYP;

	/*
	 * The coherency feature as set by FW is used in preference to the ID
	 * register, but warn on mismatch.
	 */
	if (!!(reg & IDR0_COHACC) != coherent)
		dev_warn(smmu->dev, "IDR0.COHACC overridden by FW configuration (%s)\n",
			 coherent ? "true" : "false");

	switch (FIELD_GET(IDR0_STALL_MODEL, reg)) {
	case IDR0_STALL_MODEL_FORCE:
		smmu->features |= ARM_SMMU_FEAT_STALL_FORCE;
		fallthrough;
	case IDR0_STALL_MODEL_STALL:
		smmu->features |= ARM_SMMU_FEAT_STALLS;
	}

	if (reg & IDR0_S2P)
		smmu->features |= ARM_SMMU_FEAT_TRANS_S2;

	if (!(reg & IDR0_S2P)) {
		dev_err(smmu->dev, "no translation support!\n");
		return -ENXIO;
	}

	/* We only support the AArch64 table format at present */
	switch (FIELD_GET(IDR0_TTF, reg)) {
	case IDR0_TTF_AARCH32_64:
		smmu->ias = 40;
		fallthrough;
	case IDR0_TTF_AARCH64:
		break;
	default:
		dev_err(smmu->dev, "AArch64 table format not supported!\n");
		return -ENXIO;
	}

	/* ASID/VMID sizes */
	smmu->vmid_bits = reg & IDR0_VMID16 ? 16 : 8;

	/* IDR1 */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR1);
	if (reg & (IDR1_TABLES_PRESET | IDR1_QUEUES_PRESET | IDR1_REL)) {
		dev_err(smmu->dev, "embedded implementation not supported\n");
		return -ENXIO;
	}

	/* Queue sizes, capped to ensure natural alignment */
	smmu->cmdq.q.llq.max_n_shift = min_t(u32, CMDQ_MAX_SZ_SHIFT,
					     FIELD_GET(IDR1_CMDQS, reg));
	if (!smmu->cmdq.q.llq.max_n_shift) {
		/* Odd alignment restrictions on the base, so ignore for now */
		dev_err(smmu->dev, "unit-length command queue not supported\n");
		return -ENXIO;
	}

	smmu->evtq.q.llq.max_n_shift = min_t(u32, EVTQ_MAX_SZ_SHIFT,
					     FIELD_GET(IDR1_EVTQS, reg));
	smmu->priq.q.llq.max_n_shift = min_t(u32, PRIQ_MAX_SZ_SHIFT,
					     FIELD_GET(IDR1_PRIQS, reg));

	/* SID/SSID sizes */
	smmu->sid_bits = FIELD_GET(IDR1_SIDSIZE, reg);

	/*
	 * If the SMMU supports fewer bits than would fill a single L2 stream
	 * table, use a linear table instead.
	 */
	if (smmu->sid_bits <= STRTAB_SPLIT)
		smmu->features &= ~ARM_SMMU_FEAT_2_LVL_STRTAB;

	/* IDR5 */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR5);

	/* Maximum number of outstanding stalls */
	smmu->evtq.max_stalls = FIELD_GET(IDR5_STALL_MAX, reg);

	/* Page sizes */
	if (reg & IDR5_GRAN64K)
		smmu->pgsize_bitmap |= SZ_64K | SZ_512M;
	if (reg & IDR5_GRAN16K)
		smmu->pgsize_bitmap |= SZ_16K | SZ_32M;
	if (reg & IDR5_GRAN4K)
		smmu->pgsize_bitmap |= SZ_4K | SZ_2M | SZ_1G;

	/* Input address size */
	if (FIELD_GET(IDR5_VAX, reg) == IDR5_VAX_52_BIT)
		smmu->features |= ARM_SMMU_FEAT_VAX;

	/* Output address size */
	switch (FIELD_GET(IDR5_OAS, reg)) {
	case IDR5_OAS_32_BIT:
		smmu->oas = 32;
		break;
	case IDR5_OAS_36_BIT:
		smmu->oas = 36;
		break;
	case IDR5_OAS_40_BIT:
		smmu->oas = 40;
		break;
	case IDR5_OAS_42_BIT:
		smmu->oas = 42;
		break;
	case IDR5_OAS_44_BIT:
		smmu->oas = 44;
		break;
	case IDR5_OAS_52_BIT:
		smmu->oas = 52;
		smmu->pgsize_bitmap |= 1ULL << 42; /* 4TB */
		break;
	default:
		dev_info(smmu->dev,
			"unknown output address size. Truncating to 48-bit\n");
		fallthrough;
	case IDR5_OAS_48_BIT:
		smmu->oas = 48;
	}

	smmu->ias = max(smmu->ias, smmu->oas);

	dev_info(smmu->dev, "ias %lu-bit, oas %lu-bit (features 0x%08x)\n",
		 smmu->ias, smmu->oas, smmu->features);
	return 0;
}

#ifdef CONFIG_ACPI
static void acpi_smmu_get_options(u32 model, struct arm_smmu_device *smmu)
{
	switch (model) {
	case ACPI_IORT_SMMU_V3_CAVIUM_CN99XX:
		smmu->options |= ARM_SMMU_OPT_PAGE0_REGS_ONLY;
		break;
	case ACPI_IORT_SMMU_V3_HISILICON_HI161X:
		smmu->options |= ARM_SMMU_OPT_SKIP_PREFETCH;
		break;
	}

	dev_notice(smmu->dev, "option mask 0x%x\n", smmu->options);
}

static int arm_smmu_device_acpi_probe(struct platform_device *pdev,
				      struct arm_smmu_device *smmu)
{
	struct acpi_iort_smmu_v3 *iort_smmu;
	struct device *dev = smmu->dev;
	struct acpi_iort_node *node;

	node = *(struct acpi_iort_node **)dev_get_platdata(dev);

	/* Retrieve SMMUv3 specific data */
	iort_smmu = (struct acpi_iort_smmu_v3 *)node->node_data;

	acpi_smmu_get_options(iort_smmu->model, smmu);

	if (iort_smmu->flags & ACPI_IORT_SMMU_V3_COHACC_OVERRIDE)
		smmu->features |= ARM_SMMU_FEAT_COHERENCY;

	return 0;
}
#else
static inline int arm_smmu_device_acpi_probe(struct platform_device *pdev,
					     struct arm_smmu_device *smmu)
{
	return -ENODEV;
}
#endif

static int arm_smmu_device_dt_probe(struct platform_device *pdev,
				    struct arm_smmu_device *smmu)
{
	struct device *dev = &pdev->dev;
	u32 cells;
	int ret = -EINVAL;

	if (of_property_read_u32(dev->of_node, "#iommu-cells", &cells))
		dev_err(dev, "missing #iommu-cells property\n");
	else if (cells != 1)
		dev_err(dev, "invalid #iommu-cells value (%d)\n", cells);
	else
		ret = 0;

	parse_driver_options(smmu);

	if (of_dma_is_coherent(dev->of_node))
		smmu->features |= ARM_SMMU_FEAT_COHERENCY;

	return ret;
}

static unsigned long arm_smmu_resource_size(struct arm_smmu_device *smmu)
{
	if (smmu->options & ARM_SMMU_OPT_PAGE0_REGS_ONLY)
		return SZ_64K;
	else
		return SZ_128K;
}

static void __iomem *arm_smmu_ioremap(struct device *dev, resource_size_t start,
				      resource_size_t size)
{
	struct resource res = {
		.flags = IORESOURCE_MEM,
		.start = start,
		.end = start + size - 1,
	};

	return devm_ioremap_resource(dev, &res);
}

static int arm_smmu_device_probe(struct platform_device *pdev)
{
	int irq, ret;
	struct resource *res;
	resource_size_t ioaddr;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	bool bypass;

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);
	if (!smmu) {
		dev_err(dev, "failed to allocate arm_smmu_device\n");
		return -ENOMEM;
	}
	smmu->dev = dev;

	if (dev->of_node) {
		ret = arm_smmu_device_dt_probe(pdev, smmu);
	} else {
		ret = arm_smmu_device_acpi_probe(pdev, smmu);
		if (ret == -ENODEV)
			return ret;
	}

	/* Set bypass mode according to firmware probing result */
	bypass = !!ret;

	/* Base address */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (resource_size(res) < arm_smmu_resource_size(smmu)) {
		dev_err(dev, "MMIO region too small (%pr)\n", res);
		return -EINVAL;
	}
	ioaddr = res->start;

	/*
	 * Don't map the IMPLEMENTATION DEFINED regions, since they may contain
	 * the PMCG registers which are reserved by the PMU driver.
	 */
	smmu->base = arm_smmu_ioremap(dev, ioaddr, ARM_SMMU_REG_SZ);
	if (IS_ERR(smmu->base))
		return PTR_ERR(smmu->base);

	if (arm_smmu_resource_size(smmu) > SZ_64K) {
		smmu->page1 = arm_smmu_ioremap(dev, ioaddr + SZ_64K,
					       ARM_SMMU_REG_SZ);
		if (IS_ERR(smmu->page1))
			return PTR_ERR(smmu->page1);
	} else {
		smmu->page1 = smmu->base;
	}

	/* Interrupt lines */

	irq = platform_get_irq_byname_optional(pdev, "combined");
	if (irq > 0)
		smmu->combined_irq = irq;
	else {
		irq = platform_get_irq_byname_optional(pdev, "eventq");
		if (irq > 0)
			smmu->evtq.q.irq = irq;

		irq = platform_get_irq_byname_optional(pdev, "priq");
		if (irq > 0)
			smmu->priq.q.irq = irq;

		irq = platform_get_irq_byname_optional(pdev, "gerror");
		if (irq > 0)
			smmu->gerr_irq = irq;
	}
	/* Probe the h/w */
	ret = arm_smmu_device_hw_probe(smmu);
	if (ret)
		return ret;

	/* Initialise in-memory data structures */
	ret = arm_smmu_init_structures(smmu);
	if (ret)
		return ret;

	/* Reset the device */
	ret = arm_smmu_device_reset(smmu, bypass);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};