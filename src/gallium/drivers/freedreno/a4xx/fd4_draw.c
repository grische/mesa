/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2014 Rob Clark <robclark@freedesktop.org>
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
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

#include "freedreno_state.h"
#include "freedreno_resource.h"

#include "fd4_draw.h"
#include "fd4_context.h"
#include "fd4_emit.h"
#include "fd4_program.h"
#include "fd4_format.h"
#include "fd4_zsa.h"


static void
draw_impl(struct fd_context *ctx, struct fd_ringbuffer *ring,
		struct fd4_emit *emit)
{
	const struct pipe_draw_info *info = emit->info;
	enum pc_di_primtype primtype = ctx->primtypes[info->mode];

	if (!(fd4_emit_get_vp(emit) && fd4_emit_get_fp(emit)))
		return;

	fd4_emit_state(ctx, ring, emit);

	if (emit->dirty & (FD_DIRTY_VTXBUF | FD_DIRTY_VTXSTATE))
		fd4_emit_vertex_bufs(ring, emit);

	OUT_PKT0(ring, REG_A4XX_VFD_INDEX_OFFSET, 2);
	OUT_RING(ring, info->indexed ? info->index_bias : info->start); /* VFD_INDEX_OFFSET */
	OUT_RING(ring, info->start_instance);   /* ??? UNKNOWN_2209 */

	OUT_PKT0(ring, REG_A4XX_PC_RESTART_INDEX, 1);
	OUT_RING(ring, info->primitive_restart ? /* PC_RESTART_INDEX */
			info->restart_index : 0xffffffff);

	/* points + psize -> spritelist: */
	if (ctx->rasterizer->point_size_per_vertex &&
			fd4_emit_get_vp(emit)->writes_psize &&
			(info->mode == PIPE_PRIM_POINTS))
		primtype = DI_PT_POINTLIST_PSIZE;

	fd4_draw_emit(ctx, ring,
			primtype,
			emit->key.binning_pass ? IGNORE_VISIBILITY : USE_VISIBILITY,
			info);
}

/* fixup dirty shader state in case some "unrelated" (from the state-
 * tracker's perspective) state change causes us to switch to a
 * different variant.
 */
static void
fixup_shader_state(struct fd_context *ctx, struct ir3_shader_key *key)
{
	struct fd4_context *fd4_ctx = fd4_context(ctx);
	struct ir3_shader_key *last_key = &fd4_ctx->last_key;

	if (!ir3_shader_key_equal(last_key, key)) {
		ctx->dirty |= FD_DIRTY_PROG;

		if (last_key->has_per_samp || key->has_per_samp) {
			if ((last_key->vsaturate_s != key->vsaturate_s) ||
					(last_key->vsaturate_t != key->vsaturate_t) ||
					(last_key->vsaturate_r != key->vsaturate_r) ||
					(last_key->vastc_srgb != key->vastc_srgb))
				ctx->prog.dirty |= FD_SHADER_DIRTY_VP;

			if ((last_key->fsaturate_s != key->fsaturate_s) ||
					(last_key->fsaturate_t != key->fsaturate_t) ||
					(last_key->fsaturate_r != key->fsaturate_r) ||
					(last_key->fastc_srgb != key->fastc_srgb))
				ctx->prog.dirty |= FD_SHADER_DIRTY_FP;
		}

		if (last_key->color_two_side != key->color_two_side)
			ctx->prog.dirty |= FD_SHADER_DIRTY_FP;

		if (last_key->half_precision != key->half_precision)
			ctx->prog.dirty |= FD_SHADER_DIRTY_FP;

		if (last_key->rasterflat != key->rasterflat)
			ctx->prog.dirty |= FD_SHADER_DIRTY_FP;

		fd4_ctx->last_key = *key;
	}
}

static void
fd4_draw_vbo(struct fd_context *ctx, const struct pipe_draw_info *info)
{
	struct fd4_context *fd4_ctx = fd4_context(ctx);
	struct fd4_emit emit = {
		.vtx  = &ctx->vtx,
		.prog = &ctx->prog,
		.info = info,
		.key = {
			/* do binning pass first: */
			.binning_pass = true,
			.color_two_side = ctx->rasterizer->light_twoside,
			.rasterflat = ctx->rasterizer->flatshade,
			// TODO set .half_precision based on render target format,
			// ie. float16 and smaller use half, float32 use full..
			.half_precision = !!(fd_mesa_debug & FD_DBG_FRAGHALF),
			.ucp_enables = ctx->rasterizer->clip_plane_enable,
			.has_per_samp = (fd4_ctx->fsaturate || fd4_ctx->vsaturate ||
					fd4_ctx->fastc_srgb || fd4_ctx->vastc_srgb),
			.vsaturate_s = fd4_ctx->vsaturate_s,
			.vsaturate_t = fd4_ctx->vsaturate_t,
			.vsaturate_r = fd4_ctx->vsaturate_r,
			.fsaturate_s = fd4_ctx->fsaturate_s,
			.fsaturate_t = fd4_ctx->fsaturate_t,
			.fsaturate_r = fd4_ctx->fsaturate_r,
			.vastc_srgb = fd4_ctx->vastc_srgb,
			.fastc_srgb = fd4_ctx->fastc_srgb,
		},
		.rasterflat = ctx->rasterizer->flatshade,
		.sprite_coord_enable = ctx->rasterizer->sprite_coord_enable,
		.sprite_coord_mode = ctx->rasterizer->sprite_coord_mode,
	};
	unsigned dirty;

	fixup_shader_state(ctx, &emit.key);

	dirty = ctx->dirty;
	emit.dirty = dirty & ~(FD_DIRTY_BLEND);
	draw_impl(ctx, ctx->binning_ring, &emit);

	/* and now regular (non-binning) pass: */
	emit.key.binning_pass = false;
	emit.dirty = dirty;
	emit.vp = NULL;   /* we changed key so need to refetch vp */
	emit.fp = NULL;

	if (ctx->rasterizer->rasterizer_discard) {
		fd_wfi(ctx, ctx->ring);
		OUT_PKT3(ctx->ring, CP_REG_RMW, 3);
		OUT_RING(ctx->ring, REG_A4XX_RB_RENDER_CONTROL);
		OUT_RING(ctx->ring, ~A4XX_RB_RENDER_CONTROL_DISABLE_COLOR_PIPE);
		OUT_RING(ctx->ring, A4XX_RB_RENDER_CONTROL_DISABLE_COLOR_PIPE);
	}

	draw_impl(ctx, ctx->ring, &emit);

	if (ctx->rasterizer->rasterizer_discard) {
		fd_wfi(ctx, ctx->ring);
		OUT_PKT3(ctx->ring, CP_REG_RMW, 3);
		OUT_RING(ctx->ring, REG_A4XX_RB_RENDER_CONTROL);
		OUT_RING(ctx->ring, ~A4XX_RB_RENDER_CONTROL_DISABLE_COLOR_PIPE);
		OUT_RING(ctx->ring, 0);
	}
}

/* clear operations ignore viewport state, so we need to reset it
 * based on framebuffer state:
 */
static void
reset_viewport(struct fd_ringbuffer *ring, struct pipe_framebuffer_state *pfb)
{
	float half_width = pfb->width * 0.5f;
	float half_height = pfb->height * 0.5f;

	OUT_PKT0(ring, REG_A4XX_GRAS_CL_VPORT_XOFFSET_0, 4);
	OUT_RING(ring, A4XX_GRAS_CL_VPORT_XOFFSET_0(half_width));
	OUT_RING(ring, A4XX_GRAS_CL_VPORT_XSCALE_0(half_width));
	OUT_RING(ring, A4XX_GRAS_CL_VPORT_YOFFSET_0(half_height));
	OUT_RING(ring, A4XX_GRAS_CL_VPORT_YSCALE_0(-half_height));
}

/* TODO maybe we should just migrate u_blitter for clear and do it in
 * core (so we get normal draw pass state mgmt and binning).. That should
 * work well enough for a3xx/a4xx (but maybe not a2xx?)
 */

static void
fd4_clear_binning(struct fd_context *ctx, unsigned dirty)
{
	struct fd4_context *fd4_ctx = fd4_context(ctx);
	struct fd_ringbuffer *ring = ctx->binning_ring;
	struct fd4_emit emit = {
		.vtx  = &fd4_ctx->solid_vbuf_state,
		.prog = &ctx->solid_prog,
		.key = {
			.binning_pass = true,
			.half_precision = true,
		},
		.dirty = dirty,
	};

	fd4_emit_state(ctx, ring, &emit);
	fd4_emit_vertex_bufs(ring, &emit);
	reset_viewport(ring, &ctx->framebuffer);

	OUT_PKT0(ring, REG_A4XX_PC_PRIM_VTX_CNTL, 2);
	OUT_RING(ring, A4XX_PC_PRIM_VTX_CNTL_VAROUT(0) |
			A4XX_PC_PRIM_VTX_CNTL_PROVOKING_VTX_LAST);
	OUT_RING(ring, A4XX_PC_PRIM_VTX_CNTL2_POLYMODE_FRONT_PTYPE(PC_DRAW_TRIANGLES) |
			A4XX_PC_PRIM_VTX_CNTL2_POLYMODE_BACK_PTYPE(PC_DRAW_TRIANGLES));

	OUT_PKT0(ring, REG_A4XX_GRAS_ALPHA_CONTROL, 1);
	OUT_RING(ring, 0x00000002);

	fd4_draw(ctx, ring, DI_PT_RECTLIST, IGNORE_VISIBILITY,
			DI_SRC_SEL_AUTO_INDEX, 2, 1, INDEX_SIZE_IGN, 0, 0, NULL);
}

static void
fd4_clear(struct fd_context *ctx, unsigned buffers,
		const union pipe_color_union *color, double depth, unsigned stencil)
{
	struct fd4_context *fd4_ctx = fd4_context(ctx);
	struct fd_ringbuffer *ring = ctx->ring;
	struct pipe_framebuffer_state *pfb = &ctx->framebuffer;
	unsigned char mrt_comp[A4XX_MAX_RENDER_TARGETS] = {0};
	unsigned dirty = ctx->dirty;
	unsigned i;
	struct fd4_emit emit = {
		.vtx  = &fd4_ctx->solid_vbuf_state,
		.prog = &ctx->solid_prog,
		.key = {
			.half_precision = fd_half_precision(pfb),
		},
	};

	dirty &= FD_DIRTY_FRAMEBUFFER | FD_DIRTY_SCISSOR;
	dirty |= FD_DIRTY_PROG;
	emit.dirty = dirty;

	fd4_clear_binning(ctx, dirty);

	OUT_PKT0(ring, REG_A4XX_PC_PRIM_VTX_CNTL, 1);
	OUT_RING(ring, A4XX_PC_PRIM_VTX_CNTL_PROVOKING_VTX_LAST);

	/* emit generic state now: */
	fd4_emit_state(ctx, ring, &emit);
	reset_viewport(ring, pfb);

	if (buffers & PIPE_CLEAR_DEPTH) {
		OUT_PKT0(ring, REG_A4XX_RB_DEPTH_CONTROL, 1);
		OUT_RING(ring, A4XX_RB_DEPTH_CONTROL_Z_WRITE_ENABLE |
				A4XX_RB_DEPTH_CONTROL_Z_ENABLE |
				A4XX_RB_DEPTH_CONTROL_ZFUNC(FUNC_ALWAYS));

		fd_wfi(ctx, ring);
		OUT_PKT0(ring, REG_A4XX_GRAS_CL_VPORT_ZOFFSET_0, 2);
		OUT_RING(ring, A4XX_GRAS_CL_VPORT_ZOFFSET_0(0.0));
		OUT_RING(ring, A4XX_GRAS_CL_VPORT_ZSCALE_0(depth));
		ctx->dirty |= FD_DIRTY_VIEWPORT;
	} else {
		OUT_PKT0(ring, REG_A4XX_RB_DEPTH_CONTROL, 1);
		OUT_RING(ring, A4XX_RB_DEPTH_CONTROL_ZFUNC(FUNC_NEVER));
	}

	if (buffers & PIPE_CLEAR_STENCIL) {
		OUT_PKT0(ring, REG_A4XX_RB_STENCILREFMASK, 2);
		OUT_RING(ring, A4XX_RB_STENCILREFMASK_STENCILREF(stencil) |
				A4XX_RB_STENCILREFMASK_STENCILMASK(stencil) |
				A4XX_RB_STENCILREFMASK_STENCILWRITEMASK(0xff));
		OUT_RING(ring, A4XX_RB_STENCILREFMASK_STENCILREF(0) |
				A4XX_RB_STENCILREFMASK_STENCILMASK(0) |
				0xff000000 | // XXX ???
				A4XX_RB_STENCILREFMASK_STENCILWRITEMASK(0xff));

		OUT_PKT0(ring, REG_A4XX_RB_STENCIL_CONTROL, 2);
		OUT_RING(ring, A4XX_RB_STENCIL_CONTROL_STENCIL_ENABLE |
				A4XX_RB_STENCIL_CONTROL_FUNC(FUNC_ALWAYS) |
				A4XX_RB_STENCIL_CONTROL_FAIL(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_ZPASS(STENCIL_REPLACE) |
				A4XX_RB_STENCIL_CONTROL_ZFAIL(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_FUNC_BF(FUNC_NEVER) |
				A4XX_RB_STENCIL_CONTROL_FAIL_BF(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_ZPASS_BF(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_ZFAIL_BF(STENCIL_KEEP));
		OUT_RING(ring, A4XX_RB_STENCIL_CONTROL2_STENCIL_BUFFER);
	} else {
		OUT_PKT0(ring, REG_A4XX_RB_STENCILREFMASK, 2);
		OUT_RING(ring, A4XX_RB_STENCILREFMASK_STENCILREF(0) |
				A4XX_RB_STENCILREFMASK_STENCILMASK(0) |
				A4XX_RB_STENCILREFMASK_STENCILWRITEMASK(0));
		OUT_RING(ring, A4XX_RB_STENCILREFMASK_BF_STENCILREF(0) |
				A4XX_RB_STENCILREFMASK_BF_STENCILMASK(0) |
				A4XX_RB_STENCILREFMASK_BF_STENCILWRITEMASK(0));

		OUT_PKT0(ring, REG_A4XX_RB_STENCIL_CONTROL, 2);
		OUT_RING(ring, A4XX_RB_STENCIL_CONTROL_FUNC(FUNC_NEVER) |
				A4XX_RB_STENCIL_CONTROL_FAIL(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_ZPASS(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_ZFAIL(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_FUNC_BF(FUNC_NEVER) |
				A4XX_RB_STENCIL_CONTROL_FAIL_BF(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_ZPASS_BF(STENCIL_KEEP) |
				A4XX_RB_STENCIL_CONTROL_ZFAIL_BF(STENCIL_KEEP));
		OUT_RING(ring, 0x00000000); /* RB_STENCIL_CONTROL2 */
	}

	if (buffers & PIPE_CLEAR_COLOR) {
		OUT_PKT0(ring, REG_A4XX_RB_ALPHA_CONTROL, 1);
		OUT_RING(ring, A4XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC(FUNC_NEVER));
	}

	for (i = 0; i < A4XX_MAX_RENDER_TARGETS; i++) {
		mrt_comp[i] = (buffers & (PIPE_CLEAR_COLOR0 << i)) ? 0xf : 0x0;

		OUT_PKT0(ring, REG_A4XX_RB_MRT_CONTROL(i), 1);
		OUT_RING(ring, A4XX_RB_MRT_CONTROL_ROP_CODE(ROP_COPY) |
				A4XX_RB_MRT_CONTROL_COMPONENT_ENABLE(0xf));

		OUT_PKT0(ring, REG_A4XX_RB_MRT_BLEND_CONTROL(i), 1);
		OUT_RING(ring, A4XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(FACTOR_ONE) |
				A4XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(BLEND_DST_PLUS_SRC) |
				A4XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(FACTOR_ZERO) |
				A4XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(FACTOR_ONE) |
				A4XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(BLEND_DST_PLUS_SRC) |
				A4XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR(FACTOR_ZERO));
	}

	OUT_PKT0(ring, REG_A4XX_RB_RENDER_COMPONENTS, 1);
	OUT_RING(ring, A4XX_RB_RENDER_COMPONENTS_RT0(mrt_comp[0]) |
			A4XX_RB_RENDER_COMPONENTS_RT1(mrt_comp[1]) |
			A4XX_RB_RENDER_COMPONENTS_RT2(mrt_comp[2]) |
			A4XX_RB_RENDER_COMPONENTS_RT3(mrt_comp[3]) |
			A4XX_RB_RENDER_COMPONENTS_RT4(mrt_comp[4]) |
			A4XX_RB_RENDER_COMPONENTS_RT5(mrt_comp[5]) |
			A4XX_RB_RENDER_COMPONENTS_RT6(mrt_comp[6]) |
			A4XX_RB_RENDER_COMPONENTS_RT7(mrt_comp[7]));

	fd4_emit_vertex_bufs(ring, &emit);

	OUT_PKT0(ring, REG_A4XX_GRAS_ALPHA_CONTROL, 1);
	OUT_RING(ring, 0x0);          /* XXX GRAS_ALPHA_CONTROL */

	OUT_PKT0(ring, REG_A4XX_GRAS_CLEAR_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	/* until fastclear works: */
	fd4_emit_const(ring, SHADER_FRAGMENT, 0, 0, 4, color->ui, NULL);

	OUT_PKT0(ring, REG_A4XX_VFD_INDEX_OFFSET, 2);
	OUT_RING(ring, 0);            /* VFD_INDEX_OFFSET */
	OUT_RING(ring, 0);            /* ??? UNKNOWN_2209 */

	OUT_PKT0(ring, REG_A4XX_PC_RESTART_INDEX, 1);
	OUT_RING(ring, 0xffffffff);   /* PC_RESTART_INDEX */

	OUT_PKT3(ring, CP_UNKNOWN_1A, 1);
	OUT_RING(ring, 0x00000001);

	fd4_draw(ctx, ring, DI_PT_RECTLIST, USE_VISIBILITY,
			DI_SRC_SEL_AUTO_INDEX, 2, 1, INDEX_SIZE_IGN, 0, 0, NULL);

	OUT_PKT3(ring, CP_UNKNOWN_1A, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT0(ring, REG_A4XX_GRAS_CLEAR_CNTL, 1);
	OUT_RING(ring, A4XX_GRAS_CLEAR_CNTL_NOT_FASTCLEAR);

	OUT_PKT0(ring, REG_A4XX_GRAS_SC_CONTROL, 1);
	OUT_RING(ring, A4XX_GRAS_SC_CONTROL_RENDER_MODE(RB_RENDERING_PASS) |
			A4XX_GRAS_SC_CONTROL_MSAA_DISABLE |
			A4XX_GRAS_SC_CONTROL_MSAA_SAMPLES(MSAA_ONE) |
			A4XX_GRAS_SC_CONTROL_RASTER_MODE(0));
}

void
fd4_draw_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);
	ctx->draw_vbo = fd4_draw_vbo;
	ctx->clear = fd4_clear;
}
