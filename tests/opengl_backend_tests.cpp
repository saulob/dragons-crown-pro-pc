// OpenGL backend tests without OpenGL: the CPU detiler against the game's own tiler, and the GLSL the emitter writes,
// compiled by glslangValidator. No context, no window, no game files.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "shader_recompiler/backend/glsl/emit_glsl.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_opengl/gl_detile.h"

// Normally the loader's: this test links the video core without it.
extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

namespace {

using AmdGpu::TileMode;
using Shader::Backend::GLSL::PrimitiveExpansion;
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
Shader::Backend::GLSL::Stage VertexStage(Shader::Pools& pools, Shader::Info& info, const Shader::Profile& profile) {
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

    auto stage = Shader::Backend::GLSL::EmitGLSL(profile, runtime, program);
    CHECK(stage.output_mask == 3);
    CHECK(stage.clip_distances == 2);
    return stage;
}

Shader::Backend::GLSL::Stage FragmentStage(Shader::Pools& pools, Shader::Info& info, const Shader::Profile& profile) {
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

    const AmdGpu::Image texture = AmdGpu::Image::Null(false);
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

    auto stage = Shader::Backend::GLSL::EmitGLSL(profile, runtime, program);
    CHECK(stage.samplers.size() == 2);
    CHECK(stage.inputs.size() == 3);
    return stage;
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
    std::printf("OpenGL backend tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
