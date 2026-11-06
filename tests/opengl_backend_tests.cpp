// OpenGL backend tests without OpenGL: the CPU detiler against the game's own tiler, and the GLSL the emitter writes,
// compiled by glslangValidator. No context, no window, no game files.
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "shader_recompiler/backend/glsl/emit_glsl.h"
#include "shader_recompiler/backend/glsl/ring_layout.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_opengl/gl_capabilities.h"
#include "video_core/renderer_opengl/gl_detile.h"
#include "video_core/renderer_opengl/gl_format.h"
#include "video_core/renderer_opengl/gl_primitives.h"
#include "video_core/renderer_opengl/gl_shader_cache.h"

// Normally the loader's: this test links the video core without it.
extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

namespace {

using AmdGpu::TileMode;
using Shader::Backend::GLSL::PrimitiveExpansion;
using Shader::Backend::GLSL::Target;
using Shader::IR::Opcode;
namespace IR = Shader::IR;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

struct AddressRow {
    unsigned tile_mode, bpp, alt_tile, x, y, offset;
};
const AddressRow address_rows[] = {
#include "tiling_addresses.inc"
};

OpenGL::TilingParams Params(TileMode mode, u32 bits, bool alt_tile = false) {
    return {mode, AmdGpu::GetArrayMode(mode), bits, 1, alt_tile, 0};
}

void TestGameAddresses() {
    for (const AddressRow& row : address_rows) {
        const auto mode = TileMode(row.tile_mode);
        const OpenGL::SurfaceTiler tiler{Params(mode, row.bpp, row.alt_tile != 0), 1024, 1024};
        CHECK(tiler.Address(row.x, row.y, 0) == row.offset);
    }
}

void TestDetile(TileMode mode, u32 bits, bool alt_tile, bool micro_copies) {
    constexpr u32 size = 512;
    const OpenGL::SurfaceTiler tiler{Params(mode, bits, alt_tile), size, size};
    CHECK(tiler.UsesMicroTileCopies() == micro_copies);
    const u32 bytes = bits / 8;
    const size_t total = size_t(size) * size * bytes;
    std::vector<u8> tiled(total);
    for (size_t i = 0; i < total; ++i) {
        tiled[i] = u8(i * 131 + (i >> 8) * 17);
    }
    std::vector<u8> linear(total, 0xA5);
    tiler.Detile(tiled.data(), tiled.size(), linear.data(), 1);
    bool same = true;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const u32 address = tiler.Address(x, y, 0);
            for (u32 byte = 0; byte < bytes; ++byte) {
                same &= linear[(size_t(y) * size + x) * bytes + byte] == tiled[address + byte];
            }
        }
    }
    CHECK(same);
}

IR::Value Op(IR::Block& block, Opcode op, std::initializer_list<IR::Value> args, u32 flags = 0) {
    return IR::Value{&*block.PrependNewInst(block.end(), op, args, flags)};
}

constexpr std::array<u32, 16> user_data{};
constexpr std::array<u32, 8> fake_code{};

Shader::ShaderParams Params(u64 hash) {
    return {.user_data = user_data, .code = fake_code, .hash = hash};
}

void Block(IR::Program& program, IR::Block* block) {
    auto& node = program.syntax_list.emplace_back();
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = block;
    program.blocks.push_back(block);
}

void EveryScalarOperation(IR::Block& block, IR::Value u, IR::Value f) {
    const IR::Value w = Op(block, Opcode::IAdd32, {u, IR::Value{3u}});
    for (const Opcode op :
         {Opcode::FPAdd32, Opcode::FPSub32, Opcode::FPMul32, Opcode::FPDiv32, Opcode::FPMax32, Opcode::FPMin32,
          Opcode::FPPow, Opcode::FPOrdEqual32, Opcode::FPUnordEqual32, Opcode::FPOrdNotEqual32,
          Opcode::FPUnordNotEqual32, Opcode::FPOrdLessThan32, Opcode::FPUnordLessThan32, Opcode::FPOrdGreaterThan32,
          Opcode::FPUnordGreaterThan32, Opcode::FPOrdLessThanEqual32, Opcode::FPUnordLessThanEqual32,
          Opcode::FPOrdGreaterThanEqual32, Opcode::FPUnordGreaterThanEqual32}) {
        Op(block, op, {f, IR::Value{1.5f}});
    }
    for (const Opcode op :
         {Opcode::FPAbs32, Opcode::FPNeg32, Opcode::FPRecip32, Opcode::FPRecipSqrt32, Opcode::FPSqrt, Opcode::FPSin,
          Opcode::FPCos, Opcode::FPExp2, Opcode::FPLog2, Opcode::FPSaturate32, Opcode::FPRoundEven32, Opcode::FPFloor32,
          Opcode::FPCeil32, Opcode::FPTrunc32, Opcode::FPFract32, Opcode::FPFrexpSig32, Opcode::FPFrexpExp32,
          Opcode::FPIsNan32, Opcode::FPIsInf32, Opcode::BitCastU32F32, Opcode::ConvertS32F32, Opcode::ConvertU32F32}) {
        Op(block, op, {f});
    }
    for (const Opcode op : {Opcode::FPFma32, Opcode::FPClamp32, Opcode::FPMinTri32, Opcode::FPMaxTri32, Opcode::FPMedTri32}) {
        Op(block, op, {f, IR::Value{-0.25f}, IR::Value{2.0f}});
    }
    Op(block, Opcode::FPLdexp, {f, u});
    for (const Opcode op :
         {Opcode::IAdd32, Opcode::ISub32, Opcode::IMul32, Opcode::SMulHi, Opcode::UMulHi, Opcode::SDiv32, Opcode::UDiv32,
          Opcode::SMod32, Opcode::UMod32, Opcode::ShiftLeftLogical32, Opcode::ShiftRightLogical32,
          Opcode::ShiftRightArithmetic32, Opcode::BitwiseAnd32, Opcode::BitwiseOr32, Opcode::BitwiseXor32, Opcode::SMin32,
          Opcode::SMax32, Opcode::UMin32, Opcode::UMax32, Opcode::SLessThan32, Opcode::SLessThanEqual32,
          Opcode::SGreaterThan32, Opcode::SGreaterThanEqual32, Opcode::ULessThan32, Opcode::ULessThanEqual32,
          Opcode::UGreaterThan32, Opcode::UGreaterThanEqual32, Opcode::IEqual32, Opcode::INotEqual32, Opcode::IAddCarry32}) {
        Op(block, op, {u, w});
    }
    for (const Opcode op :
         {Opcode::INeg32, Opcode::IAbs32, Opcode::BitReverse32, Opcode::BitCount32, Opcode::BitwiseNot32, Opcode::FindSMsb32,
          Opcode::FindUMsb32, Opcode::FindILsb32, Opcode::ConvertF32S32, Opcode::ConvertF32U32, Opcode::BitCastF32U32,
          Opcode::ConvertU16U32, Opcode::ConvertU8U32, Opcode::ConvertS32S8, Opcode::ConvertS32S16, Opcode::ConvertU64U32,
          Opcode::UnpackUnorm2x16, Opcode::UnpackSnorm2x16, Opcode::UnpackHalf2x16, Opcode::UnpackUnorm4x8,
          Opcode::UnpackSnorm4x8, Opcode::UnpackUnorm2_10_10_10}) {
        Op(block, op, {u});
    }
    for (const Opcode op : {Opcode::BitFieldSExtract, Opcode::BitFieldUExtract, Opcode::SClamp32, Opcode::UClamp32,
                            Opcode::SMinTri32, Opcode::SMaxTri32, Opcode::UMinTri32, Opcode::UMaxTri32,
                            Opcode::SMedTri32, Opcode::UMedTri32}) {
        Op(block, op, {u, IR::Value{4u}, IR::Value{8u}});
    }
    Op(block, Opcode::BitFieldInsert32, {u, w, IR::Value{4u}, IR::Value{8u}});

    const IR::Value v2 = Op(block, Opcode::CompositeConstructF32x2, {f, IR::Value{0.5f}});
    const IR::Value v3 = Op(block, Opcode::CompositeConstructF32x3, {f, f, IR::Value{0.5f}});
    const IR::Value v4 = Op(block, Opcode::CompositeConstructF32x4, {f, f, f, IR::Value{1.0f}});
    const IR::Value u4 = Op(block, Opcode::CompositeConstructU32x4, {u, w, u, w});
    Op(block, Opcode::CompositeExtractF32x4, {v4, IR::Value{2u}});
    Op(block, Opcode::CompositeInsertF32x3, {v3, f, IR::Value{1u}});
    Op(block, Opcode::CompositeShuffleF32x4, {v4, v4, IR::Value{0u}, IR::Value{5u}, IR::Value{2u}, IR::Value{7u}});
    Op(block, Opcode::CompositeExtractU32x4, {u4, IR::Value{1u}});
    for (const Opcode op : {Opcode::PackUnorm2x16, Opcode::PackSnorm2x16, Opcode::PackHalf2x16}) {
        Op(block, op, {v2});
    }
    for (const Opcode op : {Opcode::PackUnorm4x8, Opcode::PackSnorm4x8, Opcode::PackUnorm2_10_10_10}) {
        Op(block, op, {v4});
    }
    const IR::Value less = Op(block, Opcode::FPOrdLessThan32, {f, IR::Value{1.0f}});
    Op(block, Opcode::SelectF32, {less, f, IR::Value{2.0f}});
    Op(block, Opcode::SelectU32, {less, u, w});
    Op(block, Opcode::LogicalAnd, {less, IR::Value{true}});
    Op(block, Opcode::LogicalNot, {less});
}

struct EmittedStage {
    Shader::Info info;
    Shader::Backend::GLSL::Stage glsl;
};

// Structure: entry, if with a phi at its merge, then a loop with a break and a loop-carried phi.
Shader::Backend::GLSL::Stage VertexStage(Shader::Pools& pools, Shader::Info& info, const Shader::Profile& profile,
                                         Target target = Target::Desktop330) {
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    runtime.hw.vs.clip_disable = true;
    runtime.hw.vs.emulate_depth_negative_one_to_one = true;
    info.ud_mask.Set(IR::ScalarReg::S0);
    info.buffers.push_back({.used_types = IR::Type::U32, .buffer_type = Shader::BufferType::Guest});

    IR::Program program{info};
    auto* entry = pools.block_pool.Create(pools.inst_pool);
    auto* then_block = pools.block_pool.Create(pools.inst_pool);
    auto* merge = pools.block_pool.Create(pools.inst_pool);
    auto* header = pools.block_pool.Create(pools.inst_pool);
    auto* latch = pools.block_pool.Create(pools.inst_pool);
    auto* exit_block = pools.block_pool.Create(pools.inst_pool);

    const IR::Value user = Op(*entry, Opcode::GetUserData, {IR::Value{IR::ScalarReg::S0}});
    const IR::Value id = Op(*entry, Opcode::GetAttributeU32, {IR::Value{IR::Attribute::VertexId}, IR::Value{0u}});
    Op(*entry, Opcode::GetAttributeU32, {IR::Value{IR::Attribute::InstanceId}, IR::Value{0u}});
    const IR::Value loaded = Op(*entry, Opcode::LoadBufferU32, {IR::Value{0u}, id});
    const IR::Value number = Op(*entry, Opcode::ConvertF32U32, {loaded});
    EveryScalarOperation(*entry, user, number);
    const IR::Value branch = Op(*entry, Opcode::IEqual32, {id, IR::Value{0u}});
    Block(program, entry);
    auto& if_node = program.syntax_list.emplace_back();
    if_node.type = IR::AbstractSyntaxNode::Type::If;
    if_node.data.if_node = {IR::U1{branch}, then_block, merge};

    const IR::Value doubled = Op(*then_block, Opcode::FPMul32, {number, IR::Value{2.0f}});
    Block(program, then_block);
    auto& end_if = program.syntax_list.emplace_back();
    end_if.type = IR::AbstractSyntaxNode::Type::EndIf;
    end_if.data.end_if.merge = merge;

    auto phi = merge->PrependNewInst(merge->end(), Opcode::Phi);
    phi->SetFlags(IR::Type::F32);
    phi->AddPhiOperand(entry, number);
    phi->AddPhiOperand(then_block, doubled);
    Block(program, merge);

    auto& loop = program.syntax_list.emplace_back();
    loop.type = IR::AbstractSyntaxNode::Type::Loop;
    loop.data.loop = {header, latch, exit_block};
    auto counter = header->PrependNewInst(header->end(), Opcode::Phi);
    counter->SetFlags(IR::Type::U32);
    const IR::Value done = Op(*header, Opcode::UGreaterThanEqual32, {IR::Value{&*counter}, IR::Value{4u}});
    Block(program, header);
    auto& break_node = program.syntax_list.emplace_back();
    break_node.type = IR::AbstractSyntaxNode::Type::Break;
    break_node.data.break_node = {IR::U1{done}, exit_block, latch};

    const IR::Value next = Op(*latch, Opcode::IAdd32, {IR::Value{&*counter}, IR::Value{1u}});
    const IR::Value again = Op(*latch, Opcode::ULessThan32, {next, IR::Value{8u}});
    counter->AddPhiOperand(merge, IR::Value{0u});
    counter->AddPhiOperand(latch, next);
    Block(program, latch);
    auto& repeat = program.syntax_list.emplace_back();
    repeat.type = IR::AbstractSyntaxNode::Type::Repeat;
    repeat.data.repeat = {IR::U1{again}, header, exit_block};

    const IR::Value position = IR::Value{&*phi};
    for (u32 i = 0; i < 4; ++i) {
        Op(*exit_block, Opcode::SetAttribute, {IR::Value{IR::Attribute::Position0}, position, IR::Value{i}});
        Op(*exit_block, Opcode::SetAttribute, {IR::Value{IR::Attribute::Param0}, position, IR::Value{i}});
        Op(*exit_block, Opcode::SetAttribute, {IR::Value{IR::Attribute::Param1}, position, IR::Value{i}});
    }
    Op(*exit_block, Opcode::SetAttribute, {IR::Value{IR::Attribute::ClipDistance}, position, IR::Value{1u}});
    Op(*exit_block, Opcode::Epilogue, {});
    Block(program, exit_block);
    auto& ret = program.syntax_list.emplace_back();
    ret.type = IR::AbstractSyntaxNode::Type::Return;

    auto stage = Shader::Backend::GLSL::EmitGLSL(profile, runtime, program, target);
    CHECK(stage.output_mask == 3);
    CHECK(stage.clip_distances == 2);
    return stage;
}

Shader::Backend::GLSL::Stage FragmentStage(Shader::Pools& pools, Shader::Info& info, const Shader::Profile& profile,
                                           Target target = Target::Desktop330,
                                           AmdGpu::ImageType image_type = AmdGpu::ImageType::Color2D) {
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    runtime.hw.fs.num_inputs = 3;
    runtime.hw.fs.inputs[0] = {.param_index = 0, .is_default = false, .is_flat = false, .default_value = 0};
    runtime.hw.fs.inputs[1] = {.param_index = 1, .is_default = false, .is_flat = false, .default_value = 0};
    runtime.hw.fs.inputs[2] = {.param_index = 5, .is_default = false, .is_flat = false, .default_value = 0};
    runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Unorm;
    runtime.hw.fs.color_buffers[1].num_format = AmdGpu::NumberFormat::Uint;
    info.fs_interpolation[0] = {Shader::Qualifier::Smooth, Shader::Qualifier::None};
    info.fs_interpolation[1] = {Shader::Qualifier::Flat, Shader::Qualifier::None};
    info.fs_interpolation[2] = {Shader::Qualifier::NoPerspective, Shader::Qualifier::Centroid};

    AmdGpu::Image texture = AmdGpu::Image::Null(false);
    texture.type = u64(image_type);
    info.flattened_ud_buf.resize(sizeof(texture) / sizeof(u32));
    std::memcpy(info.flattened_ud_buf.data(), &texture, sizeof(texture));
    Shader::ImageResource image{};
    for (u32 i = 0; i < 8; ++i) {
        image.sharp_fetch.offsets[i] = u16(i);
    }
    image.sharp_fetch.load_mask = 0xFF;
    info.images.push_back(image);
    info.samplers.push_back({});

    IR::Program program{info};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    const IR::Value s = Op(*block, Opcode::GetAttribute,
                           {IR::Value{IR::Attribute::Param0}, IR::Value{0u}, IR::Value{0u}});
    const IR::Value t = Op(*block, Opcode::GetAttribute,
                           {IR::Value{IR::Attribute::Param1}, IR::Value{1u}, IR::Value{0u}});
    const IR::Value unwritten = Op(*block, Opcode::GetAttribute,
                                   {IR::Value{IR::Attribute::Param2}, IR::Value{2u}, IR::Value{0u}});
    const IR::Value coords = Op(*block, Opcode::CompositeConstructF32x2, {s, t});
    const u32 handle = 0 | 0 << 16;
    const IR::Value bias = Op(*block, Opcode::FPAdd32, {unwritten, IR::Value{0.5f}});
    const IR::Value sample =
        Op(*block, Opcode::ImageSampleImplicitLod, {IR::Value{handle}, coords, bias, IR::Value{}});
    const IR::Value explicit_sample =
        Op(*block, Opcode::ImageSampleExplicitLod, {IR::Value{handle}, coords, IR::Value{1.0f}, IR::Value{}});
    const IR::Value texel_coords = Op(*block, Opcode::CompositeConstructU32x2, {IR::Value{3u}, IR::Value{4u}});
    const IR::Value fetched =
        Op(*block, Opcode::ImageRead, {IR::Value{handle}, texel_coords, IR::Value{}, IR::Value{}});
    Op(*block, Opcode::ImageQueryDimensions, {IR::Value{handle}, IR::Value{0u}, IR::Value{false}});
    const IR::Value x = Op(*block, Opcode::CompositeExtractF32x4, {sample, IR::Value{0u}});
    const IR::Value y = Op(*block, Opcode::CompositeExtractF32x4, {explicit_sample, IR::Value{1u}});
    const IR::Value z = Op(*block, Opcode::CompositeExtractF32x4, {fetched, IR::Value{2u}});
    const IR::Value coverage = Op(*block, Opcode::FPOrdLessThan32, {x, IR::Value{0.1f}});
    Op(*block, Opcode::DiscardCond, {coverage});
    const auto color0 = IR::Attribute::RenderTarget0;
    const auto color1 = IR::Attribute::RenderTarget1;
    Op(*block, Opcode::SetAttribute, {IR::Value{color0}, x, IR::Value{0u}});
    Op(*block, Opcode::SetAttribute, {IR::Value{color0}, y, IR::Value{1u}});
    Op(*block, Opcode::SetAttribute, {IR::Value{color0}, z, IR::Value{2u}});
    Op(*block, Opcode::SetAttribute, {IR::Value{color1}, z, IR::Value{0u}});
    Op(*block, Opcode::SetAttribute, {IR::Value{IR::Attribute::Depth}, x, IR::Value{0u}});
    Op(*block, Opcode::Epilogue, {});
    Block(program, block);
    auto& ret = program.syntax_list.emplace_back();
    ret.type = IR::AbstractSyntaxNode::Type::Return;

    auto stage = Shader::Backend::GLSL::EmitGLSL(profile, runtime, program, target);
    CHECK(stage.samplers.size() == 2);
    CHECK(stage.inputs.size() == 3);
    return stage;
}

// The shape of the game's 0x4c12efc8: no attributes and no buffers, position from the vertex index alone.
Shader::Backend::GLSL::Stage VertexIdOnlyStage(Shader::Pools& pools, Shader::Info& info, const Shader::Profile& profile) {
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    runtime.hw.vs.clip_disable = true;
    runtime.hw.vs.emulate_depth_negative_one_to_one = true;
    IR::Program program{info};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    const IR::Value id = Op(*block, Opcode::GetAttributeU32, {IR::Value{IR::Attribute::VertexId}, IR::Value{0u}});
    const IR::Value number = Op(*block, Opcode::ConvertF32U32, {id});
    for (u32 i = 0; i < 4; ++i) {
        Op(*block, Opcode::SetAttribute, {IR::Value{IR::Attribute::Position0}, number, IR::Value{i}});
    }
    Op(*block, Opcode::Epilogue, {});
    Block(program, block);
    auto& ret = program.syntax_list.emplace_back();
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    return Shader::Backend::GLSL::EmitGLSL(profile, runtime, program, Target::Gles300);
}

void Write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

void Compile(const std::filesystem::path& folder, const std::string& name) {
    const auto path = folder / name;
    std::string command = std::string{"\""} + DCP_GLSLANG + "\" \"" + path.string() + "\"";
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    const int result = std::system(command.c_str());
    if (result != 0) {
        std::printf("glslangValidator rejected %s\n", path.string().c_str());
    }
    CHECK(result == 0);
}

void TestGlsl(const std::filesystem::path& folder) {
    Shader::Profile profile{};
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    Shader::Pools pools;
    Shader::Info vertex_info{Shader::HwStage::Vertex, Shader::SwStage::Vertex, Params(1)};
    Shader::Info fragment_info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, Params(2)};
    const auto vertex = VertexStage(pools, vertex_info, profile);
    const auto fragment = FragmentStage(pools, fragment_info, profile);
    CHECK(vertex.error.empty());
    CHECK(fragment.error.empty());
    if (!vertex.error.empty() || !fragment.error.empty()) {
        std::printf("emitter refused: '%s' '%s'\n", vertex.error.c_str(), fragment.error.c_str());
        return;
    }

    const auto plain = Shader::Backend::GLSL::LinkStages(vertex, &fragment, PrimitiveExpansion::None);
    CHECK(plain.geometry.empty());
    CHECK(plain.vertex.find("flat out vec4 dcp_p1;") != std::string::npos);
    CHECK(plain.fragment.find("flat in vec4 dcp_p1;") != std::string::npos);
    CHECK(plain.fragment.find("const vec4 dcp_p5=vec4(0.0);") != std::string::npos);
    Write(folder / "plain.vert", plain.vertex);
    Write(folder / "plain.frag", plain.fragment);
    Compile(folder, "plain.vert");
    Compile(folder, "plain.frag");

    for (const auto expansion : {PrimitiveExpansion::RectList, PrimitiveExpansion::QuadList}) {
        const bool rect = expansion == PrimitiveExpansion::RectList;
        const auto sources = Shader::Backend::GLSL::LinkStages(vertex, &fragment, expansion);
        CHECK(sources.fragment.find("#define dcp_p0 dcp_g0") != std::string::npos);
        CHECK(sources.geometry.find(rect ? "layout(triangles) in;" : "layout(lines_adjacency) in;") !=
              std::string::npos);
        const std::string name = rect ? "rect" : "quad";
        Write(folder / (name + ".geom"), sources.geometry);
        Write(folder / (name + ".frag"), sources.fragment);
        Compile(folder, name + ".geom");
        Compile(folder, name + ".frag");
    }

    const auto depth_only = Shader::Backend::GLSL::LinkStages(vertex, nullptr, PrimitiveExpansion::None);
    CHECK(depth_only.fragment.find("void main(){}") != std::string::npos);

    // The desktop target stays what the validated OpenGL renderer has always run.
    CHECK(plain.vertex.starts_with("#version 330 core\n"));
    CHECK(plain.fragment.starts_with("#version 330 core\n"));
    for (const char* desktop : {"uniform usamplerBuffer dcp_ring;", "uint(gl_VertexID)", "gl_Position[",
                                "gl_ClipDistance[1]=", "void main(){", "texelFetch(dcp_ring,int("}) {
        CHECK(plain.vertex.find(desktop) != std::string::npos);
    }
    for (const char* gles : {"dcp_guest", "dcp_pos", "dcp_vid", "dcp_iid", "precision", "#extension",
                             "vs_vertex_id_base"}) {
        CHECK(plain.vertex.find(gles) == std::string::npos);
        CHECK(plain.fragment.find(gles) == std::string::npos);
    }
    CHECK(vertex.vertex_inputs.empty());
}

// glslang does not know GL_EXT_clip_cull_distance, which ANGLE's translator does: only that part is hidden from it.
std::string ForGlslang(std::string source) {
    const auto erase = [&source](const std::string& text, const std::string& with) {
        for (size_t at = source.find(text); at != std::string::npos; at = source.find(text, at + with.size())) {
            source.replace(at, text.size(), with);
        }
    };
    erase("#extension GL_EXT_clip_cull_distance : require\n", "");
    erase("out highp float gl_ClipDistance[2];\n", "float clip_out[2];\n");
    erase("gl_ClipDistance", "clip_out");
    return source;
}

void TestGlesShaders(const std::filesystem::path& folder) {
    Shader::Profile profile{};
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    Shader::Pools pools;
    Shader::Info vertex_info{Shader::HwStage::Vertex, Shader::SwStage::Vertex, Params(4)};
    Shader::Info fragment_info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, Params(5)};
    auto vertex = VertexStage(pools, vertex_info, profile, Target::Gles300);
    const auto fragment = FragmentStage(pools, fragment_info, profile, Target::Gles300);
    CHECK(vertex.error.empty());
    CHECK(fragment.error.empty());
    if (!vertex.error.empty() || !fragment.error.empty()) {
        std::printf("emitter refused: '%s' '%s'\n", vertex.error.c_str(), fragment.error.c_str());
        return;
    }
    vertex.vertex_inputs.push_back({3, AmdGpu::NumberClass::Float});
    vertex.vertex_inputs.push_back({7, AmdGpu::NumberClass::Uint});

    const auto plain = Shader::Backend::GLSL::LinkStages(vertex, &fragment, PrimitiveExpansion::None);
    CHECK(plain.geometry.empty());
    CHECK(plain.vertex.starts_with("#version 300 es\n"));
    CHECK(plain.fragment.starts_with("#version 300 es\n"));
    CHECK(plain.vertex.find("#extension GL_EXT_clip_cull_distance : require") != std::string::npos);
    CHECK(plain.vertex.find("#extension GL_NV_shader_noperspective_interpolation : require") != std::string::npos);
    CHECK(plain.fragment.find("#extension GL_NV_shader_noperspective_interpolation : require") != std::string::npos);
    CHECK(plain.fragment.find("precision highp float;") != std::string::npos);
    CHECK(plain.vertex.find("precision highp usampler2D;") != std::string::npos);
    CHECK(plain.vertex.find("usamplerBuffer") == std::string::npos);
    CHECK(plain.vertex.find("uniform highp usampler2D dcp_ring;") != std::string::npos);
    CHECK(plain.vertex.find("texelFetch(dcp_ring,ivec2(int(t&0xfffu),int(t>>12u)),0)") != std::string::npos);
    CHECK(plain.vertex.find("layout(location=3) in vec4 attr3;") != std::string::npos);
    CHECK(plain.vertex.find("layout(location=7) in uvec4 attr7;") != std::string::npos);
    CHECK(plain.vertex.find("dcp_vid=uint(gl_VertexID)+vs_vertex_id_base;") != std::string::npos);
    CHECK(plain.vertex.find("gl_Position=dcp_pos;gl_ClipDistance[0]=dcp_clip[0];") != std::string::npos);
    CHECK(plain.fragment.find("void main(){dcp_guest();}") != std::string::npos);
    CHECK(1u << Shader::Backend::GLSL::RingRowBits == 4096);
    Write(folder / "gles_plain.vert", ForGlslang(plain.vertex));
    Write(folder / "gles_plain.frag", plain.fragment);
    Compile(folder, "gles_plain.vert");
    Compile(folder, "gles_plain.frag");

    const auto rect = Shader::Backend::GLSL::LinkStages(vertex, &fragment, PrimitiveExpansion::RectList);
    CHECK(rect.geometry.empty());
    CHECK(rect.vertex.find("layout(location=0) in vec4 attr3_0;") != std::string::npos);
    CHECK(rect.vertex.find("layout(location=2) in vec4 attr3_2;") != std::string::npos);
    CHECK(rect.vertex.find("layout(location=3) in uvec4 attr7_0;") != std::string::npos);
    CHECK(rect.vertex.find("layout(location=5) in uvec4 attr7_2;") != std::string::npos);
    CHECK(rect.vertex.find("dcp_run(0u);dcp_run(1u);dcp_run(2u);") != std::string::npos);
    CHECK(rect.vertex.find("uint(gl_InstanceID)*3u+k") != std::string::npos);
    CHECK(rect.vertex.find("dcp_rect_input") == std::string::npos);
    CHECK(rect.vertex.find("vs_rect_draw") == std::string::npos);
    Write(folder / "gles_rect.vert", ForGlslang(rect.vertex));
    Compile(folder, "gles_rect.vert");

    const auto indexed = Shader::Backend::GLSL::LinkStages(vertex, &fragment, PrimitiveExpansion::IndexedRectList);
    CHECK(indexed.geometry.empty());
    CHECK(indexed.vertex.find("layout(location") == std::string::npos);
    CHECK(indexed.vertex.find("attr3=uintBitsToFloat(dcp_rect_input(0,p,g));") != std::string::npos);
    CHECK(indexed.vertex.find("attr7=(dcp_rect_input(1,p,g));") != std::string::npos);
    CHECK(indexed.vertex.find("dcp_run(0u);dcp_run(1u);dcp_run(2u);") != std::string::npos);
    CHECK(indexed.vertex.find("uint r=uint(gl_InstanceID)%vs_rect_draw.y;") != std::string::npos);
    CHECK(indexed.vertex.find("uint g=uint(gl_InstanceID)/vs_rect_draw.y;") != std::string::npos);
    CHECK(indexed.vertex.find("dcp_vid=dcp_word(vs_rect_draw.x+p);dcp_iid=g;") != std::string::npos);
    CHECK(indexed.vertex.find("uint e=d.y==0u?p:min(g/d.y,d.z);") != std::string::npos);
    CHECK(indexed.vertex.find("if(gl_VertexID<3)") != std::string::npos);
    Write(folder / "gles_indexed_rect.vert", ForGlslang(indexed.vertex));
    Compile(folder, "gles_indexed_rect.vert");

    auto many = vertex;
    many.vertex_inputs.clear();
    for (u32 semantic = 0; semantic < 20; ++semantic) {
        const auto number_class = semantic % 3 == 0   ? AmdGpu::NumberClass::Float
                                  : semantic % 3 == 1 ? AmdGpu::NumberClass::Uint
                                                      : AmdGpu::NumberClass::Sint;
        many.vertex_inputs.push_back({semantic, number_class});
    }
    const auto many_inputs = Shader::Backend::GLSL::LinkStages(many, &fragment, PrimitiveExpansion::IndexedRectList);
    CHECK(many_inputs.vertex.find("uniform uvec4 vs_rect_inputs[20];") != std::string::npos);
    CHECK(many_inputs.vertex.find("attr19=(dcp_rect_input(19,p,g));") != std::string::npos);
    CHECK(many_inputs.vertex.find("attr2=ivec4(dcp_rect_input(2,p,g));") != std::string::npos);
    Write(folder / "gles_rect_inputs.vert", ForGlslang(many_inputs.vertex));
    Compile(folder, "gles_rect_inputs.vert");

    Shader::Info generated_info{Shader::HwStage::Vertex, Shader::SwStage::Vertex, Params(7)};
    const auto generated = VertexIdOnlyStage(pools, generated_info, profile);
    CHECK(generated.error.empty());
    CHECK(generated.vertex_inputs.empty());
    CHECK(!generated.uses_ring);
    const auto bare = Shader::Backend::GLSL::LinkStages(generated, &fragment, PrimitiveExpansion::IndexedRectList);
    CHECK(bare.vertex.find("vs_rect_inputs") == std::string::npos);
    CHECK(bare.vertex.find("uniform highp usampler2D dcp_ring;") != std::string::npos);
    CHECK(bare.vertex.find("dcp_vid=dcp_word(vs_rect_draw.x+p);") != std::string::npos);
    Write(folder / "gles_rect_no_inputs.vert", ForGlslang(bare.vertex));
    Compile(folder, "gles_rect_no_inputs.vert");
    const auto attribute_path = Shader::Backend::GLSL::LinkStages(generated, &fragment, PrimitiveExpansion::RectList);
    CHECK(attribute_path.vertex.find("dcp_ring") == std::string::npos);
    CHECK(attribute_path.vertex.find("dcp_vid=vs_vertex_id_base+uint(gl_InstanceID)*3u+k;") != std::string::npos);
    Write(folder / "gles_plain_rect_no_inputs.vert", ForGlslang(attribute_path.vertex));
    Compile(folder, "gles_plain_rect_no_inputs.vert");

    CHECK(OpenGL::ExpansionOf(AmdGpu::PrimitiveType::QuadList, Target::Gles300) == PrimitiveExpansion::None);
    CHECK(OpenGL::ExpansionOf(AmdGpu::PrimitiveType::QuadList, Target::Desktop330) == PrimitiveExpansion::QuadList);
    CHECK(OpenGL::ExpansionOf(AmdGpu::PrimitiveType::RectList, Target::Gles300) == PrimitiveExpansion::RectList);
    CHECK(OpenGL::ExpansionOf(AmdGpu::PrimitiveType::RectList, Target::Gles300, true) ==
          PrimitiveExpansion::IndexedRectList);
    CHECK(OpenGL::ExpansionOf(AmdGpu::PrimitiveType::RectList, Target::Desktop330) == PrimitiveExpansion::RectList);

    const auto depth_only = Shader::Backend::GLSL::LinkStages(vertex, nullptr, PrimitiveExpansion::None);
    CHECK(depth_only.fragment.find("void main(){}") != std::string::npos);
    Write(folder / "gles_depth.frag", depth_only.fragment);
    Compile(folder, "gles_depth.frag");
}

void TestGlesRefusesOneDimensionalImages() {
    Shader::Profile profile{};
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    Shader::Pools pools;
    Shader::Info info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, Params(6)};
    const auto stage = FragmentStage(pools, info, profile, Target::Gles300, AmdGpu::ImageType::Color1D);
    CHECK(stage.error.find("1D images") != std::string::npos);
}

void TestQuadExpansion() {
    std::vector<u32> out;
    OpenGL::ExpandSequentialQuads(8, out);
    CHECK((out == std::vector<u32>{0, 1, 3, 3, 1, 2, 4, 5, 7, 7, 5, 6}));
    OpenGL::ExpandSequentialQuads(6, out);
    CHECK(out.size() == 6);
    OpenGL::ExpandSequentialQuads(0, out);
    CHECK(out.empty());
    const std::array<u16, 4> quad16{10, 20, 30, 40};
    OpenGL::ExpandQuadIndices(std::span<const u16>{quad16}, false, out);
    CHECK((out == std::vector<u32>{10, 20, 40, 40, 20, 30}));
    const std::array<u32, 5> quad32{1, 2, 3, 4, 5};
    OpenGL::ExpandQuadIndices(std::span<const u32>{quad32}, false, out);
    CHECK(out.size() == 6);
    const std::array<u16, 9> restarted{1, 2, 0xFFFF, 3, 4, 5, 6, 7, 8};
    OpenGL::ExpandQuadIndices(std::span<const u16>{restarted}, true, out);
    CHECK((out == std::vector<u32>{3, 4, 6, 6, 4, 5}));
}

void TestRectAssembly() {
    std::vector<u32> out{99};

    // The real first draw: a three-vertex patch list takes floor(4 / 3) patches, so one rectangle.
    const std::array<u16, 4> real16{0, 1, 2, 3};
    OpenGL::AssembleRectVertices(std::span<const u16>{real16}, false, 0, out);
    CHECK((out == std::vector<u32>{0, 1, 2}));
    const std::array<u32, 4> real32{0, 1, 2, 3};
    OpenGL::AssembleRectVertices(std::span<const u32>{real32}, false, 0, out);
    CHECK((out == std::vector<u32>{0, 1, 2}));
    // That draw's vertex shader, 0x4c12efc8: the mirrored corner of the first three is vertex 3.
    const auto corner = [](u32 id) { return std::array<s32, 2>{s32((id & 1) * 2) - 1, s32(id & ~1u) - 1}; };
    const auto a = corner(0), b = corner(1), c = corner(2);
    CHECK((std::array<s32, 2>{b[0] + c[0] - a[0], b[1] + c[1] - a[1]} == corner(3)));

    const std::array<u16, 6> two16{4, 5, 6, 0, 1, 2};
    OpenGL::AssembleRectVertices(std::span<const u16>{two16}, false, 0, out);
    CHECK((out == std::vector<u32>{4, 5, 6, 0, 1, 2}));
    OpenGL::AssembleRectVertices(std::span<const u16>{two16}, false, 100, out);
    CHECK((out == std::vector<u32>{104, 105, 106, 100, 101, 102}));

    const std::array<u32, 6> two32{70000, 70001, 70002, 3, 4, 5};
    OpenGL::AssembleRectVertices(std::span<const u32>{two32}, false, 1, out);
    CHECK((out == std::vector<u32>{70001, 70002, 70003, 4, 5, 6}));

    // A rectangle repeats no vertex: reused indices stay separate guest vertices.
    const std::array<u16, 6> shared{0, 1, 2, 2, 1, 3};
    OpenGL::AssembleRectVertices(std::span<const u16>{shared}, false, 0, out);
    CHECK((out == std::vector<u32>{0, 1, 2, 2, 1, 3}));

    const std::array<u16, 7> restart16{1, 2, 0xFFFF, 3, 4, 5, 6};
    OpenGL::AssembleRectVertices(std::span<const u16>{restart16}, true, 0, out);
    CHECK((out == std::vector<u32>{3, 4, 5}));
    OpenGL::AssembleRectVertices(std::span<const u16>{restart16}, false, 0, out);
    CHECK((out == std::vector<u32>{1, 2, 0xFFFF, 3, 4, 5}));
    const std::array<u32, 4> restart32{7, 0xFFFFFFFF, 8, 9};
    OpenGL::AssembleRectVertices(std::span<const u32>{restart32}, true, 0, out);
    CHECK(out.empty());

    // The last whole rectangle is kept and an interrupted tail is not.
    constexpr u32 index_size = sizeof(u16);
    CHECK(OpenGL::ReadableIndexCount(9, 7 * index_size, index_size) == 7);
    CHECK(OpenGL::ReadableIndexCount(9, 100 * index_size, index_size) == 9);
    CHECK(OpenGL::ReadableIndexCount(9, 0, index_size) == 0);
    CHECK(OpenGL::ReadableIndexCount(9, 3, index_size) == 1);
    const std::array<u16, 9> truncated{0, 1, 2, 3, 4, 5, 6, 7, 8};
    OpenGL::AssembleRectVertices(std::span<const u16>{truncated}.first(7), false, 0, out);
    CHECK(out.size() == 6);
    OpenGL::AssembleRectVertices(std::span<const u16>{}, false, 0, out);
    CHECK(out.empty());

    const std::array<u32, 3> wraps{0xFFFFFFFE, 0xFFFFFFFF, 0};
    OpenGL::AssembleRectVertices(std::span<const u32>{wraps}, false, 2, out);
    CHECK((out == std::vector<u32>{0, 1, 2}));

    CHECK(OpenGL::RectDrawInstances(3, 4) == 12);
    CHECK(OpenGL::RectDrawInstances(1, 1) == 1);
    CHECK(OpenGL::RectDrawInstances(0, 9) == 0);
    CHECK(!OpenGL::RectDrawInstances(0x10000, 0x10000).has_value());
    CHECK(!OpenGL::RectDrawInstances(0xFFFFFFFF, 0xFFFFFFFF).has_value());

    CHECK(OpenGL::RectInstanceElements(1, 1, 100) == 1);
    CHECK(OpenGL::RectInstanceElements(8, 1, 100) == 8);
    CHECK(OpenGL::RectInstanceElements(8, 3, 100) == 3);
    CHECK(OpenGL::RectInstanceElements(9, 3, 100) == 3);
    CHECK(OpenGL::RectInstanceElements(10, 3, 100) == 4);
    CHECK(OpenGL::RectInstanceElements(0xFFFFFFFF, 1, 2) == 3);
    CHECK(OpenGL::RectInstanceElements(5, 0xFFFFFFFF, 0) == 1);
}

void TestIndexBounds() {
    using OpenGL::HighestElementInside;
    using OpenGL::HighestIndex;
    const std::array<u16, 4> indices16{3, 0xFFFF, 9, 2};
    CHECK(HighestIndex(std::span<const u16>{indices16}, true) == 9u);
    CHECK(HighestIndex(std::span<const u16>{indices16}, false) == 0xFFFFu);
    const std::array<u32, 2> rogue{1, 0xFFFFFFF0};
    CHECK(HighestIndex(std::span<const u32>{rogue}, true) == 0xFFFFFFF0u);
    const std::array<u32, 2> restart_only{0xFFFFFFFF, 0xFFFFFFFF};
    CHECK(!HighestIndex(std::span<const u32>{restart_only}, true).has_value());
    CHECK(!HighestIndex(std::span<const u16>{}, false).has_value());

    CHECK(HighestElementInside(0, 16, 16, 64) == 3u);
    CHECK(HighestElementInside(16, 16, 16, 64) == 2u);
    CHECK(HighestElementInside(0, 12, 16, 64) == 3u);
    CHECK(HighestElementInside(48, 16, 16, 64) == 0u);
    CHECK(!HighestElementInside(60, 16, 16, 64).has_value());
    CHECK(HighestElementInside(0, 16, 0, 64) == ~0ull);
    constexpr u64 rogue_element = 0xFFFFFFF0ull;
    CHECK(rogue_element > *HighestElementInside(0, 16, 32, 64ull * 1024 * 1024));
}

void TestVertexElements() {
    using F = vk::Format;
    using OpenGL::FindVertexFormat;
    using OpenGL::ReadVertexElement;
    const auto bits = [](float value) { return std::bit_cast<u32>(value); };
    const u32 one = bits(1.0f);
    const auto read = [&](F format, const auto& source, u32 stride = 0, u64 element = 0) {
        return ReadVertexElement(*FindVertexFormat(format), reinterpret_cast<const u8*>(source.data()),
                                 sizeof(source), stride, element);
    };

    const std::array<float, 3> position{1.5f, -2.0f, 0.25f};
    CHECK((read(F::eR32G32B32Sfloat, position) == OpenGL::VertexElement{bits(1.5f), bits(-2.0f), bits(0.25f), one}));
    const std::array<u8, 4> unorm8{255, 0, 51, 102};
    CHECK((read(F::eR8G8B8A8Unorm, unorm8) ==
           OpenGL::VertexElement{bits(1.0f), bits(0.0f), bits(0.2f), bits(0.4f)}));
    const std::array<s8, 2> snorm8{127, -128};
    CHECK((read(F::eR8G8Snorm, snorm8) == OpenGL::VertexElement{bits(1.0f), bits(-1.0f), 0, one}));
    const std::array<u16, 2> unorm16{65535, 0};
    CHECK((read(F::eR16G16Unorm, unorm16) == OpenGL::VertexElement{bits(1.0f), 0, 0, one}));
    const std::array<u8, 2> uscaled{200, 7};
    CHECK((read(F::eR8G8Uscaled, uscaled) == OpenGL::VertexElement{bits(200.0f), bits(7.0f), 0, one}));
    const std::array<s16, 1> sscaled{-300};
    CHECK((read(F::eR16Sscaled, sscaled) == OpenGL::VertexElement{bits(-300.0f), 0, 0, one}));
    const std::array<u8, 2> integers{200, 7};
    CHECK((read(F::eR8G8Uint, integers) == OpenGL::VertexElement{200, 7, 0, 1}));
    const std::array<s8, 1> negative{-2};
    CHECK((read(F::eR8Sint, negative) == OpenGL::VertexElement{0xFFFFFFFE, 0, 0, 1}));
    const std::array<u32, 1> wide{0xDEADBEEF};
    CHECK((read(F::eR32Uint, wide) == OpenGL::VertexElement{0xDEADBEEF, 0, 0, 1}));
    const std::array<u16, 4> halves{0x3C00, 0xC000, 0x3800, 0x0000};
    CHECK((read(F::eR16G16B16A16Sfloat, halves) ==
           OpenGL::VertexElement{bits(1.0f), bits(-2.0f), bits(0.5f), bits(0.0f)}));
    const std::array<u32, 1> packed{0x3FFu | (0x000u << 10) | (0x155u << 20) | (3u << 30)};
    const auto unsigned_packed = read(F::eA2B10G10R10UnormPack32, packed);
    CHECK(unsigned_packed[0] == bits(1.0f) && unsigned_packed[1] == 0 && unsigned_packed[3] == bits(1.0f));
    const std::array<u32, 1> signed_packed{0x1FFu | (0x201u << 10) | (0u << 20) | (1u << 30)};
    const auto signed_element = read(F::eA2B10G10R10SnormPack32, signed_packed);
    CHECK(signed_element[0] == bits(1.0f) && signed_element[1] == bits(-1.0f) && signed_element[3] == bits(1.0f));

    const std::array<u32, 6> interleaved{10, 11, 12, 20, 21, 22};
    CHECK(read(F::eR32Uint, interleaved, 12, 1)[0] == 20);
    CHECK(read(F::eR32Uint, interleaved, 12, 0)[0] == 10);

    // Past the data, an element reads as an attribute without data does.
    const OpenGL::VertexElement missing_float{0, 0, 0, one};
    const OpenGL::VertexElement missing_integer{0, 0, 0, 1};
    CHECK(read(F::eR32G32B32Sfloat, position, 12, 1) == missing_float);
    CHECK(read(F::eR32Uint, interleaved, 12, 2) == missing_integer);
    CHECK(read(F::eR32Uint, interleaved, 12, 0xFFFFFFFF) == missing_integer);
    CHECK(read(F::eR32Uint, interleaved, 16383, ~0ull) == missing_integer);
    const std::array<u8, 3> short_data{1, 2, 3};
    CHECK(read(F::eR32Uint, short_data) == missing_integer);
    CHECK(ReadVertexElement(*FindVertexFormat(F::eR32Sfloat), nullptr, 64, 4, 0) == missing_float);
    CHECK(ReadVertexElement(*FindVertexFormat(F::eR32Sfloat), interleaved.data() ? reinterpret_cast<const u8*>(interleaved.data()) : nullptr, 0, 4, 0) == missing_float);

    CHECK(OpenGL::VertexFormatBytes(*FindVertexFormat(F::eR32G32B32Sfloat)) == 12);
    CHECK(OpenGL::VertexFormatBytes(*FindVertexFormat(F::eA2B10G10R10UnormPack32)) == 4);
    CHECK(OpenGL::VertexFormatBytes(*FindVertexFormat(F::eR16G16Unorm)) == 4);
    CHECK(OpenGL::VertexFormatBytes(*FindVertexFormat(F::eR8Unorm)) == 1);
}

void TestGlesFormats() {
    using F = vk::Format;
    using OpenGL::FindTextureFormat;
    using OpenGL::TexelConversion;
    // The desktop table keeps the layouts OpenGL ES does not have.
    CHECK(FindTextureFormat(F::eB8G8R8A8Unorm)->format == GL_BGRA);
    CHECK(FindTextureFormat(F::eB8G8R8A8Unorm)->conversion == TexelConversion::None);
    CHECK(FindTextureFormat(F::eR8G8B8A8Unorm, false) != FindTextureFormat(F::eR8G8B8A8Unorm, true));

    for (const F format : {F::eB8G8R8A8Unorm, F::eB8G8R8A8Srgb, F::eA2R10G10B10UnormPack32, F::eB5G6R5UnormPack16,
                           F::eB4G4R4A4UnormPack16, F::eA1R5G5B5UnormPack16}) {
        const auto* gles = FindTextureFormat(format, true);
        CHECK(gles->format != GL_BGRA && gles->format != GL_BGR);
        CHECK(gles->conversion != TexelConversion::None);
    }
    CHECK(OpenGL::StoresBlueFirst(*FindTextureFormat(F::eB8G8R8A8Unorm, true)));
    CHECK(OpenGL::StoresBlueFirst(*FindTextureFormat(F::eB8G8R8A8Unorm, false)));
    CHECK(!OpenGL::StoresBlueFirst(*FindTextureFormat(F::eR8G8B8A8Unorm, true)));
    CHECK(FindTextureFormat(F::eR8G8B8A8Srgb, true)->internal_format == GL_SRGB8_ALPHA8);
    CHECK(FindTextureFormat(F::eBc1RgbaUnormBlock, true)->compressed);

    OpenGL::Capabilities caps;
    CHECK(OpenGL::FormatAvailable(*FindTextureFormat(F::eBc1RgbaUnormBlock, true), caps));
    CHECK(!OpenGL::FormatAvailable(*FindTextureFormat(F::eBc7UnormBlock, true), caps));
    CHECK(!OpenGL::FormatAvailable(*FindTextureFormat(F::eBc5UnormBlock, true), caps));
    CHECK(!OpenGL::FormatAvailable(*FindTextureFormat(F::eBc1RgbaSrgbBlock, true), caps));
    CHECK(!OpenGL::FormatAvailable(*FindTextureFormat(F::eR16Unorm, true), caps));
    caps.bptc = caps.rgtc = caps.s3tc_srgb = caps.norm16 = true;
    CHECK(OpenGL::FormatAvailable(*FindTextureFormat(F::eBc7UnormBlock, true), caps));
    CHECK(OpenGL::FormatAvailable(*FindTextureFormat(F::eR16Unorm, true), caps));

    const auto convert = [](TexelConversion conversion, auto texel) {
        OpenGL::ConvertTexels(conversion, reinterpret_cast<u8*>(&texel), sizeof(texel));
        return texel;
    };
    CHECK(convert(TexelConversion::SwapRedBlue8888, u32(0x44332211)) == 0x44112233);
    CHECK(convert(TexelConversion::SwapRedBlue2101010, u32(0xC0000000 | (0x155u << 20) | (0x2AAu << 10) | 0x0FFu)) ==
          (0xC0000000 | (0x0FFu << 20) | (0x2AAu << 10) | 0x155u));
    CHECK(convert(TexelConversion::B5G6R5, u16(0b10001'010101'00011)) == 0b00011'010101'10001);
    CHECK(convert(TexelConversion::B4G4R4A4, u16(0x1234)) == 0x3214);
    CHECK(convert(TexelConversion::A1R5G5B5, u16(0b1'10001'01010'00011)) == 0b10001'01010'00011'1);
    CHECK(convert(TexelConversion::None, u32(0x12345678)) == 0x12345678);
}

void TestRefusesWhatGlsl330Lacks() {
    Shader::Profile profile{};
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    Shader::Pools pools;
    Shader::Info info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, Params(3)};
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    info.buffers.push_back({.used_types = IR::Type::U32, .is_written = true});
    IR::Program program{info};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    Op(*block, Opcode::StoreBufferU32, {IR::Value{0u}, IR::Value{0u}, IR::Value{1u}});
    Block(program, block);
    const auto stage = Shader::Backend::GLSL::EmitGLSL(profile, runtime, program);
    CHECK(stage.error.find("StoreBufferU32") != std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    TestGameAddresses();
    TestDetile(TileMode::Thin2DThin, 32, false, true);
    TestDetile(TileMode::Thin2DThin, 32, true, true);
    TestDetile(TileMode::Display2DThin, 32, false, true);
    TestDetile(TileMode::Thin2DThin, 16, false, true);
    TestDetile(TileMode::Thin2DThin, 8, false, true);
    TestDetile(TileMode::Thin1DThin, 32, false, true);
    TestDetile(TileMode::Thin2DThin, 64, false, false);
    const auto folder = argc > 1 ? std::filesystem::path(argv[1])
                                 : std::filesystem::temp_directory_path() / "dcp_opengl_backend_tests";
    std::filesystem::create_directories(folder);
    TestGlsl(folder);
    TestRefusesWhatGlsl330Lacks();
    TestGlesShaders(folder);
    TestGlesRefusesOneDimensionalImages();
    TestQuadExpansion();
    TestRectAssembly();
    TestIndexBounds();
    TestVertexElements();
    TestGlesFormats();
    std::printf("OpenGL backend tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
