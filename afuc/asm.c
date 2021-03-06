/*
 * Copyright (c) 2017 Rob Clark <robdclark@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>

#include "afuc.h"
#include "rnn.h"
#include "rnndec.h"
#include "parser.h"
#include "asm.h"

static int gpuver;


static struct rnndeccontext *ctx;
static struct rnndb *db;
struct rnndomain *dom[2];


/* bit lame to hard-code max but fw sizes are small */
static struct asm_instruction instructions[0x2000];
static unsigned num_instructions;

static struct asm_label labels[0x512];
static unsigned num_labels;

struct asm_instruction *next_instr(int tok)
{
	struct asm_instruction *ai = &instructions[num_instructions++];
	assert(num_instructions < ARRAY_SIZE(instructions));
	ai->tok = tok;
	return ai;
}

void decl_label(const char *str)
{
	struct asm_label *label = &labels[num_labels++];

	assert(num_labels < ARRAY_SIZE(labels));

	label->offset = num_instructions;
	label->label = str;
}

static int resolve_label(const char *str)
{
	int i;

	for (i = 0; i < num_labels; i++) {
		struct asm_label *label = &labels[i];

		if (!strcmp(str, label->label)) {
			return label->offset;
		}
	}

	fprintf(stderr, "Undeclared label: %s\n", str);
	exit(2);
}

static afuc_opc tok2alu(int tok)
{
	switch (tok) {
	case T_OP_ADD:   return OPC_ADD;
	case T_OP_ADDHI: return OPC_ADDHI;
	case T_OP_SUB:   return OPC_SUB;
	case T_OP_SUBHI: return OPC_SUBHI;
	case T_OP_AND:   return OPC_AND;
	case T_OP_OR:    return OPC_OR;
	case T_OP_XOR:   return OPC_XOR;
	case T_OP_NOT:   return OPC_NOT;
	case T_OP_SHL:   return OPC_SHL;
	case T_OP_USHR:  return OPC_USHR;
	case T_OP_ISHR:  return OPC_ISHR;
	case T_OP_ROT:   return OPC_ROT;
	case T_OP_MUL8:  return OPC_MUL8;
	case T_OP_MIN:   return OPC_MIN;
	case T_OP_MAX:   return OPC_MAX;
	case T_OP_CMP:   return OPC_CMP;
	default:
		assert(0);
		return -1;
	}
}

static void emit_instructions(int outfd)
{
	int i;

	/* there is an extra 0x00000000 which kernel strips off.. we could
	 * perhaps use it for versioning.
	 */
	i = 0;
	write(outfd, &i, 4);

	for (i = 0; i < num_instructions; i++) {
		struct asm_instruction *ai = &instructions[i];
		afuc_instr instr = {0};
		afuc_opc opc;

		/* special case, 2nd dword is patched up w/ # of instructions
		 * (ie. offset of jmptbl)
		 */
		if (i == 1) {
			write(outfd, &num_instructions, 4);
			continue;
		}

		if (ai->is_literal) {
			write(outfd, &ai->literal, 4);
			continue;
		}

		switch (ai->tok) {
		case T_OP_NOP:
			opc = OPC_NOP;
			break;
		case T_OP_ADD:
		case T_OP_ADDHI:
		case T_OP_SUB:
		case T_OP_SUBHI:
		case T_OP_AND:
		case T_OP_OR:
		case T_OP_XOR:
		case T_OP_NOT:
		case T_OP_SHL:
		case T_OP_USHR:
		case T_OP_ISHR:
		case T_OP_ROT:
		case T_OP_MUL8:
		case T_OP_MIN:
		case T_OP_MAX:
		case T_OP_CMP:
			if (ai->has_immed) {
				opc = tok2alu(ai->tok);
				instr.alui.dst = ai->dst;
				instr.alui.src = ai->src1;
				instr.alui.uimm = ai->immed;
			} else {
				opc = OPC_ALU;
				instr.alu.dst  = ai->dst;
				instr.alu.src1 = ai->src1;
				instr.alu.src2 = ai->src2;
				instr.alu.alu = tok2alu(ai->tok);
			}
			break;
		case T_OP_MOV:
			/* move can either be encoded as movi (ie. move w/ immed) or
			 * an alu instruction
			 */
			if (ai->has_immed) {
				opc = OPC_MOVI;
				instr.movi.dst = ai->dst;
				instr.movi.uimm = ai->immed;
				instr.movi.shift = ai->shift;
			} else {
				/* encode as: or $dst, $00, $src */
				opc = OPC_ALU;
				instr.alu.dst  = ai->dst;
				instr.alu.src1 = 0x00;      /* $00 reads-back 0 */
				instr.alu.src2 = ai->src1;
				instr.alu.alu = OPC_OR;
			}
			break;
		case T_OP_CWRITE:
		case T_OP_CREAD:
			if (ai->tok == T_OP_CWRITE) {
				opc = OPC_CWRITE;
			} else if (ai->tok == T_OP_CREAD) {
				opc = OPC_CREAD;
			}
			instr.control.src1 = ai->src1;
			instr.control.src2 = ai->src2;
			instr.control.flags = ai->bit;
			instr.control.uimm = ai->immed;
			break;
		case T_OP_BRNE:
		case T_OP_BREQ:
			if (ai->has_immed) {
				opc = (ai->tok == T_OP_BRNE) ? OPC_BRNEI : OPC_BREQI;
				instr.br.bit_or_imm = ai->immed;
			} else {
				opc = (ai->tok == T_OP_BRNE) ? OPC_BRNEB : OPC_BREQB;
				instr.br.bit_or_imm = ai->bit;
			}
			instr.br.src = ai->src1;
			instr.br.ioff = resolve_label(ai->label) - i;
			break;
		case T_OP_RET:
			opc = OPC_RET;
			break;
		case T_OP_CALL:
			opc = OPC_CALL;
			instr.call.uoff = resolve_label(ai->label);
			break;
		case T_OP_JUMP:
			/* encode jump as: brne $00, b0, #label */
			opc = OPC_BRNEB;
			instr.br.bit_or_imm = 0;
			instr.br.src = 0x00;       /* $00 reads-back 0.. compare to 0 */
			instr.br.ioff = resolve_label(ai->label) - i;
			break;
		case T_OP_WAITIN:
			opc = OPC_WIN;
			break;
		default:
			assert(0);
		}

		afuc_set_opc(&instr, opc, ai->flush);

		write(outfd, &instr, 4);
	}

}

static int find_enum_val(struct rnnenum *en, const char *name)
{
	int i;

	for (i = 0; i < en->valsnum; i++)
		if (en->vals[i]->valvalid && !strcmp(name, en->vals[i]->name))
			return en->vals[i]->value;

	return -1;
}

static void emit_jumptable(int outfd)
{
	struct rnnenum *en = rnn_findenum(ctx->db, "adreno_pm4_type3_packets");
	uint32_t jmptable[0x80] = {0};
	int i;

	for (i = 0; i < num_labels; i++) {
		struct asm_label *label = &labels[i];
		int id = find_enum_val(en, label->label);

		/* if it doesn't match a known PM4 packet-id, try to match UNKN%d: */
		if (id < 0) {
			if (sscanf(label->label, "UNKN%d", &id) != 1) {
				/* if still not found, must not belong in jump-table: */
				continue;
			}
		}

		jmptable[id] = label->offset;
	}

	write(outfd, jmptable, sizeof(jmptable));
}

static void usage(void)
{
	fprintf(stderr, "Usage:\n"
			"\tdisasm [-g GPUVER] [-v] [-c] filename.asm\n"
			"\t\t-g - specify GPU version (5, etc)\n"
			"\t\t-c - use colors\n"
			"\t\t-v - verbose output\n"
		);
	exit(2);
}

int main(int argc, char **argv)
{
	FILE *in;
	char *file, *outfile, *name;
	int c, ret, outfd;

	/* Argument parsing: */
	while ((c = getopt (argc, argv, "g:")) != -1) {
		switch (c) {
			case 'g':
				gpuver = atoi(optarg);
				break;
			default:
				usage();
		}
	}

	if (optind >= (argc + 1)) {
		fprintf(stderr, "no file specified!\n");
		usage();
	}

	file = argv[optind];
	outfile = argv[optind + 1];

	outfd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (outfd < 0) {
		fprintf(stderr, "could not open \"%s\"\n", outfile);
		usage();
	}

	in = fopen(file, "r");
	if (!in) {
		fprintf(stderr, "could not open \"%s\"\n", file);
		usage();
	}

	yyset_in(in);

	/* if gpu version not specified, infer from filename: */
	if (!gpuver) {
		if (strstr(file, "a5")) {
			gpuver = 5;
		}
	}

	switch (gpuver) {
	case 5:
		name = "A5XX";
		break;
	default:
		fprintf(stderr, "unknown GPU version!\n");
		usage();
	}

	rnn_init();
	db = rnn_newdb();

	ctx = rnndec_newcontext(db);

	rnn_parsefile(db, "adreno.xml");
	dom[0] = rnn_finddomain(db, name);
	dom[1] = rnn_finddomain(db, "AXXX");

	ret = yyparse();
	if (ret) {
		fprintf(stderr, "parse failed: %d\n", ret);
		return ret;
	}

	emit_instructions(outfd);
	emit_jumptable(outfd);

	close(outfd);

	return 0;
}
