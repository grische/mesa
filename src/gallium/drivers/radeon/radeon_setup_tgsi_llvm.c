/*
 * Copyright 2011 Advanced Micro Devices, Inc.
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
 *
 * Authors: Tom Stellard <thomas.stellard@amd.com>
 *
 */
#include "radeon_llvm.h"

#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_gather.h"
#include "gallivm/lp_bld_flow.h"
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_intr.h"
#include "gallivm/lp_bld_misc.h"
#include "gallivm/lp_bld_swizzle.h"
#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_parse.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_debug.h"

#include <llvm-c/Core.h>
#include <llvm-c/Transforms/Scalar.h>

static struct radeon_llvm_loop * get_current_loop(struct radeon_llvm_context * ctx)
{
	return ctx->loop_depth > 0 ? ctx->loop + (ctx->loop_depth - 1) : NULL;
}

static struct radeon_llvm_branch * get_current_branch(
	struct radeon_llvm_context * ctx)
{
	return ctx->branch_depth > 0 ?
			ctx->branch + (ctx->branch_depth - 1) : NULL;
}

unsigned radeon_llvm_reg_index_soa(unsigned index, unsigned chan)
{
 return (index * 4) + chan;
}

static LLVMValueRef emit_swizzle(
	struct lp_build_tgsi_context * bld_base,
        LLVMValueRef value,
	unsigned swizzle_x,
	unsigned swizzle_y,
	unsigned swizzle_z,
	unsigned swizzle_w)
{
	LLVMValueRef swizzles[4];
	LLVMTypeRef i32t =
		LLVMInt32TypeInContext(bld_base->base.gallivm->context);

	swizzles[0] = LLVMConstInt(i32t, swizzle_x, 0);
	swizzles[1] = LLVMConstInt(i32t, swizzle_y, 0);
	swizzles[2] = LLVMConstInt(i32t, swizzle_z, 0);
	swizzles[3] = LLVMConstInt(i32t, swizzle_w, 0);

	return LLVMBuildShuffleVector(bld_base->base.gallivm->builder,
		value,
		LLVMGetUndef(LLVMTypeOf(value)),
		LLVMConstVector(swizzles, 4), "");
}

static struct tgsi_declaration_range
get_array_range(struct lp_build_tgsi_context *bld_base,
		unsigned File, const struct tgsi_ind_register *reg)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);

	if (File != TGSI_FILE_TEMPORARY || reg->ArrayID == 0 ||
	    reg->ArrayID > bld_base->info->array_max[TGSI_FILE_TEMPORARY]) {
		struct tgsi_declaration_range range;
		range.First = 0;
		range.Last = bld_base->info->file_max[File];
		return range;
	}

	return ctx->arrays[reg->ArrayID - 1];
}

static LLVMValueRef
emit_array_index(
	struct lp_build_tgsi_soa_context *bld,
	const struct tgsi_ind_register *reg,
	unsigned offset)
{
	struct gallivm_state * gallivm = bld->bld_base.base.gallivm;

	LLVMValueRef addr = LLVMBuildLoad(gallivm->builder, bld->addr[reg->Index][reg->Swizzle], "");
	return LLVMBuildAdd(gallivm->builder, addr, lp_build_const_int32(gallivm, offset), "");
}

LLVMValueRef
radeon_llvm_emit_fetch_double(
	struct lp_build_tgsi_context *bld_base,
	LLVMValueRef ptr,
	LLVMValueRef ptr2)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef result;

	result = LLVMGetUndef(LLVMVectorType(LLVMIntTypeInContext(bld_base->base.gallivm->context, 32), bld_base->base.type.length * 2));

	result = LLVMBuildInsertElement(builder,
					result,
					bitcast(bld_base, TGSI_TYPE_UNSIGNED, ptr),
					bld_base->int_bld.zero, "");
	result = LLVMBuildInsertElement(builder,
					result,
					bitcast(bld_base, TGSI_TYPE_UNSIGNED, ptr2),
					bld_base->int_bld.one, "");
	return bitcast(bld_base, TGSI_TYPE_DOUBLE, result);
}

static LLVMValueRef
emit_array_fetch(
	struct lp_build_tgsi_context *bld_base,
	unsigned File, enum tgsi_opcode_type type,
	struct tgsi_declaration_range range,
	unsigned swizzle)
{
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	struct gallivm_state * gallivm = bld->bld_base.base.gallivm;
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	unsigned i, size = range.Last - range.First + 1;
	LLVMTypeRef vec = LLVMVectorType(tgsi2llvmtype(bld_base, type), size);
	LLVMValueRef result = LLVMGetUndef(vec);

	struct tgsi_full_src_register tmp_reg = {};
	tmp_reg.Register.File = File;

	for (i = 0; i < size; ++i) {
		tmp_reg.Register.Index = i + range.First;
		LLVMValueRef temp = radeon_llvm_emit_fetch(bld_base, &tmp_reg, type, swizzle);
		result = LLVMBuildInsertElement(builder, result, temp,
			lp_build_const_int32(gallivm, i), "");
	}
	return result;
}

static bool uses_temp_indirect_addressing(
	struct lp_build_tgsi_context *bld_base)
{
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	return (bld->indirect_files & (1 << TGSI_FILE_TEMPORARY));
}

LLVMValueRef radeon_llvm_emit_fetch(struct lp_build_tgsi_context *bld_base,
				    const struct tgsi_full_src_register *reg,
				    enum tgsi_opcode_type type,
				    unsigned swizzle)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef result = NULL, ptr, ptr2;

	if (swizzle == ~0) {
		LLVMValueRef values[TGSI_NUM_CHANNELS];
		unsigned chan;
		for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
			values[chan] = radeon_llvm_emit_fetch(bld_base, reg, type, chan);
		}
		return lp_build_gather_values(bld_base->base.gallivm, values,
					      TGSI_NUM_CHANNELS);
	}

	if (reg->Register.Indirect) {
		struct tgsi_declaration_range range = get_array_range(bld_base,
			reg->Register.File, &reg->Indirect);
		return LLVMBuildExtractElement(builder,
			emit_array_fetch(bld_base, reg->Register.File, type, range, swizzle),
			emit_array_index(bld, &reg->Indirect, reg->Register.Index - range.First),
			"");
	}

	switch(reg->Register.File) {
	case TGSI_FILE_IMMEDIATE: {
		LLVMTypeRef ctype = tgsi2llvmtype(bld_base, type);
		if (type == TGSI_TYPE_DOUBLE) {
			result = LLVMGetUndef(LLVMVectorType(LLVMIntTypeInContext(bld_base->base.gallivm->context, 32), bld_base->base.type.length * 2));
			result = LLVMConstInsertElement(result,
							bld->immediates[reg->Register.Index][swizzle],
							bld_base->int_bld.zero);
			result = LLVMConstInsertElement(result,
							bld->immediates[reg->Register.Index][swizzle + 1],
							bld_base->int_bld.one);
			return LLVMConstBitCast(result, ctype);
		} else {
			return LLVMConstBitCast(bld->immediates[reg->Register.Index][swizzle], ctype);
		}
	}

	case TGSI_FILE_INPUT:
		result = ctx->inputs[radeon_llvm_reg_index_soa(reg->Register.Index, swizzle)];
		if (type == TGSI_TYPE_DOUBLE) {
			ptr = result;
			ptr2 = ctx->inputs[radeon_llvm_reg_index_soa(reg->Register.Index, swizzle + 1)];
			return radeon_llvm_emit_fetch_double(bld_base, ptr, ptr2);
		}
		break;

	case TGSI_FILE_TEMPORARY:
		if (reg->Register.Index >= ctx->temps_count)
			return LLVMGetUndef(tgsi2llvmtype(bld_base, type));
		if (uses_temp_indirect_addressing(bld_base)) {
			ptr = lp_get_temp_ptr_soa(bld, reg->Register.Index, swizzle);
			break;
		}
		ptr = ctx->temps[reg->Register.Index * TGSI_NUM_CHANNELS + swizzle];
		if (type == TGSI_TYPE_DOUBLE) {
			ptr2 = ctx->temps[reg->Register.Index * TGSI_NUM_CHANNELS + swizzle + 1];
			return radeon_llvm_emit_fetch_double(bld_base,
						 LLVMBuildLoad(builder, ptr, ""),
						 LLVMBuildLoad(builder, ptr2, ""));
		}
		result = LLVMBuildLoad(builder, ptr, "");
		break;

	case TGSI_FILE_OUTPUT:
		ptr = lp_get_output_ptr(bld, reg->Register.Index, swizzle);
		if (type == TGSI_TYPE_DOUBLE) {
			ptr2 = lp_get_output_ptr(bld, reg->Register.Index, swizzle + 1);
			return radeon_llvm_emit_fetch_double(bld_base,
						 LLVMBuildLoad(builder, ptr, ""),
						 LLVMBuildLoad(builder, ptr2, ""));
		}
		result = LLVMBuildLoad(builder, ptr, "");
		break;

	default:
		return LLVMGetUndef(tgsi2llvmtype(bld_base, type));
	}

	return bitcast(bld_base, type, result);
}

static LLVMValueRef fetch_system_value(
	struct lp_build_tgsi_context * bld_base,
	const struct tgsi_full_src_register *reg,
	enum tgsi_opcode_type type,
	unsigned swizzle)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;

	LLVMValueRef cval = ctx->system_values[reg->Register.Index];
	if (LLVMGetTypeKind(LLVMTypeOf(cval)) == LLVMVectorTypeKind) {
		cval = LLVMBuildExtractElement(gallivm->builder, cval,
					       lp_build_const_int32(gallivm, swizzle), "");
	}
	return bitcast(bld_base, type, cval);
}

static LLVMValueRef si_build_alloca_undef(struct gallivm_state *gallivm,
					  LLVMTypeRef type,
					  const char *name)
{
	LLVMValueRef ptr = lp_build_alloca(gallivm, type, name);
	LLVMBuildStore(gallivm->builder, LLVMGetUndef(type), ptr);
	return ptr;
}

static void emit_declaration(
	struct lp_build_tgsi_context * bld_base,
	const struct tgsi_full_declaration *decl)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	unsigned first, last, i, idx;
	switch(decl->Declaration.File) {
	case TGSI_FILE_ADDRESS:
	{
		 unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			unsigned chan;
			for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
				 ctx->soa.addr[idx][chan] = si_build_alloca_undef(
					&ctx->gallivm,
					ctx->soa.bld_base.uint_bld.elem_type, "");
			}
		}
		break;
	}

	case TGSI_FILE_TEMPORARY:
		if (decl->Declaration.Array) {
			if (!ctx->arrays) {
				int size = bld_base->info->array_max[TGSI_FILE_TEMPORARY];
				ctx->arrays = MALLOC(sizeof(ctx->arrays[0]) * size);
			}

			ctx->arrays[decl->Array.ArrayID - 1] = decl->Range;
		}
		if (uses_temp_indirect_addressing(bld_base)) {
			lp_emit_declaration_soa(bld_base, decl);
			break;
		}
		first = decl->Range.First;
		last = decl->Range.Last;
		if (!ctx->temps_count) {
			ctx->temps_count = bld_base->info->file_max[TGSI_FILE_TEMPORARY] + 1;
			ctx->temps = MALLOC(TGSI_NUM_CHANNELS * ctx->temps_count * sizeof(LLVMValueRef));
		}
		for (idx = first; idx <= last; idx++) {
			for (i = 0; i < TGSI_NUM_CHANNELS; i++) {
				ctx->temps[idx * TGSI_NUM_CHANNELS + i] =
					si_build_alloca_undef(bld_base->base.gallivm,
							      bld_base->base.vec_type,
							      "temp");
			}
		}
		break;

	case TGSI_FILE_INPUT:
	{
		unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			if (ctx->load_input)
				ctx->load_input(ctx, idx, decl);
		}
	}
	break;

	case TGSI_FILE_SYSTEM_VALUE:
	{
		unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			ctx->load_system_value(ctx, idx, decl);
		}
	}
	break;

	case TGSI_FILE_OUTPUT:
	{
		unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			unsigned chan;
			assert(idx < RADEON_LLVM_MAX_OUTPUTS);
			for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
				ctx->soa.outputs[idx][chan] = si_build_alloca_undef(
					&ctx->gallivm,
					ctx->soa.bld_base.base.elem_type, "");
			}
		}
		break;
	}

	case TGSI_FILE_MEMORY:
		ctx->declare_memory_region(ctx, decl);
		break;

	default:
		break;
	}
}

LLVMValueRef radeon_llvm_saturate(struct lp_build_tgsi_context *bld_base,
                                  LLVMValueRef value)
{
	struct lp_build_emit_data clamp_emit_data;

	memset(&clamp_emit_data, 0, sizeof(clamp_emit_data));
	clamp_emit_data.arg_count = 3;
	clamp_emit_data.args[0] = value;
	clamp_emit_data.args[2] = bld_base->base.one;
	clamp_emit_data.args[1] = bld_base->base.zero;

	return lp_build_emit_llvm(bld_base, TGSI_OPCODE_CLAMP,
				  &clamp_emit_data);
}

void radeon_llvm_emit_store(
	struct lp_build_tgsi_context * bld_base,
	const struct tgsi_full_instruction * inst,
	const struct tgsi_opcode_info * info,
	LLVMValueRef dst[4])
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	struct gallivm_state *gallivm = bld->bld_base.base.gallivm;
	const struct tgsi_full_dst_register *reg = &inst->Dst[0];
	LLVMBuilderRef builder = bld->bld_base.base.gallivm->builder;
	LLVMValueRef temp_ptr, temp_ptr2 = NULL;
	unsigned chan, chan_index;
	boolean is_vec_store = FALSE;
	enum tgsi_opcode_type dtype = tgsi_opcode_infer_dst_type(inst->Instruction.Opcode);

	if (dst[0]) {
		LLVMTypeKind k = LLVMGetTypeKind(LLVMTypeOf(dst[0]));
		is_vec_store = (k == LLVMVectorTypeKind);
	}

	if (is_vec_store) {
		LLVMValueRef values[4] = {};
		TGSI_FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan) {
			LLVMValueRef index = lp_build_const_int32(gallivm, chan);
			values[chan]  = LLVMBuildExtractElement(gallivm->builder,
							dst[0], index, "");
		}
		bld_base->emit_store(bld_base, inst, info, values);
		return;
	}

	TGSI_FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
		LLVMValueRef value = dst[chan_index];

		if (dtype == TGSI_TYPE_DOUBLE && (chan_index == 1 || chan_index == 3))
			continue;
		if (inst->Instruction.Saturate)
			value = radeon_llvm_saturate(bld_base, value);

		if (reg->Register.File == TGSI_FILE_ADDRESS) {
			temp_ptr = bld->addr[reg->Register.Index][chan_index];
			LLVMBuildStore(builder, value, temp_ptr);
			continue;
		}

		if (dtype != TGSI_TYPE_DOUBLE)
			value = bitcast(bld_base, TGSI_TYPE_FLOAT, value);

		if (reg->Register.Indirect) {
			struct tgsi_declaration_range range = get_array_range(bld_base,
				reg->Register.File, &reg->Indirect);

        		unsigned i, size = range.Last - range.First + 1;
			LLVMValueRef array = LLVMBuildInsertElement(builder,
				emit_array_fetch(bld_base, reg->Register.File, TGSI_TYPE_FLOAT, range, chan_index),
				value,  emit_array_index(bld, &reg->Indirect, reg->Register.Index - range.First), "");

        		for (i = 0; i < size; ++i) {
				switch(reg->Register.File) {
				case TGSI_FILE_OUTPUT:
					temp_ptr = bld->outputs[i + range.First][chan_index];
					break;

				case TGSI_FILE_TEMPORARY:
					if (range.First + i >= ctx->temps_count)
						continue;
					if (uses_temp_indirect_addressing(bld_base))
						temp_ptr = lp_get_temp_ptr_soa(bld, i + range.First, chan_index);
					else
						temp_ptr = ctx->temps[(i + range.First) * TGSI_NUM_CHANNELS + chan_index];
					break;

				default:
					return;
				}
				value = LLVMBuildExtractElement(builder, array, 
					lp_build_const_int32(gallivm, i), "");
				LLVMBuildStore(builder, value, temp_ptr);
			}

		} else {
			switch(reg->Register.File) {
			case TGSI_FILE_OUTPUT:
				temp_ptr = bld->outputs[reg->Register.Index][chan_index];
				if (dtype == TGSI_TYPE_DOUBLE)
					temp_ptr2 = bld->outputs[reg->Register.Index][chan_index + 1];
				break;

			case TGSI_FILE_TEMPORARY:
				if (reg->Register.Index >= ctx->temps_count)
					continue;
				if (uses_temp_indirect_addressing(bld_base)) {
					temp_ptr = NULL;
					break;
				}
				temp_ptr = ctx->temps[ TGSI_NUM_CHANNELS * reg->Register.Index + chan_index];
				if (dtype == TGSI_TYPE_DOUBLE)
					temp_ptr2 = ctx->temps[ TGSI_NUM_CHANNELS * reg->Register.Index + chan_index + 1];

				break;

			default:
				return;
			}
			if (dtype != TGSI_TYPE_DOUBLE)
				LLVMBuildStore(builder, value, temp_ptr);
			else {
				LLVMValueRef ptr = LLVMBuildBitCast(builder, value,
								    LLVMVectorType(LLVMIntTypeInContext(bld_base->base.gallivm->context, 32), 2), "");
				LLVMValueRef val2;
				value = LLVMBuildExtractElement(builder, ptr,
								bld_base->uint_bld.zero, "");
				val2 = LLVMBuildExtractElement(builder, ptr,
								bld_base->uint_bld.one, "");

				LLVMBuildStore(builder, bitcast(bld_base, TGSI_TYPE_FLOAT, value), temp_ptr);
				LLVMBuildStore(builder, bitcast(bld_base, TGSI_TYPE_FLOAT, val2), temp_ptr2);
			}
		}
	}
}

static void bgnloop_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	LLVMBasicBlockRef loop_block;
	LLVMBasicBlockRef endloop_block;
	endloop_block = LLVMAppendBasicBlockInContext(gallivm->context,
						ctx->main_fn, "ENDLOOP");
	loop_block = LLVMInsertBasicBlockInContext(gallivm->context,
						endloop_block, "LOOP");
	LLVMBuildBr(gallivm->builder, loop_block);
	LLVMPositionBuilderAtEnd(gallivm->builder, loop_block);

	if (++ctx->loop_depth > ctx->loop_depth_max) {
		unsigned new_max = ctx->loop_depth_max << 1;

		if (!new_max)
			new_max = RADEON_LLVM_INITIAL_CF_DEPTH;

		ctx->loop = REALLOC(ctx->loop, ctx->loop_depth_max *
				    sizeof(ctx->loop[0]),
				    new_max * sizeof(ctx->loop[0]));
		ctx->loop_depth_max = new_max;
	}

	ctx->loop[ctx->loop_depth - 1].loop_block = loop_block;
	ctx->loop[ctx->loop_depth - 1].endloop_block = endloop_block;
}

static void brk_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	struct radeon_llvm_loop * current_loop = get_current_loop(ctx);

	LLVMBuildBr(gallivm->builder, current_loop->endloop_block);
}

static void cont_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	struct radeon_llvm_loop * current_loop = get_current_loop(ctx);

	LLVMBuildBr(gallivm->builder, current_loop->loop_block);
}

static void else_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	struct radeon_llvm_branch * current_branch = get_current_branch(ctx);
	LLVMBasicBlockRef current_block = LLVMGetInsertBlock(gallivm->builder);

	/* We need to add a terminator to the current block if the previous
	 * instruction was an ENDIF.Example:
	 * IF
	 *   [code]
	 *   IF
	 *     [code]
	 *   ELSE
	 *    [code]
	 *   ENDIF <--
	 * ELSE<--
	 *   [code]
	 * ENDIF
	 */

	if (current_block != current_branch->if_block) {
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}
	if (!LLVMGetBasicBlockTerminator(current_branch->if_block)) {
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}
	current_branch->has_else = 1;
	LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->else_block);
}

static void endif_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	struct radeon_llvm_branch * current_branch = get_current_branch(ctx);
	LLVMBasicBlockRef current_block = LLVMGetInsertBlock(gallivm->builder);

	/* If we have consecutive ENDIF instructions, then the first ENDIF
	 * will not have a terminator, so we need to add one. */
	if (current_block != current_branch->if_block
			&& current_block != current_branch->else_block
			&& !LLVMGetBasicBlockTerminator(current_block)) {

		 LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}
	if (!LLVMGetBasicBlockTerminator(current_branch->else_block)) {
		LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->else_block);
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}

	if (!LLVMGetBasicBlockTerminator(current_branch->if_block)) {
		LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->if_block);
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}

	LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->endif_block);
	ctx->branch_depth--;
}

static void endloop_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	struct radeon_llvm_loop * current_loop = get_current_loop(ctx);

	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(gallivm->builder))) {
		 LLVMBuildBr(gallivm->builder, current_loop->loop_block);
	}

	LLVMPositionBuilderAtEnd(gallivm->builder, current_loop->endloop_block);
	ctx->loop_depth--;
}

static void if_cond_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data,
	LLVMValueRef cond)
{
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	LLVMBasicBlockRef if_block, else_block, endif_block;

	endif_block = LLVMAppendBasicBlockInContext(gallivm->context,
						ctx->main_fn, "ENDIF");
	if_block = LLVMInsertBasicBlockInContext(gallivm->context,
						endif_block, "IF");
	else_block = LLVMInsertBasicBlockInContext(gallivm->context,
						endif_block, "ELSE");
	LLVMBuildCondBr(gallivm->builder, cond, if_block, else_block);
	LLVMPositionBuilderAtEnd(gallivm->builder, if_block);

	if (++ctx->branch_depth > ctx->branch_depth_max) {
		unsigned new_max = ctx->branch_depth_max << 1;

		if (!new_max)
			new_max = RADEON_LLVM_INITIAL_CF_DEPTH;

		ctx->branch = REALLOC(ctx->branch, ctx->branch_depth_max *
				      sizeof(ctx->branch[0]),
				      new_max * sizeof(ctx->branch[0]));
		ctx->branch_depth_max = new_max;
	}

	ctx->branch[ctx->branch_depth - 1].endif_block = endif_block;
	ctx->branch[ctx->branch_depth - 1].if_block = if_block;
	ctx->branch[ctx->branch_depth - 1].else_block = else_block;
	ctx->branch[ctx->branch_depth - 1].has_else = 0;
}

static void if_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	LLVMValueRef cond;

	cond = LLVMBuildFCmp(gallivm->builder, LLVMRealUNE,
			emit_data->args[0],
			bld_base->base.zero, "");

	if_cond_emit(action, bld_base, emit_data, cond);
}

static void uif_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	LLVMValueRef cond;

	cond = LLVMBuildICmp(gallivm->builder, LLVMIntNE,
	        bitcast(bld_base, TGSI_TYPE_UNSIGNED, emit_data->args[0]),
			bld_base->int_bld.zero, "");

	if_cond_emit(action, bld_base, emit_data, cond);
}

static void kill_if_fetch_args(
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	const struct tgsi_full_instruction * inst = emit_data->inst;
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	unsigned i;
	LLVMValueRef conds[TGSI_NUM_CHANNELS];

	for (i = 0; i < TGSI_NUM_CHANNELS; i++) {
		LLVMValueRef value = lp_build_emit_fetch(bld_base, inst, 0, i);
		conds[i] = LLVMBuildFCmp(builder, LLVMRealOLT, value,
					bld_base->base.zero, "");
	}

	/* Or the conditions together */
	for (i = TGSI_NUM_CHANNELS - 1; i > 0; i--) {
		conds[i - 1] = LLVMBuildOr(builder, conds[i], conds[i - 1], "");
	}

	emit_data->dst_type = LLVMVoidTypeInContext(gallivm->context);
	emit_data->arg_count = 1;
	emit_data->args[0] = LLVMBuildSelect(builder, conds[0],
					lp_build_const_float(gallivm, -1.0f),
					bld_base->base.zero, "");
}

static void kil_emit(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	unsigned i;
	for (i = 0; i < emit_data->arg_count; i++) {
		emit_data->output[i] = lp_build_intrinsic_unary(
			bld_base->base.gallivm->builder,
			action->intr_name,
			emit_data->dst_type, emit_data->args[i]);
	}
}

static void radeon_llvm_cube_to_2d_coords(struct lp_build_tgsi_context *bld_base,
					  LLVMValueRef *in, LLVMValueRef *out)
{
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMTypeRef type = bld_base->base.elem_type;
	LLVMValueRef coords[4];
	LLVMValueRef mad_args[3];
	LLVMValueRef v, cube_vec;
	unsigned i;

	cube_vec = lp_build_gather_values(bld_base->base.gallivm, in, 4);
	v = lp_build_intrinsic(builder, "llvm.AMDGPU.cube", LLVMVectorType(type, 4),
                            &cube_vec, 1, LLVMReadNoneAttribute);

	for (i = 0; i < 4; ++i)
		coords[i] = LLVMBuildExtractElement(builder, v,
						    lp_build_const_int32(gallivm, i), "");

	coords[2] = lp_build_intrinsic(builder, "llvm.fabs.f32",
			type, &coords[2], 1, LLVMReadNoneAttribute);
	coords[2] = lp_build_emit_llvm_unary(bld_base, TGSI_OPCODE_RCP, coords[2]);

	mad_args[1] = coords[2];
	mad_args[2] = LLVMConstReal(type, 1.5);

	mad_args[0] = coords[0];
	coords[0] = lp_build_emit_llvm_ternary(bld_base, TGSI_OPCODE_MAD,
			mad_args[0], mad_args[1], mad_args[2]);

	mad_args[0] = coords[1];
	coords[1] = lp_build_emit_llvm_ternary(bld_base, TGSI_OPCODE_MAD,
			mad_args[0], mad_args[1], mad_args[2]);

	/* apply xyz = yxw swizzle to cooords */
	out[0] = coords[1];
	out[1] = coords[0];
	out[2] = coords[3];
}

void radeon_llvm_emit_prepare_cube_coords(
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data,
		LLVMValueRef *coords_arg,
		LLVMValueRef *derivs_arg)
{

	unsigned target = emit_data->inst->Texture.Texture;
	unsigned opcode = emit_data->inst->Instruction.Opcode;
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef coords[4];
	unsigned i;

	radeon_llvm_cube_to_2d_coords(bld_base, coords_arg, coords);

	if (opcode == TGSI_OPCODE_TXD && derivs_arg) {
		LLVMValueRef derivs[4];
		int axis;

		/* Convert cube derivatives to 2D derivatives. */
		for (axis = 0; axis < 2; axis++) {
			LLVMValueRef shifted_cube_coords[4], shifted_coords[4];

			/* Shift the cube coordinates by the derivatives to get
			 * the cube coordinates of the "neighboring pixel".
			 */
			for (i = 0; i < 3; i++)
				shifted_cube_coords[i] =
					LLVMBuildFAdd(builder, coords_arg[i],
						      derivs_arg[axis*3+i], "");
			shifted_cube_coords[3] = LLVMGetUndef(bld_base->base.elem_type);

			/* Project the shifted cube coordinates onto the face. */
			radeon_llvm_cube_to_2d_coords(bld_base, shifted_cube_coords,
						      shifted_coords);

			/* Subtract both sets of 2D coordinates to get 2D derivatives.
			 * This won't work if the shifted coordinates ended up
			 * in a different face.
			 */
			for (i = 0; i < 2; i++)
				derivs[axis * 2 + i] =
					LLVMBuildFSub(builder, shifted_coords[i],
						      coords[i], "");
		}

		memcpy(derivs_arg, derivs, sizeof(derivs));
	}

	if (target == TGSI_TEXTURE_CUBE_ARRAY ||
	    target == TGSI_TEXTURE_SHADOWCUBE_ARRAY) {
		/* for cube arrays coord.z = coord.w(array_index) * 8 + face */
		/* coords_arg.w component - array_index for cube arrays */
		coords[2] = lp_build_emit_llvm_ternary(bld_base, TGSI_OPCODE_MAD,
						       coords_arg[3], lp_build_const_float(gallivm, 8.0), coords[2]);
	}

	/* Preserve compare/lod/bias. Put it in coords.w. */
	if (opcode == TGSI_OPCODE_TEX2 ||
	    opcode == TGSI_OPCODE_TXB2 ||
	    opcode == TGSI_OPCODE_TXL2) {
		coords[3] = coords_arg[4];
	} else if (opcode == TGSI_OPCODE_TXB ||
		   opcode == TGSI_OPCODE_TXL ||
		   target == TGSI_TEXTURE_SHADOWCUBE) {
		coords[3] = coords_arg[3];
	}

	memcpy(coords_arg, coords, sizeof(coords));
}

static void emit_icmp(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	unsigned pred;
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;

	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_USEQ: pred = LLVMIntEQ; break;
	case TGSI_OPCODE_USNE: pred = LLVMIntNE; break;
	case TGSI_OPCODE_USGE: pred = LLVMIntUGE; break;
	case TGSI_OPCODE_USLT: pred = LLVMIntULT; break;
	case TGSI_OPCODE_ISGE: pred = LLVMIntSGE; break;
	case TGSI_OPCODE_ISLT: pred = LLVMIntSLT; break;
	default:
		assert(!"unknown instruction");
		pred = 0;
		break;
	}

	LLVMValueRef v = LLVMBuildICmp(builder, pred,
			emit_data->args[0], emit_data->args[1],"");

	v = LLVMBuildSExtOrBitCast(builder, v,
			LLVMInt32TypeInContext(context), "");

	emit_data->output[emit_data->chan] = v;
}

static void emit_ucmp(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	LLVMValueRef arg0 = LLVMBuildBitCast(builder, emit_data->args[0],
					     bld_base->uint_bld.elem_type, "");

	LLVMValueRef v = LLVMBuildICmp(builder, LLVMIntNE, arg0,
				       bld_base->uint_bld.zero, "");

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder, v, emit_data->args[1], emit_data->args[2], "");
}

static void emit_cmp(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef cond, *args = emit_data->args;

	cond = LLVMBuildFCmp(builder, LLVMRealOLT, args[0],
			     bld_base->base.zero, "");

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder, cond, args[1], args[2], "");
}

static void emit_set_cond(
		const struct lp_build_tgsi_action *action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMRealPredicate pred;
	LLVMValueRef cond;

	/* Use ordered for everything but NE (which is usual for
	 * float comparisons)
	 */
	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_SGE: pred = LLVMRealOGE; break;
	case TGSI_OPCODE_SEQ: pred = LLVMRealOEQ; break;
	case TGSI_OPCODE_SLE: pred = LLVMRealOLE; break;
	case TGSI_OPCODE_SLT: pred = LLVMRealOLT; break;
	case TGSI_OPCODE_SNE: pred = LLVMRealUNE; break;
	case TGSI_OPCODE_SGT: pred = LLVMRealOGT; break;
	default: assert(!"unknown instruction"); pred = 0; break;
	}

	cond = LLVMBuildFCmp(builder,
		pred, emit_data->args[0], emit_data->args[1], "");

	emit_data->output[emit_data->chan] = LLVMBuildSelect(builder,
		cond, bld_base->base.one, bld_base->base.zero, "");
}

static void emit_fcmp(
		const struct lp_build_tgsi_action *action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	LLVMRealPredicate pred;

	/* Use ordered for everything but NE (which is usual for
	 * float comparisons)
	 */
	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_FSEQ: pred = LLVMRealOEQ; break;
	case TGSI_OPCODE_FSGE: pred = LLVMRealOGE; break;
	case TGSI_OPCODE_FSLT: pred = LLVMRealOLT; break;
	case TGSI_OPCODE_FSNE: pred = LLVMRealUNE; break;
	default: assert(!"unknown instruction"); pred = 0; break;
	}

	LLVMValueRef v = LLVMBuildFCmp(builder, pred,
			emit_data->args[0], emit_data->args[1],"");

	v = LLVMBuildSExtOrBitCast(builder, v,
			LLVMInt32TypeInContext(context), "");

	emit_data->output[emit_data->chan] = v;
}

static void emit_dcmp(
		const struct lp_build_tgsi_action *action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	LLVMRealPredicate pred;

	/* Use ordered for everything but NE (which is usual for
	 * float comparisons)
	 */
	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_DSEQ: pred = LLVMRealOEQ; break;
	case TGSI_OPCODE_DSGE: pred = LLVMRealOGE; break;
	case TGSI_OPCODE_DSLT: pred = LLVMRealOLT; break;
	case TGSI_OPCODE_DSNE: pred = LLVMRealUNE; break;
	default: assert(!"unknown instruction"); pred = 0; break;
	}

	LLVMValueRef v = LLVMBuildFCmp(builder, pred,
			emit_data->args[0], emit_data->args[1],"");

	v = LLVMBuildSExtOrBitCast(builder, v,
			LLVMInt32TypeInContext(context), "");

	emit_data->output[emit_data->chan] = v;
}

static void emit_not(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef v = bitcast(bld_base, TGSI_TYPE_UNSIGNED,
			emit_data->args[0]);
	emit_data->output[emit_data->chan] = LLVMBuildNot(builder, v, "");
}

static void emit_arl(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef floor_index =  lp_build_emit_llvm_unary(bld_base, TGSI_OPCODE_FLR, emit_data->args[0]);
	emit_data->output[emit_data->chan] = LLVMBuildFPToSI(builder,
			floor_index, bld_base->base.int_elem_type , "");
}

static void emit_and(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildAnd(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_or(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildOr(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_uadd(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildAdd(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_udiv(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildUDiv(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_idiv(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildSDiv(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_mod(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildSRem(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_umod(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildURem(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_shl(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildShl(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_ushr(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildLShr(builder,
			emit_data->args[0], emit_data->args[1], "");
}
static void emit_ishr(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildAShr(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_xor(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildXor(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_ssg(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	LLVMValueRef cmp, val;

	if (emit_data->inst->Instruction.Opcode == TGSI_OPCODE_ISSG) {
		cmp = LLVMBuildICmp(builder, LLVMIntSGT, emit_data->args[0], bld_base->int_bld.zero, "");
		val = LLVMBuildSelect(builder, cmp, bld_base->int_bld.one, emit_data->args[0], "");
		cmp = LLVMBuildICmp(builder, LLVMIntSGE, val, bld_base->int_bld.zero, "");
		val = LLVMBuildSelect(builder, cmp, val, LLVMConstInt(bld_base->int_bld.elem_type, -1, true), "");
	} else { // float SSG
		cmp = LLVMBuildFCmp(builder, LLVMRealOGT, emit_data->args[0], bld_base->base.zero, "");
		val = LLVMBuildSelect(builder, cmp, bld_base->base.one, emit_data->args[0], "");
		cmp = LLVMBuildFCmp(builder, LLVMRealOGE, val, bld_base->base.zero, "");
		val = LLVMBuildSelect(builder, cmp, val, LLVMConstReal(bld_base->base.elem_type, -1), "");
	}

	emit_data->output[emit_data->chan] = val;
}

static void emit_ineg(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildNeg(builder,
			emit_data->args[0], "");
}

static void emit_dneg(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildFNeg(builder,
			emit_data->args[0], "");
}

static void emit_frac(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	char *intr;

	if (emit_data->info->opcode == TGSI_OPCODE_FRC)
		intr = "llvm.floor.f32";
	else if (emit_data->info->opcode == TGSI_OPCODE_DFRAC)
		intr = "llvm.floor.f64";
	else {
		assert(0);
		return;
	}

	LLVMValueRef floor = lp_build_intrinsic(builder, intr, emit_data->dst_type,
						&emit_data->args[0], 1,
						LLVMReadNoneAttribute);
	emit_data->output[emit_data->chan] = LLVMBuildFSub(builder,
			emit_data->args[0], floor, "");
}

static void emit_f2i(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildFPToSI(builder,
			emit_data->args[0], bld_base->int_bld.elem_type, "");
}

static void emit_f2u(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildFPToUI(builder,
			emit_data->args[0], bld_base->uint_bld.elem_type, "");
}

static void emit_i2f(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildSIToFP(builder,
			emit_data->args[0], bld_base->base.elem_type, "");
}

static void emit_u2f(
		const struct lp_build_tgsi_action * action,
		struct lp_build_tgsi_context * bld_base,
		struct lp_build_emit_data * emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildUIToFP(builder,
			emit_data->args[0], bld_base->base.elem_type, "");
}

static void emit_immediate(struct lp_build_tgsi_context * bld_base,
		const struct tgsi_full_immediate *imm)
{
	unsigned i;
	struct radeon_llvm_context * ctx = radeon_llvm_context(bld_base);

	for (i = 0; i < 4; ++i) {
		ctx->soa.immediates[ctx->soa.num_immediates][i] =
				LLVMConstInt(bld_base->uint_bld.elem_type, imm->u[i].Uint, false   );
	}

	ctx->soa.num_immediates++;
}

void
build_tgsi_intrinsic_nomem(const struct lp_build_tgsi_action *action,
			   struct lp_build_tgsi_context *bld_base,
			   struct lp_build_emit_data *emit_data)
{
	struct lp_build_context * base = &bld_base->base;
	emit_data->output[emit_data->chan] =
		lp_build_intrinsic(base->gallivm->builder, action->intr_name,
				   emit_data->dst_type, emit_data->args,
				   emit_data->arg_count, LLVMReadNoneAttribute);
}

static void emit_bfi(const struct lp_build_tgsi_action * action,
		     struct lp_build_tgsi_context * bld_base,
		     struct lp_build_emit_data * emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef bfi_args[3];

	// Calculate the bitmask: (((1 << src3) - 1) << src2
	bfi_args[0] = LLVMBuildShl(builder,
				   LLVMBuildSub(builder,
						LLVMBuildShl(builder,
							     bld_base->int_bld.one,
							     emit_data->args[3], ""),
						bld_base->int_bld.one, ""),
				   emit_data->args[2], "");

	bfi_args[1] = LLVMBuildShl(builder, emit_data->args[1],
				   emit_data->args[2], "");

	bfi_args[2] = emit_data->args[0];

	/* Calculate:
	 *   (arg0 & arg1) | (~arg0 & arg2) = arg2 ^ (arg0 & (arg1 ^ arg2)
	 * Use the right-hand side, which the LLVM backend can convert to V_BFI.
	 */
	emit_data->output[emit_data->chan] =
		LLVMBuildXor(builder, bfi_args[2],
			LLVMBuildAnd(builder, bfi_args[0],
				LLVMBuildXor(builder, bfi_args[1], bfi_args[2],
					     ""), ""), "");
}

/* this is ffs in C */
static void emit_lsb(const struct lp_build_tgsi_action * action,
		     struct lp_build_tgsi_context * bld_base,
		     struct lp_build_emit_data * emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMValueRef args[2] = {
		emit_data->args[0],

		/* The value of 1 means that ffs(x=0) = undef, so LLVM won't
		 * add special code to check for x=0. The reason is that
		 * the LLVM behavior for x=0 is different from what we
		 * need here.
		 *
		 * The hardware already implements the correct behavior.
		 */
		lp_build_const_int32(gallivm, 1)
	};

	emit_data->output[emit_data->chan] =
		lp_build_intrinsic(gallivm->builder, "llvm.cttz.i32",
				emit_data->dst_type, args, Elements(args),
				LLVMReadNoneAttribute);
}

/* Find the last bit set. */
static void emit_umsb(const struct lp_build_tgsi_action * action,
		      struct lp_build_tgsi_context * bld_base,
		      struct lp_build_emit_data * emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef args[2] = {
		emit_data->args[0],
		/* Don't generate code for handling zero: */
		lp_build_const_int32(gallivm, 1)
	};

	LLVMValueRef msb =
		lp_build_intrinsic(builder, "llvm.ctlz.i32",
				emit_data->dst_type, args, Elements(args),
				LLVMReadNoneAttribute);

	/* The HW returns the last bit index from MSB, but TGSI wants
	 * the index from LSB. Invert it by doing "31 - msb". */
	msb = LLVMBuildSub(builder, lp_build_const_int32(gallivm, 31),
			   msb, "");

	/* Check for zero: */
	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder,
				LLVMBuildICmp(builder, LLVMIntEQ, args[0],
					      bld_base->uint_bld.zero, ""),
				lp_build_const_int32(gallivm, -1), msb, "");
}

/* Find the last bit opposite of the sign bit. */
static void emit_imsb(const struct lp_build_tgsi_action * action,
		     struct lp_build_tgsi_context * bld_base,
		     struct lp_build_emit_data * emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef arg = emit_data->args[0];

	LLVMValueRef msb =
		lp_build_intrinsic(builder, "llvm.AMDGPU.flbit.i32",
				emit_data->dst_type, &arg, 1,
				LLVMReadNoneAttribute);

	/* The HW returns the last bit index from MSB, but TGSI wants
	 * the index from LSB. Invert it by doing "31 - msb". */
	msb = LLVMBuildSub(builder, lp_build_const_int32(gallivm, 31),
			   msb, "");

	/* If arg == 0 || arg == -1 (0xffffffff), return -1. */
	LLVMValueRef all_ones = lp_build_const_int32(gallivm, -1);

	LLVMValueRef cond =
		LLVMBuildOr(builder,
			    LLVMBuildICmp(builder, LLVMIntEQ, arg,
					  bld_base->uint_bld.zero, ""),
			    LLVMBuildICmp(builder, LLVMIntEQ, arg,
					  all_ones, ""), "");

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder, cond, all_ones, msb, "");
}

static void emit_iabs(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	emit_data->output[emit_data->chan] =
		lp_build_emit_llvm_binary(bld_base, TGSI_OPCODE_IMAX,
					  emit_data->args[0],
					  LLVMBuildNeg(builder,
						       emit_data->args[0], ""));
}

static void emit_minmax_int(const struct lp_build_tgsi_action *action,
			    struct lp_build_tgsi_context *bld_base,
			    struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMIntPredicate op;

	switch (emit_data->info->opcode) {
	default:
		assert(0);
	case TGSI_OPCODE_IMAX:
		op = LLVMIntSGT;
		break;
	case TGSI_OPCODE_IMIN:
		op = LLVMIntSLT;
		break;
	case TGSI_OPCODE_UMAX:
		op = LLVMIntUGT;
		break;
	case TGSI_OPCODE_UMIN:
		op = LLVMIntULT;
		break;
	}

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder,
				LLVMBuildICmp(builder, op, emit_data->args[0],
					      emit_data->args[1], ""),
				emit_data->args[0],
				emit_data->args[1], "");
}

static void pk2h_fetch_args(struct lp_build_tgsi_context * bld_base,
			    struct lp_build_emit_data * emit_data)
{
	emit_data->args[0] = lp_build_emit_fetch(bld_base, emit_data->inst,
						 0, TGSI_CHAN_X);
	emit_data->args[1] = lp_build_emit_fetch(bld_base, emit_data->inst,
						 0, TGSI_CHAN_Y);
}

static void emit_pk2h(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	struct lp_build_context *uint_bld = &bld_base->uint_bld;
	LLVMTypeRef fp16, i16;
	LLVMValueRef const16, comp[2];
	unsigned i;

	fp16 = LLVMHalfTypeInContext(context);
	i16 = LLVMInt16TypeInContext(context);
	const16 = lp_build_const_int32(uint_bld->gallivm, 16);

	for (i = 0; i < 2; i++) {
		comp[i] = LLVMBuildFPTrunc(builder, emit_data->args[i], fp16, "");
		comp[i] = LLVMBuildBitCast(builder, comp[i], i16, "");
		comp[i] = LLVMBuildZExt(builder, comp[i], uint_bld->elem_type, "");
	}

	comp[1] = LLVMBuildShl(builder, comp[1], const16, "");
	comp[0] = LLVMBuildOr(builder, comp[0], comp[1], "");

	emit_data->output[emit_data->chan] = comp[0];
}

static void up2h_fetch_args(struct lp_build_tgsi_context * bld_base,
			    struct lp_build_emit_data * emit_data)
{
	emit_data->args[0] = lp_build_emit_fetch(bld_base, emit_data->inst,
						 0, TGSI_CHAN_X);
}

static void emit_up2h(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	struct lp_build_context *uint_bld = &bld_base->uint_bld;
	LLVMTypeRef fp16, i16;
	LLVMValueRef const16, input, val;
	unsigned i;

	fp16 = LLVMHalfTypeInContext(context);
	i16 = LLVMInt16TypeInContext(context);
	const16 = lp_build_const_int32(uint_bld->gallivm, 16);
	input = emit_data->args[0];

	for (i = 0; i < 2; i++) {
		val = i == 1 ? LLVMBuildLShr(builder, input, const16, "") : input;
		val = LLVMBuildTrunc(builder, val, i16, "");
		val = LLVMBuildBitCast(builder, val, fp16, "");
		emit_data->output[i] =
			LLVMBuildFPExt(builder, val, bld_base->base.elem_type, "");
	}
}

void radeon_llvm_context_init(struct radeon_llvm_context * ctx, const char *triple)
{
	struct lp_type type;

	/* Initialize the gallivm object:
	 * We are only using the module, context, and builder fields of this struct.
	 * This should be enough for us to be able to pass our gallivm struct to the
	 * helper functions in the gallivm module.
	 */
	memset(&ctx->gallivm, 0, sizeof (ctx->gallivm));
	memset(&ctx->soa, 0, sizeof(ctx->soa));
	ctx->gallivm.context = LLVMContextCreate();
	ctx->gallivm.module = LLVMModuleCreateWithNameInContext("tgsi",
						ctx->gallivm.context);
	LLVMSetTarget(ctx->gallivm.module,

#if HAVE_LLVM < 0x0306
			"r600--");
#else
			triple);
#endif
	ctx->gallivm.builder = LLVMCreateBuilderInContext(ctx->gallivm.context);

	struct lp_build_tgsi_context * bld_base = &ctx->soa.bld_base;

	type.floating = TRUE;
	type.fixed = FALSE;
	type.sign = TRUE;
	type.norm = FALSE;
	type.width = 32;
	type.length = 1;

	lp_build_context_init(&bld_base->base, &ctx->gallivm, type);
	lp_build_context_init(&ctx->soa.bld_base.uint_bld, &ctx->gallivm, lp_uint_type(type));
	lp_build_context_init(&ctx->soa.bld_base.int_bld, &ctx->gallivm, lp_int_type(type));
	{
		struct lp_type dbl_type;
		dbl_type = type;
		dbl_type.width *= 2;
		lp_build_context_init(&ctx->soa.bld_base.dbl_bld, &ctx->gallivm, dbl_type);
	}

	bld_base->soa = 1;
	bld_base->emit_store = radeon_llvm_emit_store;
	bld_base->emit_swizzle = emit_swizzle;
	bld_base->emit_declaration = emit_declaration;
	bld_base->emit_immediate = emit_immediate;

	bld_base->emit_fetch_funcs[TGSI_FILE_IMMEDIATE] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_INPUT] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_TEMPORARY] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_OUTPUT] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_SYSTEM_VALUE] = fetch_system_value;

	/* Allocate outputs */
	ctx->soa.outputs = ctx->outputs;

	lp_set_default_actions(bld_base);

	bld_base->op_actions[TGSI_OPCODE_ABS].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_ABS].intr_name = "llvm.fabs.f32";
	bld_base->op_actions[TGSI_OPCODE_AND].emit = emit_and;
	bld_base->op_actions[TGSI_OPCODE_ARL].emit = emit_arl;
	bld_base->op_actions[TGSI_OPCODE_BFI].emit = emit_bfi;
	bld_base->op_actions[TGSI_OPCODE_BGNLOOP].emit = bgnloop_emit;
	bld_base->op_actions[TGSI_OPCODE_BREV].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_BREV].intr_name =
		HAVE_LLVM >= 0x0308 ? "llvm.bitreverse.i32" : "llvm.AMDGPU.brev";
	bld_base->op_actions[TGSI_OPCODE_BRK].emit = brk_emit;
	bld_base->op_actions[TGSI_OPCODE_CEIL].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_CEIL].intr_name = "llvm.ceil.f32";
	bld_base->op_actions[TGSI_OPCODE_CLAMP].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_CLAMP].intr_name =
		HAVE_LLVM >= 0x0308 ? "llvm.AMDGPU.clamp." : "llvm.AMDIL.clamp.";
	bld_base->op_actions[TGSI_OPCODE_CMP].emit = emit_cmp;
	bld_base->op_actions[TGSI_OPCODE_CONT].emit = cont_emit;
	bld_base->op_actions[TGSI_OPCODE_COS].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_COS].intr_name = "llvm.cos.f32";
	bld_base->op_actions[TGSI_OPCODE_DABS].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DABS].intr_name = "llvm.fabs.f64";
	bld_base->op_actions[TGSI_OPCODE_DFMA].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DFMA].intr_name = "llvm.fma.f64";
	bld_base->op_actions[TGSI_OPCODE_DFRAC].emit = emit_frac;
	bld_base->op_actions[TGSI_OPCODE_DNEG].emit = emit_dneg;
	bld_base->op_actions[TGSI_OPCODE_DSEQ].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DSGE].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DSLT].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DSNE].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DRSQ].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DRSQ].intr_name = "llvm.AMDGPU.rsq.f64";
	bld_base->op_actions[TGSI_OPCODE_DSQRT].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DSQRT].intr_name = "llvm.sqrt.f64";
	bld_base->op_actions[TGSI_OPCODE_ELSE].emit = else_emit;
	bld_base->op_actions[TGSI_OPCODE_ENDIF].emit = endif_emit;
	bld_base->op_actions[TGSI_OPCODE_ENDLOOP].emit = endloop_emit;
	bld_base->op_actions[TGSI_OPCODE_EX2].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_EX2].intr_name =
		HAVE_LLVM >= 0x0308 ? "llvm.exp2.f32" : "llvm.AMDIL.exp.";
	bld_base->op_actions[TGSI_OPCODE_FLR].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_FLR].intr_name = "llvm.floor.f32";
	bld_base->op_actions[TGSI_OPCODE_FMA].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_FMA].intr_name = "llvm.fma.f32";
	bld_base->op_actions[TGSI_OPCODE_FRC].emit = emit_frac;
	bld_base->op_actions[TGSI_OPCODE_F2I].emit = emit_f2i;
	bld_base->op_actions[TGSI_OPCODE_F2U].emit = emit_f2u;
	bld_base->op_actions[TGSI_OPCODE_FSEQ].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_FSGE].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_FSLT].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_FSNE].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_IABS].emit = emit_iabs;
	bld_base->op_actions[TGSI_OPCODE_IBFE].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_IBFE].intr_name = "llvm.AMDGPU.bfe.i32";
	bld_base->op_actions[TGSI_OPCODE_IDIV].emit = emit_idiv;
	bld_base->op_actions[TGSI_OPCODE_IF].emit = if_emit;
	bld_base->op_actions[TGSI_OPCODE_UIF].emit = uif_emit;
	bld_base->op_actions[TGSI_OPCODE_IMAX].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_IMIN].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_IMSB].emit = emit_imsb;
	bld_base->op_actions[TGSI_OPCODE_INEG].emit = emit_ineg;
	bld_base->op_actions[TGSI_OPCODE_ISHR].emit = emit_ishr;
	bld_base->op_actions[TGSI_OPCODE_ISGE].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_ISLT].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_ISSG].emit = emit_ssg;
	bld_base->op_actions[TGSI_OPCODE_I2F].emit = emit_i2f;
	bld_base->op_actions[TGSI_OPCODE_KILL_IF].fetch_args = kill_if_fetch_args;
	bld_base->op_actions[TGSI_OPCODE_KILL_IF].emit = kil_emit;
	bld_base->op_actions[TGSI_OPCODE_KILL_IF].intr_name = "llvm.AMDGPU.kill";
	bld_base->op_actions[TGSI_OPCODE_KILL].emit = lp_build_tgsi_intrinsic;
	bld_base->op_actions[TGSI_OPCODE_KILL].intr_name = "llvm.AMDGPU.kilp";
	bld_base->op_actions[TGSI_OPCODE_LSB].emit = emit_lsb;
	bld_base->op_actions[TGSI_OPCODE_LG2].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_LG2].intr_name = "llvm.log2.f32";
	bld_base->op_actions[TGSI_OPCODE_MOD].emit = emit_mod;
	bld_base->op_actions[TGSI_OPCODE_UMSB].emit = emit_umsb;
	bld_base->op_actions[TGSI_OPCODE_NOT].emit = emit_not;
	bld_base->op_actions[TGSI_OPCODE_OR].emit = emit_or;
	bld_base->op_actions[TGSI_OPCODE_PK2H].fetch_args = pk2h_fetch_args;
	bld_base->op_actions[TGSI_OPCODE_PK2H].emit = emit_pk2h;
	bld_base->op_actions[TGSI_OPCODE_POPC].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_POPC].intr_name = "llvm.ctpop.i32";
	bld_base->op_actions[TGSI_OPCODE_POW].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_POW].intr_name = "llvm.pow.f32";
	bld_base->op_actions[TGSI_OPCODE_ROUND].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_ROUND].intr_name = "llvm.rint.f32";
	bld_base->op_actions[TGSI_OPCODE_RSQ].intr_name = "llvm.AMDGPU.rsq.clamped.f32";
	bld_base->op_actions[TGSI_OPCODE_RSQ].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_SGE].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SEQ].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SHL].emit = emit_shl;
	bld_base->op_actions[TGSI_OPCODE_SLE].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SLT].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SNE].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SGT].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SIN].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_SIN].intr_name = "llvm.sin.f32";
	bld_base->op_actions[TGSI_OPCODE_SQRT].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_SQRT].intr_name = "llvm.sqrt.f32";
	bld_base->op_actions[TGSI_OPCODE_SSG].emit = emit_ssg;
	bld_base->op_actions[TGSI_OPCODE_TRUNC].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_TRUNC].intr_name = "llvm.trunc.f32";
	bld_base->op_actions[TGSI_OPCODE_UADD].emit = emit_uadd;
	bld_base->op_actions[TGSI_OPCODE_UBFE].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_UBFE].intr_name = "llvm.AMDGPU.bfe.u32";
	bld_base->op_actions[TGSI_OPCODE_UDIV].emit = emit_udiv;
	bld_base->op_actions[TGSI_OPCODE_UMAX].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_UMIN].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_UMOD].emit = emit_umod;
	bld_base->op_actions[TGSI_OPCODE_USEQ].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_USGE].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_USHR].emit = emit_ushr;
	bld_base->op_actions[TGSI_OPCODE_USLT].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_USNE].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_U2F].emit = emit_u2f;
	bld_base->op_actions[TGSI_OPCODE_XOR].emit = emit_xor;
	bld_base->op_actions[TGSI_OPCODE_UCMP].emit = emit_ucmp;
	bld_base->op_actions[TGSI_OPCODE_UP2H].fetch_args = up2h_fetch_args;
	bld_base->op_actions[TGSI_OPCODE_UP2H].emit = emit_up2h;
}

void radeon_llvm_create_func(struct radeon_llvm_context * ctx,
			     LLVMTypeRef *return_types, unsigned num_return_elems,
			     LLVMTypeRef *ParamTypes, unsigned ParamCount)
{
	LLVMTypeRef main_fn_type, ret_type;
	LLVMBasicBlockRef main_fn_body;

	if (num_return_elems)
		ret_type = LLVMStructTypeInContext(ctx->gallivm.context,
						   return_types,
						   num_return_elems, true);
	else
		ret_type = LLVMVoidTypeInContext(ctx->gallivm.context);

	/* Setup the function */
	ctx->return_type = ret_type;
	main_fn_type = LLVMFunctionType(ret_type, ParamTypes, ParamCount, 0);
	ctx->main_fn = LLVMAddFunction(ctx->gallivm.module, "main", main_fn_type);
	main_fn_body = LLVMAppendBasicBlockInContext(ctx->gallivm.context,
			ctx->main_fn, "main_body");
	LLVMPositionBuilderAtEnd(ctx->gallivm.builder, main_fn_body);
}

void radeon_llvm_finalize_module(struct radeon_llvm_context * ctx)
{
	struct gallivm_state * gallivm = ctx->soa.bld_base.base.gallivm;
	const char *triple = LLVMGetTarget(gallivm->module);
	LLVMTargetLibraryInfoRef target_library_info;

	/* Create the pass manager */
	gallivm->passmgr = LLVMCreateFunctionPassManagerForModule(
							gallivm->module);

	target_library_info = gallivm_create_target_library_info(triple);
	LLVMAddTargetLibraryInfo(target_library_info, gallivm->passmgr);

	/* This pass should eliminate all the load and store instructions */
	LLVMAddPromoteMemoryToRegisterPass(gallivm->passmgr);

	/* Add some optimization passes */
	LLVMAddScalarReplAggregatesPass(gallivm->passmgr);
	LLVMAddLICMPass(gallivm->passmgr);
	LLVMAddAggressiveDCEPass(gallivm->passmgr);
	LLVMAddCFGSimplificationPass(gallivm->passmgr);
	LLVMAddInstructionCombiningPass(gallivm->passmgr);

	/* Run the pass */
	LLVMRunFunctionPassManager(gallivm->passmgr, ctx->main_fn);

	LLVMDisposeBuilder(gallivm->builder);
	LLVMDisposePassManager(gallivm->passmgr);
	gallivm_dispose_target_library_info(target_library_info);
}

void radeon_llvm_dispose(struct radeon_llvm_context * ctx)
{
	LLVMDisposeModule(ctx->soa.bld_base.base.gallivm->module);
	LLVMContextDispose(ctx->soa.bld_base.base.gallivm->context);
	FREE(ctx->arrays);
	ctx->arrays = NULL;
	FREE(ctx->temps);
	ctx->temps = NULL;
	ctx->temps_count = 0;
	FREE(ctx->loop);
	ctx->loop = NULL;
	ctx->loop_depth_max = 0;
	FREE(ctx->branch);
	ctx->branch = NULL;
	ctx->branch_depth_max = 0;
}
