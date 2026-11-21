/*
 * Copyright 2026 Dylan Ironside
 * SPDX-License-Identifier: MIT
 */

#include <vector>
#include <gtest/gtest.h>
#include "agx_builder.h"
#include "agx_test.h"

/* agx3_g16_hoist_pixel_independent runs after scheduling, so these cases pin
 * its dependency rules and its gates directly. */

namespace {

struct hoist_case {
   void *mem;
   agx_shader_key key;
   agx_builder *b;
   agx_instr *wait;
   agx_instr *mul;  /* uniform * uniform */
   agx_instr *add;  /* mul + immediate */
   agx_instr *fma;  /* add * uniform + mov_imm */
   agx_instr *imm;  /* mov_imm */
   agx_instr *tile; /* ld_tile */
   agx_instr *mix;  /* tile * add: tile-dependent */
   agx_instr *mix2; /* mix + uniform: depends on a tile-dependent value */
};

static void
build(hoist_case &c, enum agx_chip chip, mesa_shader_stage stage,
      bool preamble = false)
{
   c.mem = ralloc_context(nullptr);
   c.key = {};
   c.key.dev.chip = chip;
   c.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   c.b = agx_test_builder(c.mem);
   c.b->shader->key = &c.key;
   c.b->shader->stage = stage;
   c.b->shader->is_preamble = preamble;

   agx_builder *b = c.b;
   agx_context *ctx = b->shader;
   const agx_index u0 = agx_uniform(4, AGX_SIZE_32);
   const agx_index u1 = agx_uniform(6, AGX_SIZE_32);

   c.wait = agx_wait_pix(b, 1);
   c.tile = agx_ld_tile_to(b, agx_temp(ctx, AGX_SIZE_32), agx_immediate(0xff),
                           agx_null(), AGX_FORMAT_I32, 1, 0, false, 0);
   c.mul = agx_fmul_to(b, agx_temp(ctx, AGX_SIZE_32), u0, u1);
   c.imm = agx_mov_imm_to(b, agx_temp(ctx, AGX_SIZE_32), 0x3f800000);
   c.mix = agx_fmul_to(b, agx_temp(ctx, AGX_SIZE_32), c.tile->dest[0],
                       c.mul->dest[0]);
   c.add = agx_fadd_to(b, agx_temp(ctx, AGX_SIZE_32), c.mul->dest[0],
                       agx_immediate_f(0.5f));
   c.mix2 = agx_fadd_to(b, agx_temp(ctx, AGX_SIZE_32), c.mix->dest[0], u0);
   c.fma = agx_ffma_to(b, agx_temp(ctx, AGX_SIZE_32), c.add->dest[0], u1,
                       c.imm->dest[0]);
}

static std::vector<agx_instr *>
order(agx_context *ctx)
{
   std::vector<agx_instr *> v;
   agx_foreach_instr_global(ctx, I)
      v.push_back(I);
   return v;
}

static void
expect_unchanged(enum agx_chip chip, mesa_shader_stage stage, bool preamble)
{
   hoist_case c;
   build(c, chip, stage, preamble);
   const std::vector<agx_instr *> before = order(c.b->shader);
   EXPECT_FALSE(agx3_g16_hoist_pixel_independent(c.b->shader));
   EXPECT_EQ(order(c.b->shader), before);
   ralloc_free(c.mem);
}

} /* namespace */

/* The uniform-only chain mul -> add -> fma (with the mov_imm it reads) moves
 * ahead of WAIT_PIX in its scheduled order. The tile load, the operation that
 * reads it and the operation that reads that result keep their places after
 * the wait, even though they also read uniforms. */
TEST(G16PixelHoist, UniformChainMovesTileDependentStays)
{
   hoist_case c;
   build(c, AGX_CHIP_G16G, MESA_SHADER_FRAGMENT);

   EXPECT_TRUE(agx3_g16_hoist_pixel_independent(c.b->shader));
   const std::vector<agx_instr *> expected = {
      c.mul, c.imm, c.add, c.fma, c.wait, c.tile, c.mix, c.mix2,
   };
   EXPECT_EQ(order(c.b->shader), expected);

   /* Nothing about the moved operations changes. */
   EXPECT_EQ(c.add->src[0].value, c.mul->dest[0].value);
   EXPECT_EQ(c.fma->src[2].value, c.imm->dest[0].value);

   /* A second run finds nothing left to move. */
   EXPECT_FALSE(agx3_g16_hoist_pixel_independent(c.b->shader));
   EXPECT_EQ(order(c.b->shader), expected);
   ralloc_free(c.mem);
}

/* Uniform-only operations already ahead of the wait are not reordered, and a
 * block without WAIT_PIX is left alone. */
TEST(G16PixelHoist, NothingAfterWaitOrNoWait)
{
   void *mem = ralloc_context(nullptr);
   agx_shader_key key = {};
   key.dev.chip = AGX_CHIP_G16G;
   key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   agx_builder *b = agx_test_builder(mem);
   b->shader->key = &key;
   b->shader->stage = MESA_SHADER_FRAGMENT;
   agx_fmul_to(b, agx_temp(b->shader, AGX_SIZE_32),
               agx_uniform(4, AGX_SIZE_32), agx_uniform(6, AGX_SIZE_32));
   agx_fadd_to(b, agx_temp(b->shader, AGX_SIZE_32),
               agx_uniform(8, AGX_SIZE_32), agx_immediate_f(1.0f));

   std::vector<agx_instr *> before = order(b->shader);
   EXPECT_FALSE(agx3_g16_hoist_pixel_independent(b->shader));
   EXPECT_EQ(order(b->shader), before);

   agx_wait_pix(b, 1);
   before = order(b->shader);
   EXPECT_FALSE(agx3_g16_hoist_pixel_independent(b->shader));
   EXPECT_EQ(order(b->shader), before);
   ralloc_free(mem);
}

/* Only G16G fragment main programs are touched. */
TEST(G16PixelHoist, OtherChipsStagesAndPreamblesUnchanged)
{
   for (enum agx_chip chip : {AGX_CHIP_G13G, AGX_CHIP_G13X, AGX_CHIP_G14G,
                              AGX_CHIP_G14X, AGX_CHIP_G17P})
      expect_unchanged(chip, MESA_SHADER_FRAGMENT, false);
   expect_unchanged(AGX_CHIP_G16G, MESA_SHADER_COMPUTE, false);
   expect_unchanged(AGX_CHIP_G16G, MESA_SHADER_VERTEX, false);
   expect_unchanged(AGX_CHIP_G16G, MESA_SHADER_FRAGMENT, true);
}

/* A uniform write after the wait stops the scan: nothing after it reads its
 * uniforms earlier than scheduled. */
TEST(G16PixelHoist, StopsAtUniformWrite)
{
   hoist_case c;
   build(c, AGX_CHIP_G16G, MESA_SHADER_FRAGMENT);
   agx_builder sb = agx_init_builder(c.b->shader, agx_after_instr(c.wait));
   agx_uniform_store(&sb, agx_immediate(0), agx_immediate(4), 0x3);
   const std::vector<agx_instr *> before = order(c.b->shader);
   EXPECT_FALSE(agx3_g16_hoist_pixel_independent(c.b->shader));
   EXPECT_EQ(order(c.b->shader), before);
   ralloc_free(c.mem);
}
