#pragma once
/*
 * Per-phone constants. build.sh passes -DTARGET_VAYU for the Poco X3 Pro;
 * the default is topaz (Redmi Note 12 4G).
 */

#if defined(TARGET_VAYU)
/*
 * vayu: Poco X3 Pro, SM8150-AC (msm-id 0x169), stock MIUI 14 base DTB
 * "SM8150P v2" + dtbo "VAYU" (board-id 0x2f). Same MMIO map as sweet's
 * SM7150 for GIC, UFS, PS_HOLD and the splash.
 */
#define TARGET_NAME		"vayu"
#define IMEM_RESTART_REASON	0x146bf65cUL	/* qcom,msm-imem@146bf000 restart_reason@65c */
#define PSHOLD			0x0c264000UL	/* restart@c264000 pshold-base */
#define GICD_BASE		0x17a00000UL	/* interrupt-controller@17a00000 */
#define GICR_BASE		0x17a60000UL	/* redistributor-stride 0x20000 */
#define FB_BASE			0x9c000000UL	/* reserved-memory cont_splash_region@9c000000 */
#define FB_REGION_SIZE		0x02400000UL
/* MDSS (mdss_mdp@ae00000): ABL's splash pipe is one of these SSPPs */
#define MDSS_BASE		0x0ae00000UL
/* cpufreq-hw domains (osm pwrcl / perfcl / perfpcl): A55, A76 gold, A76 prime */
#define CPUFREQ_DOMAINS		{ 0x18323000UL, 0x18325800UL, 0x18327800UL }
/* up to 40 LUT rows (silver has ~18 levels; 12 stopped it at 1305 MHz on the phone) */
#define CPUFREQ_LUT_ROWS	40
/* DynamIQ: MPIDR Aff1 = core. 0x000-0x300 A55, 0x400-0x600 A76, 0x700 A76 prime */
static inline unsigned long cpu_mpidr(int i) { return (unsigned long)i << 8; }
/* Clamp the kernel's RAM to the /memory ranges ABL writes into the DTB */
#define MEM_FROM_FDT		1

#else
#define TARGET_TOPAZ		1
#define TARGET_NAME		"topaz"
#define IMEM_RESTART_REASON	0x0c12565cUL	/* qcom,msm-imem@c125000 restart_reason@65c */
#define PSHOLD			0x0440b000UL
#define GICD_BASE		0x0f200000UL
#define GICR_BASE		0x0f300000UL
#define FB_BASE			0x5c000000UL	/* reserved-memory cont_splash_region */
#define FB_REGION_SIZE		0x00f00000UL
#define CPUFREQ_DOMAINS		{ 0x0f523000UL }	/* gold freq-domain1 */
#define CPUFREQ_LUT_ROWS	12
/* CPU0-3 (A53) at MPIDR 0x0-0x3, CPU4-7 (A73) at 0x100-0x103 */
static inline unsigned long cpu_mpidr(int i) { return (unsigned long)(i >> 2) << 8 | (i & 3); }
#endif
