/* Copyright 2026 Sean Dolbec; SPDX-License-Identifier: MIT */
#include <gtest/gtest.h>
#include <vector>
#include "agx_compile.h"
#include "agx_test.h"
#include "compiler/nir/nir_builder.h"
extern "C" {
#include "agx_nir.h"
}

/*
 * cmat_muladd_agx on AGX3B compiles to the 12-byte f32matmad8x8 (see
 * NativeForms.AGX3BMma8MetalWitnesses for the encoding): fp32 or fp16 A and
 * B, an fp32 accumulator. bf16 has no AGX3B form and is refused rather than
 * miscompiled. The register-pair form reaches halves 0..191, which is the
 * whole AGX3B register file (AGX3B_NUM_REGS), so full pressure needs no extra
 * register-allocation constraint.
 */

/* Constant bits of every f32matmad8x8 witness, and its A/B type byte (bit 7
 * of byte 8 is A's discard hint). */
static unsigned
count_mma8(const std::vector<uint8_t> &code, uint8_t types)
{
   unsigned n = 0;
   for (size_t i = 0; i + 12 <= code.size(); ++i) {
      const uint8_t *c = &code[i];
      if (c[0] == 0xcf && (c[2] & 0xfc) == 0x54 && c[4] == 0x00 &&
          (c[9] & 0xfc) == 0x40 && c[10] == 0x24 && (c[11] & 0xfe) == 0 &&
          (c[8] & 0x7f) == types)
         ++n;
   }
   return n;
}

static nir_def *
load_global(nir_builder *b, nir_def *addr)
{
   nir_def *def = nir_load_global(b, 1, 32, addr);
   nir_intrinsic_set_access(
      nir_def_as_intrinsic(def),
      (gl_access_qualifier)(ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER));
   nir_intrinsic_set_align(nir_def_as_intrinsic(def), 4, 0);
   return def;
}

/* live > 0 keeps that many loaded values live across the multiply so the
 * register file fills past half 192. */
static bool
compile_mma8(agx_chip chip, nir_alu_type type, unsigned live,
             std::vector<uint8_t> *code)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "AGX3B mma8");
   const unsigned bits = nir_alu_type_get_type_size(type);
   nir_def *a = agx_test_load_preamble(&b, 2, bits, 4);
   nir_def *bm = agx_test_load_preamble(&b, 2, bits, 8);
   nir_def *c = agx_test_load_preamble(&b, 2, 32, 12);

   std::vector<nir_def *> values;
   if (live) {
      nir_def *base = agx_test_load_preamble(&b, 1, 64, 16);
      nir_def *sum = nir_imm_float(&b, 0.0f);
      for (unsigned i = 0; i < live; ++i) {
         values.push_back(load_global(&b, nir_iadd_imm(&b, base, 4 * i)));
         sum = nir_fadd(&b, sum, values.back());
      }
      c = nir_vec2(&b, nir_fadd(&b, nir_channel(&b, c, 0), sum),
                   nir_channel(&b, c, 1));
   }

   nir_def *d = nir_cmat_muladd_agx(&b, a, bm, c);
   nir_intrinsic_set_src_type(nir_def_as_intrinsic(d), type);
   nir_intrinsic_set_src_type2(nir_def_as_intrinsic(d), type);

   nir_def *x = nir_channel(&b, d, 0), *y = nir_channel(&b, d, 1);
   for (unsigned i = 0; i < values.size(); ++i)
      y = nir_ffma(&b, values[i], x, y);

   nir_intrinsic_instr *store = nir_image_store(
      &b, nir_imm_intN_t(&b, 1, 16), nir_imm_ivec4(&b, 0, 0, 0, 0),
      nir_imm_intN_t(&b, 0, 16), nir_vec4(&b, x, y, x, y),
      nir_imm_intN_t(&b, 0, 16));
   nir_intrinsic_set_image_dim(store, GLSL_SAMPLER_DIM_2D);
   nir_intrinsic_set_format(store, PIPE_FORMAT_R32G32B32A32_FLOAT);
   nir_intrinsic_set_src_type(store, nir_type_float32);

   agx_shader_key key = {};
   key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   key.dev.chip = chip;
   key.no_preamble = true;
   key.reserved_preamble = 24;
   key.has_scratch = live != 0;
   agx_preprocess_nir(b.shader);
   agx_shader_part part = {};
   const bool ok = agx_compile_shader_nir(b.shader, &key, &part);
   if (ok) {
      const uint8_t *bytes = static_cast<const uint8_t *>(part.binary);
      code->assign(bytes, bytes + part.info.binary_size);
   }
   free(part.binary);
   ralloc_free(b.shader);
   return ok;
}

TEST(AGX3BCooperativeMatrix, Fp32OperandsUseMma8)
{
   for (agx_chip chip : {AGX_CHIP_G17P, AGX_CHIP_G16G}) {
      SCOPED_TRACE(chip);
      std::vector<uint8_t> code;
      ASSERT_TRUE(compile_mma8(chip, nir_type_float32, 0, &code));
      EXPECT_EQ(count_mma8(code, 0x54), 1u);
   }
}

TEST(AGX3BCooperativeMatrix, Fp16OperandsUseMma8)
{
   for (agx_chip chip : {AGX_CHIP_G17P, AGX_CHIP_G16G}) {
      SCOPED_TRACE(chip);
      std::vector<uint8_t> code;
      ASSERT_TRUE(compile_mma8(chip, nir_type_float16, 0, &code));
      EXPECT_EQ(count_mma8(code, 0x4a), 1u);
   }
}

/* With the register file full across the multiply (spilling), the shader
 * still packs. */
TEST(AGX3BCooperativeMatrix, FullRegisterFilePacks)
{
   for (nir_alu_type type : {nir_type_float32, nir_type_float16}) {
      SCOPED_TRACE(type);
      std::vector<uint8_t> code;
      ASSERT_TRUE(compile_mma8(AGX_CHIP_G17P, type, 110, &code));
      EXPECT_EQ(count_mma8(code, type == nir_type_float32 ? 0x54 : 0x4a), 1u);
   }
}

/* bf16 operands have no AGX3B encoding: refuse the shader. */
TEST(AGX3BCooperativeMatrix, Bf16IsRefused)
{
   std::vector<uint8_t> code;
   EXPECT_FALSE(compile_mma8(AGX_CHIP_G17P, nir_type_uint16, 0, &code));
}
