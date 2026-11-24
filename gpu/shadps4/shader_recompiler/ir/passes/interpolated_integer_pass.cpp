// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: integer attributes passed as smooth float varyings (instanced objects can pass the
// instance index this way, then truncate it with v_cvt_u32_f32). AMD hardware interpolates
// three equal values exactly; other drivers need not, and a value such as 0.99999994 truncates
// to 0 on part of the triangle: the object flickers with noise (shadPS4 PR 1644). Where the
// driver interpolates (no AMD explicit vertex parameter, no manual barycentric path), a
// truncation of an interpolated value is scaled by 1 + 2^-16 first: it restores the exact
// integer, and moves other values by far less than a pixel's gradient.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/interpolated_integer.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

namespace {
bool IsTruncation(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::ConvertU32F32:
    case IR::Opcode::ConvertS32F32:
    case IR::Opcode::FPTrunc32:
    case IR::Opcode::FPFloor32:
        return true;
    default:
        return false;
    }
}

bool IsInterpolatedParam(const Info& info, const IR::Value& value) {
    if (value.IsImmediate()) {
        return false;
    }
    const IR::Inst* inst = value.TryInst();
    if (!inst || inst->GetOpcode() != IR::Opcode::GetAttribute) {
        return false;
    }
    const IR::Attribute attr = inst->Arg(0).Attribute();
    if (!IR::IsParam(attr)) {
        return false;
    }
    const u32 index = static_cast<u32>(attr) - static_cast<u32>(IR::Attribute::Param0);
    const Qualifier primary = info.fs_interpolation[index].primary;
    return primary != Qualifier::Flat && primary != Qualifier::PerVertex;
}
} // namespace

void InterpolatedIntegerPass(IR::Program& program) {
    u32 patched = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTruncation(inst.GetOpcode()) || !IsInterpolatedParam(program.info, inst.Arg(0))) {
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::F32 value{inst.Arg(0)};
            inst.SetArg(0, ir.FPMul(value, ir.Imm32(InterpolatedIntegerScale)));
            ++patched;
        }
    }
    if (patched) {
        program.info.interp_summary.integer_fix_count =
            static_cast<u8>(std::min<u32>(patched, 255u));
        static std::atomic<u32> shaders{0};
        const u32 count = shaders.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 32) {
            std::printf("Shader: interpolated integer fix, %u truncations in %016llx (shader %u)\n",
                        patched, static_cast<unsigned long long>(program.info.pgm_hash), count);
        }
    }
}

} // namespace Shader::Optimization
