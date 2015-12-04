
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "register.h"
#include "aarch64.h"
#include "target_type64.h"
#include "armv8_opcodes.h"
#include "armv8_cti.h"


extern int aarch64_exec_opcode(struct target *target,
	uint32_t opcode, uint32_t *edscr_p);




void print_edscr_detail(struct command_context *cmd_ctx, uint32_t edscr)
{
	struct target *target = get_current_target(cmd_ctx);
	enum target_debug_reason backup_debug_reason = target->debug_reason;
	target->debug_reason = armv8_edscr_debug_reason(edscr);

	command_print(cmd_ctx,
		"EDSCR = 0x%.8"PRIx32 "\n"
		"\tRXFull = %d                    TXFull = %d\n"
		"\tITO (ITR Overrun) = %d         ITE (ITR empty) = %d\n"
        "\tRXO (RX Overrun) = %d          TXU (TX Underrun) = %d\n"
		"\tTDA (trap dbg reg acc) = %d    MA (mem access mode) = %d\n"
		"\tNS (Non-Secure) = %d           SDD (Secure debug disable) = %d\n"
		"\tRW =0x%03x                     EL = %d\n"
		"\tA (sys error) = %d             ERR = %d\n"
		"\tHDE (halting debug) = %d       STATUS = 0x%03x (%s)\n"
		"\tPIPEADV (Pipeline advance) = %d\n",
		edscr,
		(edscr & ARMV8_EDSCR_RXFULL) ? 1 : 0,
		(edscr & ARMV8_EDSCR_TXFULL) ? 1 : 0,
		(edscr & ARMV8_EDSCR_ITO) ? 1 : 0,
		(edscr & ARMV8_EDSCR_ITE) ? 1 : 0,
		(edscr & ARMV8_EDSCR_RXO) ? 1 : 0,
		(edscr & ARMV8_EDSCR_TXU) ? 1 : 0,
		(edscr & ARMV8_EDSCR_TDA) ? 1 : 0,
		(edscr & ARMV8_EDSCR_MA) ? 1 : 0,
		(edscr & ARMV8_EDSCR_NS) ? 1 : 0,
		(edscr & ARMV8_EDSCR_SDD) ? 1 : 0,
		EDSCR_RW(edscr), EDSCR_EL(edscr),
		(edscr & ARMV8_EDSCR_A) ? 1 : 0,
		(edscr & ARMV8_EDSCR_ERR) ? 1 : 0,
		(edscr & ARMV8_EDSCR_HDE) ? 1 : 0, EDSCR_STATUS(edscr), debug_reason_name(target),
		(edscr & ARMV8_EDSCR_PIPEADV) ? 1: 0
		);

	target->debug_reason = backup_debug_reason;
}

void print_edesr_detail(struct command_context *cmd_ctx, uint32_t edesr)
{
	command_print(cmd_ctx,
		"EDESR = 0x%.8"PRIx32 "\n"
        "\tSS (Halting step debug event pending) = %d\n"
        "\tRC (Reset catch debug event pending) = %d\n"
        "\tOSUC (OS unlock debug event pending) = %d\n",
		edesr,
		(edesr & ARMV8_EDESR_SS) ? 1 : 0,
		(edesr & ARMV8_EDESR_RC) ? 1 : 0,
		(edesr & ARMV8_EDESR_OSUC) ? 1 : 0
		);
}

void print_edprsr_detail(struct command_context *cmd_ctx, uint32_t edprsr)
{
	command_print(cmd_ctx,
		"EDPRSR = 0x%.8"PRIx32 "\n"
        "\tHALTED = %d                    SDR (Sticky debug restart) = %d\n"
		"\tSPMAD (Stidky EPMAD err) = %d  EPMAD (Ext PM access disable) = %d\n"
		"\tSDAD (Sticky EDAD err) = %d    EDAD (Ext Debug disable) = %d\n"
		"\tDLK (Double lock) = %d         OSLK = %d\n"
		"\tSR (Sticky reset) = %d         R = %d\n"
		"\tSPD (Sticky Powerdown) = %d    PU = %d\n",
		edprsr,
		(edprsr & ARMV8_EDPRSR_HALTED) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_SDR) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_SPMAD) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_EPMAD) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_SDAD) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_EDAD) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_DLK) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_OSLK) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_SR) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_R) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_SPD) ? 1 : 0,
		(edprsr & ARMV8_EDPRSR_PU) ? 1 : 0
		);
}

void print_target_debug_info_status(struct command_context *cmd_ctx, struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *dap = armv8->arm.dap;
	int rc = ERROR_FAIL;
	uint32_t edprsr, edesr;

	command_print(cmd_ctx, "----- Target %s -----", target_name(target));

	dap_ap_select(dap, armv8->debug_ap);

	print_edscr_detail(cmd_ctx, aarch64->cpudbg_edscr);

	rc = mem_ap_read_atomic_u32(dap,
			armv8->debug_base + ARMV8_REG_EDESR, &edesr);
	if (rc == ERROR_OK) {
		print_edesr_detail(cmd_ctx, edesr);
	}

	rc = mem_ap_read_atomic_u32(dap,
			armv8->debug_base + ARMV8_REG_EDPRSR, &edprsr);
	if (rc == ERROR_OK) {
		print_edprsr_detail(cmd_ctx, edprsr);
	}
}

/* ---------------------------------------- */
int print_target_debug_info_cti(
	struct command_context *cmd_ctx,
	struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *dap = armv8->arm.dap;
	int rc = ERROR_FAIL;
	uint32_t cti_base = armv8->debug_base + ARMV8_CTI_BASE_OFST;
	uint32_t cti_devid, cti_devtype;
	uint32_t cti_control, cti_appset, cti_gate, cti_asicctl;
	uint32_t cti_inen[8], cti_outen[8];
	uint32_t cti_triginstatus, cti_trigoutstatus;
	uint32_t cti_chinstatus, cti_choutstatus;
	/* WO: CTI_INTACK, CTI_APPCLEAR, CTI_APPPULSE
	 */

	assert(dap != NULL);
//	uint32_t cti_authstatus, /* FB8 */


	/* We need at least 3 channels (DEBUG, RESTART, IRQ) to work */
	rc = mem_ap_read_atomic_u32(dap, cti_base + CS_REG_DEVID, &cti_devid);
	if (rc == ERROR_OK) {
		command_print(cmd_ctx, "CTI DevID = 0x%.8" PRIx32 ", Num Channel=%d, Num Trigger=%d",
			cti_devid,
			ARMV8_CTIDEVID_NUMCHAN(cti_devid),
			ARMV8_CTIDEVID_NUMTRIG(cti_devid)
		);
	}

	rc = mem_ap_read_atomic_u32(dap, cti_base + CS_REG_DEVTYPE, &cti_devtype);
	if (rc == ERROR_OK) {
		command_print(cmd_ctx, "\tCTI type Major/Sub = 0x%x/0x%x",
			(cti_devtype & 0xf),
			(cti_devtype >> 4) & 0xf
		);
	}

	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_CONTROL,	&cti_control);
	if (rc != ERROR_OK)	goto err;
	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_GATE,	&cti_gate);
	if (rc != ERROR_OK)	goto err;
	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_ASICCTL,	&cti_asicctl);
	if (rc != ERROR_OK)	goto err;
	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_APPSET,	&cti_appset);
	if (rc != ERROR_OK)	goto err;
	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_TRIGINSTATUS,	&cti_triginstatus);
	if (rc != ERROR_OK)	goto err;
	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_TRIGOUTSTATUS,	&cti_trigoutstatus);
	if (rc != ERROR_OK)	goto err;
	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_CHINSTATUS,	&cti_chinstatus);
	if (rc != ERROR_OK)	goto err;
	rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_CHOUTSTATUS,	&cti_choutstatus);
	if (rc != ERROR_OK)	goto err;
	int i;
	for (i = 0; i < 8; i++) {
		rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_INEN(i),	&(cti_inen[i]));
		if (rc != ERROR_OK)	goto err;
		rc = mem_ap_read_u32(dap, cti_base + ARMV8_REG_CTI_OUTEN(i),&(cti_outen[i]));
		if (rc != ERROR_OK)	goto err;
	}
	rc = dap_run(dap);
	if (rc != ERROR_OK)	goto err;

	command_print(cmd_ctx,
		"\tcontrol (glben) = %08x    gate = %08x\n"
		"\tasicctl = %08x            appset = %08x\n"
		"\ttrig in status = %08x     trig out status = %08x\n"
		"\t  ch in status = %08x       ch out status = %08x\n"
		"\t in en 0..7 = %08x %08x %08x %08x\n"
		"\t              %08x %08x %08x %08x\n"
		"\tout en 0..7 = %08x %08x %08x %08x\n"
		"\t              %08x %08x %08x %08x\n",
		cti_control, cti_gate,
		cti_asicctl, cti_appset,
		cti_triginstatus,	cti_trigoutstatus,
		cti_chinstatus,		cti_choutstatus,
		cti_inen[0], cti_inen[1], cti_inen[2], cti_inen[3],
		cti_inen[4], cti_inen[5], cti_inen[6], cti_inen[7],
		cti_outen[0], cti_outen[1], cti_outen[2], cti_outen[3],
		cti_outen[4], cti_outen[5], cti_outen[6], cti_outen[7]
	);

err:
	return rc;
}

/* ---------------------------------------- */
#define ARMV8_BP_NUM		(4)		// Should be 16
#define ARMV8_WP_NUM		(4)
int print_target_debug_info_bpwp(
	struct command_context *cmd_ctx,
	struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *dap = armv8->arm.dap;
//	struct aarch64_brp *brp_list = aarch64->brp_list;

	int rc = ERROR_FAIL;
	int i;
	uint32_t dbgbcr, dbgbvr_lo, dbgbvr_hi;
	uint64_t dbgbvr;

	command_print(cmd_ctx, "Target %s", target_name(target));

	/* H9.2.2 DBGBCR<n>_EL1, n = 0-15 */
	for (i = 0; i < ARMV8_BP_NUM; i++) {
		rc = mem_ap_read_u32(dap,
			armv8->debug_base + ARMV8_REG_DBGBCR_EL1(i), &dbgbcr);
		if (rc != ERROR_OK)	goto err;
		rc = mem_ap_read_u32(dap,
			armv8->debug_base + ARMV8_REG_DBGBVR_EL1_LO(i), &dbgbvr_lo);
		if (rc != ERROR_OK)	goto err;
		rc = mem_ap_read_u32(dap,
			armv8->debug_base + ARMV8_REG_DBGBVR_EL1_HI(i), &dbgbvr_hi);
		if (rc != ERROR_OK)	goto err;
		rc = dap_run(dap);
		if (rc != ERROR_OK)	goto err;

		dbgbvr = ((uint64_t)dbgbvr_hi << 32) | (uint64_t)dbgbvr_lo;

		command_print(cmd_ctx,
			"  BP(%d) BCR=0x%08"PRIx32"(BT:0x%02x,LBN:0x%02x,BAS:0x%02x,E:%d) Addr=0x%.16"PRIX64,
			i,
			dbgbcr,
			(dbgbcr & ARMV8_DBGBCR_BT_MASK) >> ARMV8_DBGBCR_BT_SHIFT,
			(dbgbcr & ARMV8_DBGBCR_LBN_MASK) >> ARMV8_DBGBCR_LBN_SHIFT,
			(dbgbcr & ARMV8_DBGBCR_BAS_MASK) >> ARMV8_DBGBCR_BAS_SHIFT,
			(dbgbcr & ARMV8_DBGBCR_E) ? 1 : 0,
			dbgbvr);
	}

#if 0
	/* H9.2.8 DBGWCR<n>_EL1, n = 0-15 */
	for (i = 0; i < 15; i++) {
		rc = aarch64_dap_read_memap_register_u32(target,
			armv8->debug_base + ARMV8_REG_DBGWCR_EL1(i), &dbgbcr);
		if (rc != ERROR_OK)	goto err;
		rc = aarch64_dap_read_memap_register_u32(target,
			armv8->debug_base + ARMV8_REG_DBGWVR_EL1_LO(i), &dbgbvr_lo);
		if (rc != ERROR_OK)	goto err;
		rc = aarch64_dap_read_memap_register_u32(target,
			armv8->debug_base + ARMV8_REG_DBGWVR_EL1_HI(i), &dbgbvr_hi);
		if (rc != ERROR_OK)	goto err;

	}
#endif

err:
	return rc;
}

void print_target_debug_info_all(
	struct command_context *cmd_ctx,
	struct target *target)
{
	print_target_debug_info_status  (cmd_ctx, target);
	print_target_debug_info_cti     (cmd_ctx, target);
	print_target_debug_info_bpwp    (cmd_ctx, target);
}

COMMAND_HANDLER(aarch64_handle_debug_info_status_command)
{
	struct target *target = get_current_target(CMD_CTX);
	bool opt_smp = false;

	if (CMD_ARGC >= 1) {
		opt_smp = (strcmp(CMD_ARGV[0], "smp") == 0);
	}

	if (!opt_smp) {
		print_target_debug_info_status(CMD_CTX, target);
		return ERROR_OK;
	}

	for (target = all_targets; target; target = target->next)
		print_target_debug_info_status(CMD_CTX, target);

	return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_debug_info_cti_command)
{
	struct target *target = get_current_target(CMD_CTX);
	bool opt_smp = false;

	if (CMD_ARGC >= 1) {
		opt_smp = (strcmp(CMD_ARGV[0], "smp") == 0);
	}

	if (!opt_smp) {
		print_target_debug_info_cti(CMD_CTX, target);
		return ERROR_OK;
	}

	for (target = all_targets; target; target = target->next)
		print_target_debug_info_cti(CMD_CTX, target);

	return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_debug_info_bpwp_command)
{
	struct target *target = get_current_target(CMD_CTX);
	bool opt_smp = false;

	if (CMD_ARGC >= 1) {
		opt_smp = (strcmp(CMD_ARGV[0], "smp") == 0);
	}

	if (!opt_smp) {
		print_target_debug_info_bpwp(CMD_CTX, target);
		return ERROR_OK;
	}

	for (target = all_targets; target; target = target->next)
		print_target_debug_info_bpwp(CMD_CTX, target);

	return ERROR_OK;
}

/* Falls into this function if nothing matched in the subcommand table */
COMMAND_HANDLER(aarch64_handle_debug_info_command)
{
	struct target *target;

/*
	for (i = 0; i < CMD_ARGC; i++)
		command_print(CMD_CTX, "parm %d = %s\n", i, CMD_ARGV[i]);
*/

	for (target = all_targets; target; target = target->next)
		print_target_debug_info_all(CMD_CTX, target);

	return ERROR_OK;
}

/* ---------------------------------------- */

/*
		d2801543	mov	x3, #0xAA
		14000002	b	loop
		d2800aa3	mov	x3, #0x55
loop:
		14000000	b	loop
*/
static uint32_t opcodes_55aa[] = {
			/* loop_00: */
			0x14000000,		/* b   loop_00 */

			0xd2801543,		/* mov x3, #0xAA */
			/* loop_10: */
			0x14000000,		/* b   loop_10 */

			0xd2800aa3,		/* mov x3, #0x55 */
			/* loop_20: */
			0x14000000		/* b   loop_20 */
			};

/*
	    d2800004     mov    x4, #0x0    // #0
	    d2800025     mov    x5, #0x1    // #1
	    8b050086     add    x6, x4, x5
	    8b0600a7     add    x7, x5, x6
	    8b0700c8     add    x8, x6, x7
	    8b0800e9     add    x9, x7, x8
	dead_loop:
	    14000000     b    18 <dead_loop>
*/
static uint32_t opcodes_add[] = {
			0xd2800004,		/* mov x4, #0x0 */
			0xd2800025,		/* mov x5, #0x1 */
			0x8b050086,		/* add x6, x4, x5 */
			0x8b0600a7,		/* add x7, x5, x6 */
			0x8b0700c8,		/* add x8, x6, x7 */
			0x8b0800e9,		/* add x9, x7, x8 */
			0x14000000		/* b <dead_loop> */
			};

COMMAND_HANDLER(aarch64_handle_debug_codes_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target->arch_info;
	struct reg *r;
	uint64_t addr;
	int64_t offset;
	uint64_t armpc_addr, regpc_addr;
	uint32_t *code;
	uint32_t code_size;

	armpc_addr = buf_get_u64(arm->pc->value, 0, 64);

	r = armv8_get_reg_by_num(arm, AARCH64_PC);
	regpc_addr = buf_get_u64(r->value, 0, 64);

	command_print(CMD_CTX, "armpc=0x%.16" PRIX64 ", regpc=0x%.16" PRIX64,
		armpc_addr,
		regpc_addr
		);

	/* Parameter 0: code pattern */
	code = NULL;
	if (CMD_ARGC >= 1) {
		if (strcmp(CMD_ARGV[0], "55aa") == 0) {
			code = opcodes_55aa;
			code_size = ARRAY_SIZE(opcodes_55aa);
		} else if (strcmp(CMD_ARGV[0], "add") == 0) {
			code = opcodes_add;
			code_size = ARRAY_SIZE(opcodes_add);
		}
	}
	if (code == NULL) {
		command_print(CMD_CTX, "No code selected");
		return ERROR_OK;
	}

	/* Parameter 1: address/offset */
	addr = regpc_addr;
	if (CMD_ARGC >= 2) {
		if ((CMD_ARGV[1][0] == '-') || (CMD_ARGV[1][0] == '+')) {
			/* relative offset */
			COMMAND_PARSE_NUMBER(s64, CMD_ARGV[1], offset);
			addr += offset;
			command_print(CMD_CTX, "offset = 0x%"PRIX64 ", addr=0x%.16"PRIX64,
				offset, addr);
		} else {
			/* absoluted address */
			COMMAND_PARSE_NUMBER(u64, CMD_ARGV[1], addr);
			command_print(CMD_CTX, "user pc = 0x%.16" PRIX64, addr);
		}
	}

#if 1
	return target->type64->write_memory(target, addr, 4, code_size, (uint8_t *)code);
#else
	while (code_size--) {
//		aarch64_write_memory(target, addr + i*4, 4, 1, (uint8_t *)(&opcodes[i]));
		target->type64->write_memory(target, addr, 4, 1, (uint8_t *)(code++));

		/* step to next instruction (4-byte) */
		addr += 4;
	} /* End of while(code_size) */

	return ERROR_OK;
#endif
}

COMMAND_HANDLER(aarch64_handle_debug_cache_ic_command)
{
	struct target *target = get_current_target(CMD_CTX);
	bool ialluis;
	bool iallu;
	uint32_t opcode;
	uint32_t edscr;
	int retval;

	if (CMD_ARGC == 0) {
		iallu = 0;
		ialluis = 1;
		opcode = 0xd508711f;	/* ic ialluis */
		} else if (CMD_ARGC >= 1) {
		ialluis = strcmp(CMD_ARGV[0], "ialluis") == 0;
		iallu = strcmp(CMD_ARGV[0], "iallu") == 0;
	}

	/* IC IALLU and DC ste/way: apply only to the caches of the PE that performs the instruction.
	 * IC IALLUIS instruction can affect the caches of all PEs in the same Inner Shareable shareability domain */

	if (iallu)
		opcode = 0xd508751f;	/* ic    iallu */
	if (ialluis)
		opcode = 0xd508711f;	/* ic    ialluis */
	edscr = 0;
	retval = aarch64_exec_opcode(target, opcode, &edscr);

	command_print(CMD_CTX, "I-Cahce flushed (ialluis=%d, iallu=%d)",
		ialluis, iallu);

	return retval;
}

/* Kernel: arch/arm64/mm/cache.S
 * flush_cache_all
 *	mov	x12, lr
 *	bl	__flush_dcache_all
 *	mov	x0, #0
 *	ic	ialluis		// I+BTB cache invalidate
 *	ret	x12
 */
COMMAND_HANDLER(aarch64_handle_debug_cache_flushall_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	uint32_t edscr;
	uint32_t opcode;
	int rc = ERROR_OK;

#if 0	/* beginning of cache flushing only: doens't work */
	/* DSB SY & ISB: Invalidate entire TLB */
	opcode = A64_OPCODE_DSB_SY;		/* d5033f9f 	dsb	sy	*/
	if (rc == ERROR_OK)
		rc = aarch64_exec_opcode(target, opcode, &edscr);

	opcode = A64_OPCODE_ISB;		/* d5033fdf 	isb		*/
	if (rc == ERROR_OK)
		rc = aarch64_exec_opcode(target, opcode, &edscr);
#endif

	/* Flush D-Cache */
	if (armv8->armv8_mmu.armv8_cache.flush_dcache_all)
		armv8->armv8_mmu.armv8_cache.flush_dcache_all(target);

	/* Flush I-Cache */
	opcode = 0xd508711f;	/* ic    ialluis */
	edscr = 0;
	rc = aarch64_exec_opcode(target, opcode, &edscr);

#if 0
	/* DSB SY & ISB: Invalidate entire TLB */
	opcode = A64_OPCODE_DSB_SY;		/* d5033f9f 	dsb	sy	*/
	if (rc == ERROR_OK)
		rc = aarch64_exec_opcode(target, opcode, &edscr);

	opcode = A64_OPCODE_ISB;		/* d5033fdf 	isb		*/
	if (rc == ERROR_OK)
		rc = aarch64_exec_opcode(target, opcode, &edscr);
#endif

	if (rc != ERROR_OK) {
		command_print(CMD_CTX, "Fail to flushall, rc = %d", rc);
	}

	return rc;
}

COMMAND_HANDLER(aarch64_handle_debug_resume_command)
{

	return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_debug_step_command)
{

	return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_debug_test_command)
{

	return ERROR_OK;
}

#if 0
COMMAND_HANDLER(aarch64_handle_debug_command)
{
//	struct target *target = get_current_target(CMD_CTX);

	return ERROR_OK;
}
#endif

static const struct command_registration aarch64_debug_info_subcommand_handlers[] = {
	{
		.name = "status",		/* aarch64 debug info status */
		.handler = aarch64_handle_debug_info_status_command,
		.mode = COMMAND_EXEC,
		.help = "aarch64 debug info status [smp]",
		.usage = "",
	},
	{
		.name = "cti",			/* aarch64 debug info cti */
		.handler = aarch64_handle_debug_info_cti_command,
		.mode = COMMAND_EXEC,
		.help = "aarch64 debug info cti [smp]",
		.usage = "",
	},
	{
		.name = "bpwp",			/* aarch64 debug info bpwp */
		.handler = aarch64_handle_debug_info_bpwp_command,
		.mode = COMMAND_EXEC,
		.help = "aarch64 debug info bpwp [smp]",
		.usage = "",
	},

	COMMAND_REGISTRATION_DONE
};

static const struct command_registration aarch64_debug_cache_subcommand_handlers[] = {
	{
		.name = "iallu",		/* aarch64 debug cache iallu */
		.handler = aarch64_handle_debug_cache_ic_command,
		.mode = COMMAND_EXEC,
		.help = "ic iallu",
		.usage = "",
	},
	{
		.name = "ialluis",		/* aarch64 debug cache ialluis */
		.handler = aarch64_handle_debug_cache_ic_command,
		.mode = COMMAND_EXEC,
		.help = "ic ialluis",
		.usage = "",
	},
	{
		.name = "flushall",		/* aarch64 debug cache flushall */
		.handler = aarch64_handle_debug_cache_flushall_command,
		.mode = COMMAND_EXEC,
		.help = "flush I & D caches",
		.usage = "",
	},

	COMMAND_REGISTRATION_DONE
};

const struct command_registration aarch64_debug_subcommand_handlers[] = {
	{
		.name = "info",
		.handler = aarch64_handle_debug_info_command,
		.mode = COMMAND_EXEC,
		.help = "debugging information",
		.chain = aarch64_debug_info_subcommand_handlers,
		.usage = "",
	},

	{
		.name = "codes",		/* debug codes */
		.mode = COMMAND_ANY,
		.handler = aarch64_handle_debug_codes_command,
		.help = "replace opcodes around PC",
//		.chain = aarch64_debug_codes_subcommand_handlers,
		.usage = "",
	},

	{
		.name = "resume",		/* debug resume */
		.mode = COMMAND_ANY,
		.handler = aarch64_handle_debug_resume_command,
		.help = "debugging 'resume' command",
//		.chain = aarch64_debug_resume_subcommand_handlers,
		.usage = "",
	},

	{
		.name = "step",			/* debug step */
		.mode = COMMAND_ANY,
		.handler = aarch64_handle_debug_step_command,
		.help = "debugging 'step' command",
//		.chain = aarch64_debug_step_subcommand_handlers,
		.usage = "",
	},

	{
		.name = "cache",		/* cache operation */
		.mode = COMMAND_EXEC,
//		.handler = aarch64_handle_debug_cache_command,
		.help = "cache operation",
		.chain = aarch64_debug_cache_subcommand_handlers,
		.usage = "",
	},

	{
		.name = "test",		/* test operation */
		.mode = COMMAND_EXEC,
		.handler = aarch64_handle_debug_test_command,
		.help = "test something misc",
		.usage = "",
	},

	COMMAND_REGISTRATION_DONE
};

