// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "shader_recompiler/backend/glsl/emit_glsl.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::GLSL {
namespace {

using IR::Opcode;

constexpr const char* HelperBitExtractU =
    "uint bfe_u(uint v,uint o,uint c){return c==0u?0u:(c>=32u?v>>o:(v>>o)&((1u<<c)-1u));}\n";
constexpr const char* HelperBitExtractS =
    "uint bfe_s(uint v,uint o,uint c){if(c==0u)return 0u;"
    "return uint(int(v<<(32u-o-c))>>int(32u-c));}\n";
constexpr const char* HelperBitInsert =
    "uint bfi(uint b,uint i,uint o,uint c){uint m=(c>=32u?0xFFFFFFFFu:((1u<<c)-1u))<<o;"
    "return (b&~m)|((i<<o)&m);}\n";
constexpr const char* HelperBitCount =
    "uint popcnt(uint v){v=v-((v>>1u)&0x55555555u);v=(v&0x33333333u)+((v>>2u)&0x33333333u);"
    "return (((v+(v>>4u))&0x0F0F0F0Fu)*0x01010101u)>>24u;}\n";
constexpr const char* HelperBitReverse =
    "uint bitrev(uint v){v=((v>>1u)&0x55555555u)|((v&0x55555555u)<<1u);"
    "v=((v>>2u)&0x33333333u)|((v&0x33333333u)<<2u);v=((v>>4u)&0x0F0F0F0Fu)|((v&0x0F0F0F0Fu)<<4u);"
    "v=((v>>8u)&0x00FF00FFu)|((v&0x00FF00FFu)<<8u);return (v>>16u)|(v<<16u);}\n";
constexpr const char* HelperLsb =
    "uint ilsb(uint v){if(v==0u)return 0xFFFFFFFFu;uint n=0u;while((v&1u)==0u){v>>=1u;++n;}"
    "return n;}\n";
constexpr const char* HelperUmsb =
    "uint umsb(uint v){if(v==0u)return 0xFFFFFFFFu;uint n=31u;"
    "while((v&0x80000000u)==0u){v<<=1u;--n;}return n;}\n";
constexpr const char* HelperSmsb = "uint smsb(uint v){return umsb(int(v)<0?~v:v);}\n";
constexpr const char* HelperUmulHi =
    "uint umulhi(uint a,uint b){uint al=a&0xFFFFu,ah=a>>16u,bl=b&0xFFFFu,bh=b>>16u;uint t=al*bl;"
    "uint m1=ah*bl+(t>>16u);uint m2=al*bh+(m1&0xFFFFu);return ah*bh+(m1>>16u)+(m2>>16u);}\n";
constexpr const char* HelperSmulHi =
    "uint smulhi(uint a,uint b){uint r=umulhi(a,b);if(int(a)<0)r-=b;if(int(b)<0)r-=a;return r;}\n";
constexpr const char* HelperAddCarry =
    "uvec2 uaddc(uint a,uint b){uint r=a+b;return uvec2(r,r<a?1u:0u);}\n";
constexpr const char* HelperSmod =
    "uint smod(uint a,uint b){int x=int(a),y=int(b);int r=x-y*(x/y);"
    "if(r!=0&&((r<0)!=(y<0)))r+=y;return uint(r);}\n";
constexpr const char* HelperLdexp =
    "float ldexpf(float x,uint e){return x*pow(2.0,float(int(e)));}\n";
constexpr const char* HelperFrexp =
    "uint frexp_e(float x){if(x==0.0||isinf(x)||isnan(x))return 0u;"
    "return uint(int((floatBitsToUint(x)>>23u)&0xFFu)-126);}\n"
    "float frexp_s(float x){if(x==0.0||isinf(x)||isnan(x))return x;"
    "return uintBitsToFloat((floatBitsToUint(x)&0x807FFFFFu)|0x3F000000u);}\n";
constexpr const char* HelperPackUnorm2x16 =
    "uint pk_un2x16(vec2 v){uvec2 u=uvec2(round(clamp(v,0.0,1.0)*65535.0));"
    "return u.x|(u.y<<16u);}\n"
    "vec2 up_un2x16(uint p){return vec2(float(p&0xFFFFu),float(p>>16u))/65535.0;}\n";
constexpr const char* HelperPackSnorm2x16 =
    "uint pk_sn2x16(vec2 v){ivec2 i=ivec2(round(clamp(v,-1.0,1.0)*32767.0));"
    "return (uint(i.x)&0xFFFFu)|(uint(i.y)<<16u);}\n"
    "vec2 up_sn2x16(uint p){ivec2 i=ivec2(int(p<<16u)>>16,int(p)>>16);"
    "return clamp(vec2(i)/32767.0,-1.0,1.0);}\n";
constexpr const char* HelperPackUnorm4x8 =
    "uint pk_un4x8(vec4 v){uvec4 u=uvec4(round(clamp(v,0.0,1.0)*255.0));"
    "return u.x|(u.y<<8u)|(u.z<<16u)|(u.w<<24u);}\n"
    "vec4 up_un4x8(uint p){return vec4(float(p&0xFFu),float((p>>8u)&0xFFu),"
    "float((p>>16u)&0xFFu),float(p>>24u))/255.0;}\n";
constexpr const char* HelperPackSnorm4x8 =
    "uint pk_sn4x8(vec4 v){ivec4 i=ivec4(round(clamp(v,-1.0,1.0)*127.0));"
    "return (uint(i.x)&0xFFu)|((uint(i.y)&0xFFu)<<8u)|((uint(i.z)&0xFFu)<<16u)|(uint(i.w)<<24u);}\n"
    "vec4 up_sn4x8(uint p){ivec4 i=ivec4(int(p<<24u)>>24,int(p<<16u)>>24,int(p<<8u)>>24,"
    "int(p)>>24);return clamp(vec4(i)/127.0,-1.0,1.0);}\n";
constexpr const char* HelperPackHalf =
    "uint f2h(float f){uint b=floatBitsToUint(f);uint s=(b>>16u)&0x8000u;"
    "int e=int((b>>23u)&0xFFu)-112;uint m=b&0x7FFFFFu;if(e<=0)return s;"
    "if(e>=31)return s|0x7C00u|(((b&0x7F800000u)==0x7F800000u&&m!=0u)?0x200u:0u);"
    "return s|(uint(e)<<10u)|(m>>13u);}\n"
    "float h2f(uint h){uint s=(h&0x8000u)<<16u;uint e=(h>>10u)&0x1Fu;uint m=h&0x3FFu;"
    "if(e==0u)return uintBitsToFloat(s)+float(m)*0.000000059604645;"
    "if(e==31u)return uintBitsToFloat(s|0x7F800000u|(m<<13u));"
    "return uintBitsToFloat(s|((e+112u)<<23u)|(m<<13u));}\n"
    "uint pk_half(vec2 v){return f2h(v.x)|(f2h(v.y)<<16u);}\n"
    "vec2 up_half(uint p){return vec2(h2f(p&0xFFFFu),h2f(p>>16u));}\n";
constexpr const char* HelperPackUnorm2101010 =
    "uint pk_un1010102(vec4 v){uvec4 u=uvec4(round(clamp(v,0.0,1.0)*vec4(1023.0,1023.0,1023.0,3.0)"
    "));return u.x|(u.y<<10u)|(u.z<<20u)|(u.w<<30u);}\n"
    "vec4 up_un1010102(uint p){return vec4(float(p&0x3FFu),float((p>>10u)&0x3FFu),"
    "float((p>>20u)&0x3FFu),float(p>>30u))/vec4(1023.0,1023.0,1023.0,3.0);}\n";

constexpr u32 NumDimensions(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return 1;
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return 2;
    default:
        return 3;
    }
}

std::string SamplerTypeName(AmdGpu::ImageType type, AmdGpu::NumberClass number_class, bool shadow) {
    const char* base = "2D";
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        base = "1D";
        break;
    case AmdGpu::ImageType::Color1DArray:
        base = "1DArray";
        break;
    case AmdGpu::ImageType::Color2DArray:
        base = "2DArray";
        break;
    case AmdGpu::ImageType::Color3D:
        base = "3D";
        break;
    default:
        break;
    }
    const char* prefix = number_class == AmdGpu::NumberClass::Uint   ? "u"
                         : number_class == AmdGpu::NumberClass::Sint ? "i"
                                                                     : "";
    return fmt::format("{}sampler{}{}", prefix, base, shadow ? "Shadow" : "");
}

const char* VectorType(AmdGpu::NumberClass number_class) {
    switch (number_class) {
    case AmdGpu::NumberClass::Uint:
        return "uvec4";
    case AmdGpu::NumberClass::Sint:
        return "ivec4";
    default:
        return "vec4";
    }
}

const char* QualifierName(Qualifier qualifier) {
    switch (qualifier) {
    case Qualifier::Flat:
        return "flat ";
    case Qualifier::NoPerspective:
        return "noperspective ";
    case Qualifier::Centroid:
        return "centroid ";
    default:
        return "";
    }
}

class Emitter {
public:
    Emitter(const Profile& profile_, const RuntimeInfo& runtime_info_, const IR::Program& program_)
        : profile{profile_}, runtime_info{runtime_info_}, program{program_}, info{program_.info},
          prefix{info.sw_stage == SwStage::Vertex ? "vs_" : "fs_"} {}

    Stage Run() {
        if (info.sw_stage != SwStage::Vertex && info.sw_stage != SwStage::Fragment) {
            Fail(fmt::format("stage {}", magic_enum::enum_name(info.sw_stage)));
            return std::move(stage);
        }
        if (info.sw_stage == SwStage::Vertex && runtime_info.hw_stage != HwStage::Vertex) {
            Fail("a vertex shader outside the VS hardware stage");
            return std::move(stage);
        }
        AssignNames();
        CollectPhiMoves();
        for (const auto& node : program.syntax_list) {
            EmitNode(node);
        }
        BuildDeclarations();
        BuildMain();
        stage.clip_distances = clip_distances;
        return std::move(stage);
    }

private:
    void Fail(std::string what) {
        if (stage.error.empty()) {
            stage.error = std::move(what);
        }
    }

    void AssignNames() {
        u32 next = 1;
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                inst.SetDefinition<u32>(next);
                ++next;
            }
        }
    }

    static IR::Type PhiType(const IR::Inst& phi) {
        std::set<const IR::Inst*> visited{&phi};
        std::vector<const IR::Inst*> pending{&phi};
        while (!pending.empty()) {
            const IR::Inst* current = pending.back();
            pending.pop_back();
            for (size_t i = 0; i < current->NumArgs(); ++i) {
                const IR::Value arg = current->Arg(i);
                if (arg.IsImmediate()) {
                    return arg.Type();
                }
                const IR::Inst* source = arg.Inst();
                if (source->GetOpcode() != Opcode::Phi) {
                    return source->Type();
                }
                if (visited.insert(source).second) {
                    pending.push_back(source);
                }
            }
        }
        return IR::Type::Void;
    }

    std::string TypeName(IR::Type type) {
        switch (type) {
        case IR::Type::U1:
            return "bool";
        case IR::Type::U8:
        case IR::Type::U16:
        case IR::Type::U32:
            return "uint";
        case IR::Type::U64:
        case IR::Type::U32x2:
            return "uvec2";
        case IR::Type::U32x3:
            return "uvec3";
        case IR::Type::U32x4:
            return "uvec4";
        case IR::Type::F32:
            return "float";
        case IR::Type::F32x2:
            return "vec2";
        case IR::Type::F32x3:
            return "vec3";
        case IR::Type::F32x4:
            return "vec4";
        default:
            Fail(fmt::format("value type {}", type));
            return "uint";
        }
    }

    // Phi values leave SSA through two variables: moves land in phi_in at the end of each
    // predecessor and the value is read once at the head of the block. Parallel moves and a
    // condition evaluated after the moves then cannot see a half-updated value.
    void CollectPhiMoves() {
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != Opcode::Phi) {
                    continue;
                }
                for (size_t i = 0; i < inst.NumArgs(); ++i) {
                    phi_moves[inst.PhiBlock(i)].emplace_back(&inst, inst.Arg(i));
                }
            }
        }
    }

    static std::string FloatLiteral(float value) {
        const u32 bits = std::bit_cast<u32>(value);
        if (!std::isnormal(value) && value != 0.0f) {
            return fmt::format("uintBitsToFloat({:#x}u)", bits);
        }
        if (bits == 0x80000000u) {
            return "(-0.0)";
        }
        return value < 0.0f ? fmt::format("({:.9e})", value) : fmt::format("{:.9e}", value);
    }

    std::string Name(const IR::Inst* inst) const {
        return fmt::format("v{}", inst->Definition<u32>());
    }

    std::string Ref(const IR::Value& value) {
        if (!value.IsImmediate()) {
            return Name(value.Inst());
        }
        switch (value.Type()) {
        case IR::Type::U1:
            return value.U1() ? "true" : "false";
        case IR::Type::U8:
            return fmt::format("{}u", value.U8());
        case IR::Type::U16:
            return fmt::format("{}u", value.U16());
        case IR::Type::U32:
            return fmt::format("{:#x}u", value.U32());
        case IR::Type::F32:
            return FloatLiteral(value.F32());
        case IR::Type::U64: {
            const u64 bits = value.U64();
            return fmt::format("uvec2({:#x}u,{:#x}u)", u32(bits), u32(bits >> 32));
        }
        default:
            Fail(fmt::format("immediate of type {}", value.Type()));
            return "0u";
        }
    }

    void Line(const std::string& text) {
        body += text;
        body += '\n';
    }

    void Assign(const IR::Inst& inst, const std::string& expression) {
        body += Name(&inst);
        body += '=';
        body += expression;
        body += ";\n";
    }

    void NeedHelper(const char* helper) {
        if (std::ranges::find(helpers, helper) == helpers.end()) {
            helpers.push_back(helper);
        }
    }

    std::string Call(const char* helper, const char* name, std::initializer_list<std::string> args) {
        NeedHelper(helper);
        return Join(name, args);
    }

    static std::string Join(const char* name, std::initializer_list<std::string> args) {
        std::string text{name};
        text += '(';
        bool first = true;
        for (const auto& arg : args) {
            if (!first) {
                text += ',';
            }
            text += arg;
            first = false;
        }
        text += ')';
        return text;
    }

    void EmitNode(const IR::AbstractSyntaxNode& node) {
        using Type = IR::AbstractSyntaxNode::Type;
        switch (node.type) {
        case Type::Block:
            for (IR::Inst& inst : node.data.block->Instructions()) {
                EmitInst(inst);
            }
            EmitPhiMoves(node.data.block);
            break;
        case Type::If:
            Line(fmt::format("if({}){{", Ref(node.data.if_node.cond)));
            break;
        case Type::EndIf:
            Line("}");
            break;
        case Type::Loop:
            Line("do{");
            break;
        case Type::Repeat:
            Line(fmt::format("}}while({});", Ref(node.data.repeat.cond)));
            break;
        case Type::Break:
            Line(fmt::format("if({})break;", Ref(node.data.break_node.cond)));
            break;
        case Type::Return:
            Line("return;");
            break;
        case Type::Unreachable:
            break;
        }
    }

    void EmitPhiMoves(IR::Block* block) {
        const auto it = phi_moves.find(block);
        if (it == phi_moves.end()) {
            return;
        }
        for (const auto& [phi, value] : it->second) {
            Line(fmt::format("phi{}={};", phi->Definition<u32>(), Ref(value)));
        }
    }

    u32 FlatBufferSlot() {
        for (u32 i = 0; i < info.buffers.size(); ++i) {
            if (info.buffers[i].buffer_type == BufferType::Flatbuf) {
                return i;
            }
        }
        Fail("a constant read without a flattened user data buffer");
        return 0;
    }

    std::string BufferDword(u32 slot, const std::string& index) {
        uses_buffers = true;
        return fmt::format("{}load({},{})", prefix, slot, index);
    }

    std::string LoadVector(u32 slot, const std::string& address, u32 count, bool is_float) {
        std::string text = count == 1 ? std::string{}
                                      : fmt::format("{}vec{}(", is_float ? "" : "u", count);
        for (u32 i = 0; i < count; ++i) {
            const std::string index = i == 0 ? address : fmt::format("{}+{}u", address, i);
            const std::string dword = BufferDword(slot, index);
            if (i != 0) {
                text += ',';
            }
            text += is_float ? fmt::format("uintBitsToFloat({})", dword) : dword;
        }
        return count == 1 ? text : text + ")";
    }

    void EmitBufferLoad(const IR::Inst& inst, u32 count, bool is_float) {
        const u32 slot = inst.Arg(0).U32();
        Assign(inst, LoadVector(slot, Ref(inst.Arg(1)), count, is_float));
    }

    std::string SamplerName(u32 handle, bool shadow, bool fetch) {
        const u32 image = handle & 0xFFFF;
        const u32 sampler = fetch ? NoSampler : handle >> 16;
        for (const auto& existing : stage.samplers) {
            if (existing.image == image && existing.sampler == sampler &&
                existing.shadow == shadow) {
                return existing.name;
            }
        }
        std::string name = fmt::format("{}t{}s{}{}", prefix, image,
                                       sampler == NoSampler ? 99u : sampler, shadow ? "d" : "");
        stage.samplers.push_back({image, sampler, shadow, name});
        return name;
    }

    const ImageResource& Image(u32 handle) const {
        return info.images[handle & 0xFFFF];
    }

    AmdGpu::ImageType ViewType(u32 handle) const {
        const auto& desc = Image(handle);
        return desc.GetSharp(info).GetViewType(desc.is_array);
    }

    AmdGpu::NumberClass ImageClass(u32 handle) const {
        return AmdGpu::GetNumberClass(Image(handle).GetSharp(info).GetNumberFmt());
    }

    std::string TexelToFloat(u32 handle, const std::string& expression) const {
        switch (ImageClass(handle)) {
        case AmdGpu::NumberClass::Uint:
            return fmt::format("uintBitsToFloat({})", expression);
        case AmdGpu::NumberClass::Sint:
            return fmt::format("intBitsToFloat({})", expression);
        default:
            return expression;
        }
    }

    // Texture offsets must be constant expressions in GLSL.
    std::string Offset(const IR::Value& offset, u32 dimensions) {
        const auto single = [](u32 value) { return fmt::format("{}", s32(value)); };
        if (offset.IsImmediate()) {
            if (dimensions == 1) {
                return single(offset.U32());
            }
            Fail("a scalar texel offset on a multi-dimensional image");
            return "0";
        }
        const IR::Inst* inst = offset.Inst();
        if (inst->AreAllArgsImmediates()) {
            switch (inst->GetOpcode()) {
            case Opcode::CompositeConstructU32x2:
                return fmt::format("ivec2({},{})", single(inst->Arg(0).U32()),
                                   single(inst->Arg(1).U32()));
            case Opcode::CompositeConstructU32x3:
                return fmt::format("ivec3({},{},{})", single(inst->Arg(0).U32()),
                                   single(inst->Arg(1).U32()), single(inst->Arg(2).U32()));
            default:
                break;
            }
        }
        Fail("a texel offset that is not constant");
        return "0";
    }

    static u32 OffsetDimensions(AmdGpu::ImageType type) {
        switch (type) {
        case AmdGpu::ImageType::Color1D:
        case AmdGpu::ImageType::Color1DArray:
            return 1;
        case AmdGpu::ImageType::Color3D:
            return 3;
        default:
            return 2;
        }
    }

    std::string ShadowCoords(AmdGpu::ImageType type, const std::string& coords,
                             const std::string& dref) {
        switch (type) {
        case AmdGpu::ImageType::Color1D:
            return fmt::format("vec3({},0.0,{})", coords, dref);
        case AmdGpu::ImageType::Color2D:
        case AmdGpu::ImageType::Color1DArray:
            return fmt::format("vec3({},{})", coords, dref);
        case AmdGpu::ImageType::Color2DArray:
            return fmt::format("vec4({},{})", coords, dref);
        default:
            Fail("a depth comparison on this image type");
            return coords;
        }
    }

    void EmitSampleImplicit(const IR::Inst& inst) {
        const u32 handle = inst.Arg(0).U32();
        const auto sampler = SamplerName(handle, false, false);
        const auto coords = Ref(inst.Arg(1));
        const IR::Value bias = inst.Arg(2);
        const IR::Value offset = inst.Arg(3);
        const auto bias_text = bias.IsEmpty() ? std::string{} : "," + Ref(bias);
        std::string call;
        if (offset.IsEmpty()) {
            call = fmt::format("texture({},{}{})", sampler, coords, bias_text);
        } else {
            call = fmt::format("textureOffset({},{},{}{})", sampler, coords,
                               Offset(offset, OffsetDimensions(ViewType(handle))), bias_text);
        }
        Assign(inst, TexelToFloat(handle, call));
    }

    void EmitSampleExplicit(const IR::Inst& inst) {
        const u32 handle = inst.Arg(0).U32();
        const auto sampler = SamplerName(handle, false, false);
        const auto coords = Ref(inst.Arg(1));
        const auto lod = Ref(inst.Arg(2));
        const IR::Value offset = inst.Arg(3);
        std::string call;
        if (offset.IsEmpty()) {
            call = fmt::format("textureLod({},{},{})", sampler, coords, lod);
        } else {
            call = fmt::format("textureLodOffset({},{},{},{})", sampler, coords, lod,
                               Offset(offset, OffsetDimensions(ViewType(handle))));
        }
        Assign(inst, TexelToFloat(handle, call));
    }

    void EmitSampleDref(const IR::Inst& inst, bool explicit_lod) {
        const u32 handle = inst.Arg(0).U32();
        const auto type = ViewType(handle);
        const auto sampler = SamplerName(handle, true, false);
        const auto coords = ShadowCoords(type, Ref(inst.Arg(1)), Ref(inst.Arg(2)));
        const IR::Value lod_or_bias = inst.Arg(3);
        const IR::Value offset = inst.Arg(4);
        if (!offset.IsEmpty()) {
            Fail("a depth comparison with a texel offset");
        }
        if (type == AmdGpu::ImageType::Color2DArray && explicit_lod) {
            Fail("an explicit LOD depth comparison on an array image");
        }
        std::string call;
        if (explicit_lod) {
            call = fmt::format("textureLod({},{},{})", sampler, coords, Ref(lod_or_bias));
        } else if (lod_or_bias.IsEmpty() || type == AmdGpu::ImageType::Color2DArray) {
            call = fmt::format("texture({},{})", sampler, coords);
        } else {
            call = fmt::format("texture({},{},{})", sampler, coords, Ref(lod_or_bias));
        }
        Assign(inst, fmt::format("vec4({},0.0,0.0,0.0)", call));
    }

    void EmitImageRead(const IR::Inst& inst) {
        const u32 handle = inst.Arg(0).U32();
        if (Image(handle).is_written) {
            Fail("storage images");
            return;
        }
        const auto type = ViewType(handle);
        const u32 dimensions = NumDimensions(type);
        const auto sampler = SamplerName(handle, false, true);
        const auto coords = Ref(inst.Arg(1));
        const IR::Value lod = inst.Arg(2);
        const auto lod_text = lod.IsEmpty() ? std::string{"0"} : fmt::format("int({})", Ref(lod));
        const auto coord_text = dimensions == 1 ? fmt::format("int({})", coords)
                                                : fmt::format("ivec{}({})", dimensions, coords);
        Assign(inst, TexelToFloat(handle, fmt::format("texelFetch({},{},{})", sampler, coord_text,
                                                      lod_text)));
    }

    void EmitQueryDimensions(const IR::Inst& inst) {
        const u32 handle = inst.Arg(0).U32();
        if (inst.Arg(2).U1()) {
            Fail("a mip level count query");
        }
        const auto type = ViewType(handle);
        const auto sampler = SamplerName(handle, false, true);
        const auto size = fmt::format("textureSize({},int({}))", sampler, Ref(inst.Arg(1)));
        switch (NumDimensions(type)) {
        case 1:
            Assign(inst, fmt::format("uvec4(uint({}),0u,0u,0u)", size));
            break;
        case 2:
            Assign(inst, fmt::format("uvec4(uvec2({}),0u,0u)", size));
            break;
        default:
            Assign(inst, fmt::format("uvec4(uvec3({}),0u)", size));
            break;
        }
    }

    std::string InputParameter(u32 index, u32 comp) {
        if (info.sw_stage == SwStage::Vertex) {
            if (!vs_inputs_ready) {
                PrepareVertexInputs();
            }
            const auto attribute = vs_attributes.find(index);
            if (attribute == vs_attributes.end()) {
                Fail(fmt::format("reading vertex input {} the fetch shader never loads", index));
                return "0.0";
            }
            const auto name = fmt::format("attr{}[{}]", index, comp);
            switch (attribute->second) {
            case AmdGpu::NumberClass::Uint:
                return fmt::format("uintBitsToFloat({})", name);
            case AmdGpu::NumberClass::Sint:
                return fmt::format("intBitsToFloat({})", name);
            default:
                return name;
            }
        }
        const auto& input = runtime_info.hw.fs.inputs[index];
        const u32 location = input.param_index;
        if (std::ranges::none_of(stage.inputs,
                                 [&](const auto& known) { return known.location == location; })) {
            stage.inputs.push_back({location, info.fs_interpolation[index]});
        }
        return fmt::format("dcp_p{}[{}]", location, comp);
    }

    void PrepareVertexInputs() {
        vs_inputs_ready = true;
        const auto fetch_shader = Gcn::ParseFetchShader(info);
        if (!fetch_shader) {
            return;
        }
        for (const auto& attrib : fetch_shader->attributes) {
            const auto sharp = attrib.GetSharp(info);
            vs_attributes.emplace(attrib.semantic, AmdGpu::GetNumberClass(sharp.GetNumberFmt()));
        }
    }

    void EmitGetAttribute(const IR::Inst& inst) {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        const u32 comp = inst.Arg(1).U32();
        if (IR::IsParam(attr)) {
            const u32 index = u32(attr) - u32(IR::Attribute::Param0);
            Assign(inst, InputParameter(index, comp));
            return;
        }
        if (attr == IR::Attribute::FragCoord) {
            Assign(inst, fmt::format("gl_FragCoord[{}]", comp));
            return;
        }
        Fail(fmt::format("reading attribute {}", IR::NameOf(attr)));
    }

    void EmitGetAttributeU1(const IR::Inst& inst) {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        if (attr == IR::Attribute::IsFrontFace) {
            Assign(inst, "gl_FrontFacing");
            return;
        }
        Fail(fmt::format("reading attribute {}", IR::NameOf(attr)));
    }

    void EmitGetAttributeU32(const IR::Inst& inst) {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        switch (attr) {
        case IR::Attribute::VertexId:
            Assign(inst, "uint(gl_VertexID)");
            return;
        case IR::Attribute::InstanceId:
            // InstanceIndex includes the base instance; gl_InstanceID does not.
            uses_base_instance = true;
            Assign(inst, "uint(gl_InstanceID)+vs_base_instance");
            return;
        case IR::Attribute::BaseVertex:
            uses_base_vertex = true;
            Assign(inst, "vs_base_vertex");
            return;
        case IR::Attribute::BaseInstance:
            uses_base_instance = true;
            Assign(inst, "vs_base_instance");
            return;
        default:
            Fail(fmt::format("reading attribute {}", IR::NameOf(attr)));
        }
    }

    void EmitSetAttribute(const IR::Inst& inst) {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        const auto value = Ref(inst.Arg(1));
        const u32 element = inst.Arg(2).U32();
        if (IR::IsParam(attr)) {
            const u32 index = u32(attr) - u32(IR::Attribute::Param0);
            stage.output_mask |= 1u << index;
            Line(fmt::format("dcp_p{}[{}]={};", index, element, value));
            return;
        }
        if (IR::IsMrt(attr)) {
            const u32 index = u32(attr) - u32(IR::Attribute::RenderTarget0);
            const auto number_class =
                AmdGpu::GetNumberClass(runtime_info.hw.fs.color_buffers[index].num_format);
            mrt_outputs[index] = true;
            mrt_class[index] = number_class;
            const auto stored = number_class == AmdGpu::NumberClass::Uint
                                    ? fmt::format("floatBitsToUint({})", value)
                                : number_class == AmdGpu::NumberClass::Sint
                                    ? fmt::format("floatBitsToInt({})", value)
                                    : value;
            Line(fmt::format("frag{}[{}]={};", index, element, stored));
            return;
        }
        switch (attr) {
        case IR::Attribute::Position0:
            Line(fmt::format("gl_Position[{}]={};", element, value));
            return;
        case IR::Attribute::ClipDistance:
            clip_distances = std::max(clip_distances, element + 1);
            Line(fmt::format("gl_ClipDistance[{}]={};", element, value));
            return;
        case IR::Attribute::PointSize:
            Line(fmt::format("gl_PointSize={};", value));
            return;
        case IR::Attribute::Depth:
            Line(fmt::format("gl_FragDepth={};", value));
            return;
        case IR::Attribute::StencilRef:
            return;
        default:
            Fail(fmt::format("writing attribute {}", IR::NameOf(attr)));
        }
    }

    void EmitEpilogue() {
        if (info.sw_stage != SwStage::Vertex) {
            return;
        }
        const auto& vs = runtime_info.hw.vs;
        if (vs.clip_disable) {
            // Window coordinates over the fixed 16K viewport, as ConvertPositionToClipSpace.
            const float half_width = float(std::min<u32>(profile.max_viewport_width / 2, 8192));
            const float half_height = float(std::min<u32>(profile.max_viewport_height / 2, 8192));
            Line(fmt::format("gl_Position.x=(gl_Position.x*vs_transform.z+vs_transform.x)/{}-1.0;",
                             FloatLiteral(half_width)));
            Line(fmt::format("gl_Position.y=(gl_Position.y*vs_transform.w+vs_transform.y)/{}-1.0;",
                             FloatLiteral(half_height)));
        }
        // emulate_depth_negative_one_to_one here means the guest's clip Z is [0,w], OpenGL's [-w,w].
        if (vs.emulate_depth_negative_one_to_one) {
            Line("gl_Position.z=2.0*gl_Position.z-gl_Position.w;");
        }
        Line("gl_Position.y*=vs_flip_y;");
        uses_transform = true;
    }

    void Binary(const IR::Inst& inst, const char* op) {
        Assign(inst, fmt::format("{}{}{}", Ref(inst.Arg(0)), op, Ref(inst.Arg(1))));
    }

    void Unary(const IR::Inst& inst, const char* name) {
        Assign(inst, fmt::format("{}({})", name, Ref(inst.Arg(0))));
    }

    void Func2(const IR::Inst& inst, const char* name) {
        Assign(inst, fmt::format("{}({},{})", name, Ref(inst.Arg(0)), Ref(inst.Arg(1))));
    }

    void Func3(const IR::Inst& inst, const char* name) {
        Assign(inst, fmt::format("{}({},{},{})", name, Ref(inst.Arg(0)), Ref(inst.Arg(1)),
                                 Ref(inst.Arg(2))));
    }

    std::string S(const IR::Value& value) {
        return fmt::format("int({})", Ref(value));
    }

    void SignedCompare(const IR::Inst& inst, const char* op) {
        Assign(inst, fmt::format("{}{}{}", S(inst.Arg(0)), op, S(inst.Arg(1))));
    }

    void SignedFunc2(const IR::Inst& inst, const char* name) {
        Assign(inst, fmt::format("uint({}({},{}))", name, S(inst.Arg(0)), S(inst.Arg(1))));
    }

    void Median(const IR::Inst& inst, bool is_signed) {
        const auto a = is_signed ? S(inst.Arg(0)) : Ref(inst.Arg(0));
        const auto b = is_signed ? S(inst.Arg(1)) : Ref(inst.Arg(1));
        const auto c = is_signed ? S(inst.Arg(2)) : Ref(inst.Arg(2));
        const auto text = fmt::format("max(min({0},{1}),min(max({0},{1}),{2}))", a, b, c);
        Assign(inst, is_signed ? fmt::format("uint({})", text) : text);
    }

    void Composite(const IR::Inst& inst, const char* type, u32 count) {
        std::string text = fmt::format("{}(", type);
        for (u32 i = 0; i < count; ++i) {
            text += i ? "," : "";
            text += Ref(inst.Arg(i));
        }
        Assign(inst, text + ")");
    }

    void Extract(const IR::Inst& inst) {
        Assign(inst, fmt::format("{}[{}]", Ref(inst.Arg(0)), inst.Arg(1).U32()));
    }

    void Insert(const IR::Inst& inst) {
        Assign(inst, Ref(inst.Arg(0)));
        Line(fmt::format("{}[{}]={};", Name(&inst), inst.Arg(2).U32(), Ref(inst.Arg(1))));
    }

    void Shuffle(const IR::Inst& inst, const char* type, u32 count) {
        const auto first = Ref(inst.Arg(0));
        const auto second = Ref(inst.Arg(1));
        std::string text = fmt::format("{}(", type);
        for (u32 i = 0; i < count; ++i) {
            const u32 comp = inst.Arg(2 + i).U32();
            text += i ? "," : "";
            text += comp < count ? fmt::format("{}[{}]", first, comp)
                                 : fmt::format("{}[{}]", second, comp - count);
        }
        Assign(inst, text + ")");
    }

    void EmitInst(IR::Inst& inst) {
        switch (inst.GetOpcode()) {
        case Opcode::Phi:
            Line(fmt::format("{}=phi{};", Name(&inst), inst.Definition<u32>()));
            break;
        case Opcode::Void:
        case Opcode::Prologue:
        case Opcode::Barrier:
        case Opcode::WorkgroupMemoryBarrier:
        case Opcode::DeviceMemoryBarrier:
            break;
        case Opcode::ConditionRef:
            Assign(inst, Ref(inst.Arg(0)));
            break;
        case Opcode::Epilogue:
            EmitEpilogue();
            break;
        case Opcode::Discard:
            Line("discard;");
            break;
        case Opcode::DiscardCond:
            Line(fmt::format("if({})discard;", Ref(inst.Arg(0))));
            break;
        case Opcode::ReadConst:
            Assign(inst, BufferDword(FlatBufferSlot(), fmt::format("{}u", inst.Flags<u32>())));
            break;
        case Opcode::ReadConstBuffer:
            Assign(inst, BufferDword(inst.Arg(0).U32(), Ref(inst.Arg(1))));
            break;
        case Opcode::GetUserData: {
            const u32 index = info.ud_mask.Index(inst.Arg(0).ScalarReg());
            uses_user_data = true;
            Assign(inst, fmt::format("{}ud[{}][{}]", prefix, index >> 2, index & 3));
            break;
        }
        case Opcode::GetAttribute:
            EmitGetAttribute(inst);
            break;
        case Opcode::GetAttributeU1:
            EmitGetAttributeU1(inst);
            break;
        case Opcode::GetAttributeU32:
            EmitGetAttributeU32(inst);
            break;
        case Opcode::SetAttribute:
            EmitSetAttribute(inst);
            break;
        case Opcode::UndefU1:
            Assign(inst, "false");
            break;
        case Opcode::UndefU8:
        case Opcode::UndefU16:
        case Opcode::UndefU32:
            Assign(inst, "0u");
            break;
        case Opcode::UndefU32x2:
        case Opcode::UndefU64:
            Assign(inst, "uvec2(0u)");
            break;
        case Opcode::UndefU32x3:
            Assign(inst, "uvec3(0u)");
            break;
        case Opcode::UndefU32x4:
            Assign(inst, "uvec4(0u)");
            break;
        case Opcode::UndefF32:
            Assign(inst, "0.0");
            break;

        case Opcode::LoadBufferU8: {
            const auto address = Ref(inst.Arg(1));
            const auto dword =
                BufferDword(inst.Arg(0).U32(), fmt::format("({}>>2u)", address));
            Assign(inst, fmt::format("({}>>(({}&3u)*8u))&0xFFu", dword, address));
            break;
        }
        case Opcode::LoadBufferU16: {
            const auto address = Ref(inst.Arg(1));
            const auto dword =
                BufferDword(inst.Arg(0).U32(), fmt::format("({}>>1u)", address));
            Assign(inst, fmt::format("({}>>(({}&1u)*16u))&0xFFFFu", dword, address));
            break;
        }
        case Opcode::LoadBufferU32:
            EmitBufferLoad(inst, 1, false);
            break;
        case Opcode::LoadBufferU32x2:
            EmitBufferLoad(inst, 2, false);
            break;
        case Opcode::LoadBufferU32x3:
            EmitBufferLoad(inst, 3, false);
            break;
        case Opcode::LoadBufferU32x4:
            EmitBufferLoad(inst, 4, false);
            break;
        case Opcode::LoadBufferF32:
            EmitBufferLoad(inst, 1, true);
            break;
        case Opcode::LoadBufferF32x2:
            EmitBufferLoad(inst, 2, true);
            break;
        case Opcode::LoadBufferF32x3:
            EmitBufferLoad(inst, 3, true);
            break;
        case Opcode::LoadBufferF32x4:
            EmitBufferLoad(inst, 4, true);
            break;

        case Opcode::CompositeConstructU32x2:
            Composite(inst, "uvec2", 2);
            break;
        case Opcode::CompositeConstructU32x3:
            Composite(inst, "uvec3", 3);
            break;
        case Opcode::CompositeConstructU32x4:
            Composite(inst, "uvec4", 4);
            break;
        case Opcode::CompositeConstructU32x2x2:
            Composite(inst, "uvec4", 2);
            break;
        case Opcode::CompositeConstructF32x2:
            Composite(inst, "vec2", 2);
            break;
        case Opcode::CompositeConstructF32x3:
            Composite(inst, "vec3", 3);
            break;
        case Opcode::CompositeConstructF32x4:
            Composite(inst, "vec4", 4);
            break;
        case Opcode::CompositeConstructF32x2x2:
            Composite(inst, "vec4", 2);
            break;
        case Opcode::CompositeExtractU32x2:
        case Opcode::CompositeExtractU32x3:
        case Opcode::CompositeExtractU32x4:
        case Opcode::CompositeExtractF32x2:
        case Opcode::CompositeExtractF32x3:
        case Opcode::CompositeExtractF32x4:
            Extract(inst);
            break;
        case Opcode::CompositeInsertU32x2:
        case Opcode::CompositeInsertU32x3:
        case Opcode::CompositeInsertU32x4:
        case Opcode::CompositeInsertF32x2:
        case Opcode::CompositeInsertF32x3:
        case Opcode::CompositeInsertF32x4:
            Insert(inst);
            break;
        case Opcode::CompositeShuffleU32x2:
            Shuffle(inst, "uvec2", 2);
            break;
        case Opcode::CompositeShuffleU32x3:
            Shuffle(inst, "uvec3", 3);
            break;
        case Opcode::CompositeShuffleU32x4:
            Shuffle(inst, "uvec4", 4);
            break;
        case Opcode::CompositeShuffleF32x2:
            Shuffle(inst, "vec2", 2);
            break;
        case Opcode::CompositeShuffleF32x3:
            Shuffle(inst, "vec3", 3);
            break;
        case Opcode::CompositeShuffleF32x4:
            Shuffle(inst, "vec4", 4);
            break;

        case Opcode::SelectU1:
        case Opcode::SelectU32:
        case Opcode::SelectF32:
        case Opcode::SelectU64:
            Assign(inst, fmt::format("{}?{}:{}", Ref(inst.Arg(0)), Ref(inst.Arg(1)),
                                     Ref(inst.Arg(2))));
            break;

        case Opcode::BitCastU32F32:
            Unary(inst, "floatBitsToUint");
            break;
        case Opcode::BitCastF32U32:
            Unary(inst, "uintBitsToFloat");
            break;
        case Opcode::PackUint2x32:
        case Opcode::UnpackUint2x32:
            Assign(inst, Ref(inst.Arg(0)));
            break;
        case Opcode::PackUnorm2x16:
            Assign(inst, Call(HelperPackUnorm2x16, "pk_un2x16", {Ref(inst.Arg(0))}));
            break;
        case Opcode::UnpackUnorm2x16:
            Assign(inst, Call(HelperPackUnorm2x16, "up_un2x16", {Ref(inst.Arg(0))}));
            break;
        case Opcode::PackSnorm2x16:
            Assign(inst, Call(HelperPackSnorm2x16, "pk_sn2x16", {Ref(inst.Arg(0))}));
            break;
        case Opcode::UnpackSnorm2x16:
            Assign(inst, Call(HelperPackSnorm2x16, "up_sn2x16", {Ref(inst.Arg(0))}));
            break;
        case Opcode::PackHalf2x16:
            Assign(inst, Call(HelperPackHalf, "pk_half", {Ref(inst.Arg(0))}));
            break;
        case Opcode::UnpackHalf2x16:
            Assign(inst, Call(HelperPackHalf, "up_half", {Ref(inst.Arg(0))}));
            break;
        case Opcode::PackUnorm4x8:
            Assign(inst, Call(HelperPackUnorm4x8, "pk_un4x8", {Ref(inst.Arg(0))}));
            break;
        case Opcode::UnpackUnorm4x8:
            Assign(inst, Call(HelperPackUnorm4x8, "up_un4x8", {Ref(inst.Arg(0))}));
            break;
        case Opcode::PackSnorm4x8:
            Assign(inst, Call(HelperPackSnorm4x8, "pk_sn4x8", {Ref(inst.Arg(0))}));
            break;
        case Opcode::UnpackSnorm4x8:
            Assign(inst, Call(HelperPackSnorm4x8, "up_sn4x8", {Ref(inst.Arg(0))}));
            break;
        case Opcode::PackUnorm2_10_10_10:
            Assign(inst, Call(HelperPackUnorm2101010, "pk_un1010102", {Ref(inst.Arg(0))}));
            break;
        case Opcode::UnpackUnorm2_10_10_10:
            Assign(inst, Call(HelperPackUnorm2101010, "up_un1010102", {Ref(inst.Arg(0))}));
            break;

        case Opcode::FPAbs32:
            Unary(inst, "abs");
            break;
        case Opcode::FPAdd32:
            Binary(inst, "+");
            break;
        case Opcode::FPSub32:
            Binary(inst, "-");
            break;
        case Opcode::FPMul32:
            Binary(inst, "*");
            break;
        case Opcode::FPDiv32:
            Binary(inst, "/");
            break;
        case Opcode::FPFma32:
            Assign(inst, fmt::format("{}*{}+{}", Ref(inst.Arg(0)), Ref(inst.Arg(1)),
                                     Ref(inst.Arg(2))));
            break;
        case Opcode::FPMax32:
            Func2(inst, "max");
            break;
        case Opcode::FPMin32:
            Func2(inst, "min");
            break;
        case Opcode::FPMinTri32:
            Assign(inst, fmt::format("min({},min({},{}))", Ref(inst.Arg(0)), Ref(inst.Arg(1)),
                                     Ref(inst.Arg(2))));
            break;
        case Opcode::FPMaxTri32:
            Assign(inst, fmt::format("max({},max({},{}))", Ref(inst.Arg(0)), Ref(inst.Arg(1)),
                                     Ref(inst.Arg(2))));
            break;
        case Opcode::FPMedTri32:
            Assign(inst, fmt::format("max(min({0},{1}),min(max({0},{1}),{2}))", Ref(inst.Arg(0)),
                                     Ref(inst.Arg(1)), Ref(inst.Arg(2))));
            break;
        case Opcode::FPNeg32:
            Assign(inst, fmt::format("-({})", Ref(inst.Arg(0))));
            break;
        case Opcode::FPRecip32:
            Assign(inst, fmt::format("1.0/{}", Ref(inst.Arg(0))));
            break;
        case Opcode::FPRecipSqrt32:
            Unary(inst, "inversesqrt");
            break;
        case Opcode::FPSqrt:
            Unary(inst, "sqrt");
            break;
        case Opcode::FPSin:
            Assign(inst, fmt::format("sin(6.283185307*{})", Ref(inst.Arg(0))));
            break;
        case Opcode::FPCos:
            Assign(inst, fmt::format("cos(6.283185307*{})", Ref(inst.Arg(0))));
            break;
        case Opcode::FPExp2:
            Unary(inst, "exp2");
            break;
        case Opcode::FPPow:
            Func2(inst, "pow");
            break;
        case Opcode::FPLdexp:
            Assign(inst, Call(HelperLdexp, "ldexpf", {Ref(inst.Arg(0)), Ref(inst.Arg(1))}));
            break;
        case Opcode::FPLog2:
            Unary(inst, "log2");
            break;
        case Opcode::FPSaturate32:
            Assign(inst, fmt::format("clamp({},0.0,1.0)", Ref(inst.Arg(0))));
            break;
        case Opcode::FPClamp32:
            Func3(inst, "clamp");
            break;
        case Opcode::FPRoundEven32:
            Unary(inst, "roundEven");
            break;
        case Opcode::FPFloor32:
            Unary(inst, "floor");
            break;
        case Opcode::FPCeil32:
            Unary(inst, "ceil");
            break;
        case Opcode::FPTrunc32:
            Unary(inst, "trunc");
            break;
        case Opcode::FPFract32:
            Unary(inst, "fract");
            break;
        case Opcode::FPFrexpSig32:
            Assign(inst, Call(HelperFrexp, "frexp_s", {Ref(inst.Arg(0))}));
            break;
        case Opcode::FPFrexpExp32:
            Assign(inst, Call(HelperFrexp, "frexp_e", {Ref(inst.Arg(0))}));
            break;
        case Opcode::FPOrdEqual32:
            Binary(inst, "==");
            break;
        case Opcode::FPUnordEqual32:
            Assign(inst, fmt::format("!({0}<{1}||{0}>{1})", Ref(inst.Arg(0)), Ref(inst.Arg(1))));
            break;
        case Opcode::FPOrdNotEqual32:
            Assign(inst, fmt::format("({0}<{1}||{0}>{1})", Ref(inst.Arg(0)), Ref(inst.Arg(1))));
            break;
        case Opcode::FPUnordNotEqual32:
            Binary(inst, "!=");
            break;
        case Opcode::FPOrdLessThan32:
            Binary(inst, "<");
            break;
        case Opcode::FPUnordLessThan32:
            Assign(inst, fmt::format("!({}>={})", Ref(inst.Arg(0)), Ref(inst.Arg(1))));
            break;
        case Opcode::FPOrdGreaterThan32:
            Binary(inst, ">");
            break;
        case Opcode::FPUnordGreaterThan32:
            Assign(inst, fmt::format("!({}<={})", Ref(inst.Arg(0)), Ref(inst.Arg(1))));
            break;
        case Opcode::FPOrdLessThanEqual32:
            Binary(inst, "<=");
            break;
        case Opcode::FPUnordLessThanEqual32:
            Assign(inst, fmt::format("!({}>{})", Ref(inst.Arg(0)), Ref(inst.Arg(1))));
            break;
        case Opcode::FPOrdGreaterThanEqual32:
            Binary(inst, ">=");
            break;
        case Opcode::FPUnordGreaterThanEqual32:
            Assign(inst, fmt::format("!({}<{})", Ref(inst.Arg(0)), Ref(inst.Arg(1))));
            break;
        case Opcode::FPIsNan32:
            Unary(inst, "isnan");
            break;
        case Opcode::FPIsInf32:
            Unary(inst, "isinf");
            break;

        case Opcode::IAdd32:
            Binary(inst, "+");
            break;
        case Opcode::IAddCarry32:
            Assign(inst, Call(HelperAddCarry, "uaddc", {Ref(inst.Arg(0)), Ref(inst.Arg(1))}));
            break;
        case Opcode::ISub32:
            Binary(inst, "-");
            break;
        case Opcode::IMul32:
            Binary(inst, "*");
            break;
        case Opcode::SMulHi:
            NeedHelper(HelperUmulHi);
            Assign(inst, Call(HelperSmulHi, "smulhi", {Ref(inst.Arg(0)), Ref(inst.Arg(1))}));
            break;
        case Opcode::UMulHi:
            Assign(inst, Call(HelperUmulHi, "umulhi", {Ref(inst.Arg(0)), Ref(inst.Arg(1))}));
            break;
        case Opcode::SDiv32:
            Assign(inst, fmt::format("uint({}/{})", S(inst.Arg(0)), S(inst.Arg(1))));
            break;
        case Opcode::UDiv32:
            Binary(inst, "/");
            break;
        case Opcode::SMod32:
            Assign(inst, Call(HelperSmod, "smod", {Ref(inst.Arg(0)), Ref(inst.Arg(1))}));
            break;
        case Opcode::UMod32:
            Binary(inst, "%");
            break;
        case Opcode::INeg32:
            Assign(inst, fmt::format("uint(-{})", S(inst.Arg(0))));
            break;
        case Opcode::IAbs32:
            Assign(inst, fmt::format("uint(abs({}))", S(inst.Arg(0))));
            break;
        case Opcode::ShiftLeftLogical32:
            Binary(inst, "<<");
            break;
        case Opcode::ShiftRightLogical32:
            Binary(inst, ">>");
            break;
        case Opcode::ShiftRightArithmetic32:
            Assign(inst, fmt::format("uint({}>>{})", S(inst.Arg(0)), S(inst.Arg(1))));
            break;
        case Opcode::BitwiseAnd32:
            Binary(inst, "&");
            break;
        case Opcode::BitwiseOr32:
            Binary(inst, "|");
            break;
        case Opcode::BitwiseXor32:
            Binary(inst, "^");
            break;
        case Opcode::BitwiseNot32:
            Assign(inst, fmt::format("~{}", Ref(inst.Arg(0))));
            break;
        case Opcode::BitwiseAnd64:
            Binary(inst, "&");
            break;
        case Opcode::BitwiseOr64:
            Binary(inst, "|");
            break;
        case Opcode::BitwiseXor64:
            Binary(inst, "^");
            break;
        case Opcode::BitwiseNot64:
            Assign(inst, fmt::format("~{}", Ref(inst.Arg(0))));
            break;
        case Opcode::IEqual64:
            Binary(inst, "==");
            break;
        case Opcode::INotEqual64:
            Binary(inst, "!=");
            break;
        case Opcode::BitFieldInsert32:
            Assign(inst, Call(HelperBitInsert, "bfi",
                              {Ref(inst.Arg(0)), Ref(inst.Arg(1)), Ref(inst.Arg(2)),
                               Ref(inst.Arg(3))}));
            break;
        case Opcode::BitFieldSExtract:
            Assign(inst, Call(HelperBitExtractS, "bfe_s",
                              {Ref(inst.Arg(0)), Ref(inst.Arg(1)), Ref(inst.Arg(2))}));
            break;
        case Opcode::BitFieldUExtract:
            Assign(inst, Call(HelperBitExtractU, "bfe_u",
                              {Ref(inst.Arg(0)), Ref(inst.Arg(1)), Ref(inst.Arg(2))}));
            break;
        case Opcode::BitReverse32:
            Assign(inst, Call(HelperBitReverse, "bitrev", {Ref(inst.Arg(0))}));
            break;
        case Opcode::BitCount32:
            Assign(inst, Call(HelperBitCount, "popcnt", {Ref(inst.Arg(0))}));
            break;
        case Opcode::FindSMsb32:
            NeedHelper(HelperUmsb);
            Assign(inst, Call(HelperSmsb, "smsb", {Ref(inst.Arg(0))}));
            break;
        case Opcode::FindUMsb32:
            Assign(inst, Call(HelperUmsb, "umsb", {Ref(inst.Arg(0))}));
            break;
        case Opcode::FindILsb32:
            Assign(inst, Call(HelperLsb, "ilsb", {Ref(inst.Arg(0))}));
            break;
        case Opcode::SMin32:
            SignedFunc2(inst, "min");
            break;
        case Opcode::SMax32:
            SignedFunc2(inst, "max");
            break;
        case Opcode::UMin32:
            Func2(inst, "min");
            break;
        case Opcode::UMax32:
            Func2(inst, "max");
            break;
        case Opcode::SMinTri32:
            Assign(inst, fmt::format("uint(min({},min({},{})))", S(inst.Arg(0)), S(inst.Arg(1)),
                                     S(inst.Arg(2))));
            break;
        case Opcode::SMaxTri32:
            Assign(inst, fmt::format("uint(max({},max({},{})))", S(inst.Arg(0)), S(inst.Arg(1)),
                                     S(inst.Arg(2))));
            break;
        case Opcode::UMinTri32:
            Assign(inst, fmt::format("min({},min({},{}))", Ref(inst.Arg(0)), Ref(inst.Arg(1)),
                                     Ref(inst.Arg(2))));
            break;
        case Opcode::UMaxTri32:
            Assign(inst, fmt::format("max({},max({},{}))", Ref(inst.Arg(0)), Ref(inst.Arg(1)),
                                     Ref(inst.Arg(2))));
            break;
        case Opcode::SMedTri32:
            Median(inst, true);
            break;
        case Opcode::UMedTri32:
            Median(inst, false);
            break;
        case Opcode::SClamp32:
            Assign(inst, fmt::format("uint(clamp({},{},{}))", S(inst.Arg(0)), S(inst.Arg(1)),
                                     S(inst.Arg(2))));
            break;
        case Opcode::UClamp32:
            Func3(inst, "clamp");
            break;
        case Opcode::SLessThan32:
            SignedCompare(inst, "<");
            break;
        case Opcode::SLessThanEqual32:
            SignedCompare(inst, "<=");
            break;
        case Opcode::SGreaterThan32:
            SignedCompare(inst, ">");
            break;
        case Opcode::SGreaterThanEqual32:
            SignedCompare(inst, ">=");
            break;
        case Opcode::ULessThan32:
            Binary(inst, "<");
            break;
        case Opcode::ULessThanEqual32:
            Binary(inst, "<=");
            break;
        case Opcode::UGreaterThan32:
            Binary(inst, ">");
            break;
        case Opcode::UGreaterThanEqual32:
            Binary(inst, ">=");
            break;
        case Opcode::IEqual32:
            Binary(inst, "==");
            break;
        case Opcode::INotEqual32:
            Binary(inst, "!=");
            break;

        case Opcode::LogicalOr:
            Binary(inst, "||");
            break;
        case Opcode::LogicalAnd:
            Binary(inst, "&&");
            break;
        case Opcode::LogicalXor:
            Binary(inst, "!=");
            break;
        case Opcode::LogicalNot:
            Assign(inst, fmt::format("!{}", Ref(inst.Arg(0))));
            break;

        case Opcode::ConvertS32F32:
            Assign(inst, fmt::format("uint(int({}))", Ref(inst.Arg(0))));
            break;
        case Opcode::ConvertU32F32:
            Unary(inst, "uint");
            break;
        case Opcode::ConvertF32S32:
            Assign(inst, fmt::format("float({})", S(inst.Arg(0))));
            break;
        case Opcode::ConvertF32U32:
        case Opcode::ConvertF32U16:
            Unary(inst, "float");
            break;
        case Opcode::ConvertU16U32:
            Assign(inst, fmt::format("{}&0xFFFFu", Ref(inst.Arg(0))));
            break;
        case Opcode::ConvertU8U32:
            Assign(inst, fmt::format("{}&0xFFu", Ref(inst.Arg(0))));
            break;
        case Opcode::ConvertU32U16:
        case Opcode::ConvertU32U8:
            Assign(inst, Ref(inst.Arg(0)));
            break;
        case Opcode::ConvertS32S8:
            Assign(inst, fmt::format("uint(int({}<<24u)>>24)", Ref(inst.Arg(0))));
            break;
        case Opcode::ConvertS32S16:
            Assign(inst, fmt::format("uint(int({}<<16u)>>16)", Ref(inst.Arg(0))));
            break;
        case Opcode::ConvertU64U32:
            Assign(inst, fmt::format("uvec2({},0u)", Ref(inst.Arg(0))));
            break;

        case Opcode::ImageSampleImplicitLod:
            EmitSampleImplicit(inst);
            break;
        case Opcode::ImageSampleExplicitLod:
            EmitSampleExplicit(inst);
            break;
        case Opcode::ImageSampleDrefImplicitLod:
            EmitSampleDref(inst, false);
            break;
        case Opcode::ImageSampleDrefExplicitLod:
            EmitSampleDref(inst, true);
            break;
        case Opcode::ImageGradient: {
            const u32 handle = inst.Arg(0).U32();
            const auto sampler = SamplerName(handle, false, false);
            const auto coords = Ref(inst.Arg(1));
            const auto dx = Ref(inst.Arg(2));
            const auto dy = Ref(inst.Arg(3));
            const IR::Value offset = inst.Arg(4);
            const auto call =
                offset.IsEmpty()
                    ? fmt::format("textureGrad({},{},{},{})", sampler, coords, dx, dy)
                    : fmt::format("textureGradOffset({},{},{},{},{})", sampler, coords, dx, dy,
                                  Offset(offset, OffsetDimensions(ViewType(handle))));
            Assign(inst, TexelToFloat(handle, call));
            break;
        }
        case Opcode::ImageRead:
            EmitImageRead(inst);
            break;
        case Opcode::ImageQueryDimensions:
            EmitQueryDimensions(inst);
            break;

        default:
            Fail(fmt::format("opcode {}", inst.GetOpcode()));
            break;
        }
    }

    void BuildDeclarations() {
        std::string out;
        if (uses_user_data) {
            out += fmt::format("uniform uvec4 {}ud[4];\n", prefix);
        }
        if (uses_buffers) {
            out += "uniform usamplerBuffer dcp_ring;\n";
            out += fmt::format("uniform uvec2 {}buffers[{}];\n", prefix,
                               std::max<size_t>(info.buffers.size(), 1));
            out += fmt::format("uint {0}load(int s,uint i){{return i<{0}buffers[s].y?"
                               "texelFetch(dcp_ring,int({0}buffers[s].x+i)).r:0u;}}\n",
                               prefix);
        }
        if (info.sw_stage == SwStage::Vertex) {
            if (uses_transform) {
                out += "uniform vec4 vs_transform;\nuniform float vs_flip_y;\n";
            }
            if (uses_base_vertex) {
                out += "uniform uint vs_base_vertex;\n";
            }
            if (uses_base_instance) {
                out += "uniform uint vs_base_instance;\n";
            }
            if (!vs_inputs_ready) {
                PrepareVertexInputs();
            }
            for (const auto& [semantic, number_class] : vs_attributes) {
                out += fmt::format("layout(location={}) in {} attr{};\n", semantic,
                                   VectorType(number_class), semantic);
            }
            if (clip_distances != 0) {
                out += fmt::format("out float gl_ClipDistance[{}];\n", clip_distances);
            }
        }
        for (const auto& sampler : stage.samplers) {
            const auto type = ViewType(sampler.image);
            out += fmt::format("uniform {} {};\n",
                               SamplerTypeName(type, ImageClass(sampler.image), sampler.shadow),
                               sampler.name);
        }
        const bool dual_source = info.sw_stage == SwStage::Fragment &&
                                 runtime_info.hw.fs.dual_source_blending;
        for (u32 i = 0; i < mrt_outputs.size(); ++i) {
            if (!mrt_outputs[i]) {
                continue;
            }
            const auto layout = dual_source ? fmt::format("location=0,index={}", i)
                                            : fmt::format("location={}", i);
            out += fmt::format("layout({}) out {} frag{};\n", layout, VectorType(mrt_class[i]), i);
        }
        for (const char* helper : helpers) {
            out += helper;
        }
        stage.declarations = std::move(out);
    }

    void BuildMain() {
        std::string out = "void main(){\n";
        std::map<std::string, std::string> locals;
        for (IR::Block* block : program.blocks) {
            for (const IR::Inst& inst : block->Instructions()) {
                const auto opcode = inst.GetOpcode();
                const IR::Type type = opcode == Opcode::Phi ? PhiType(inst) : inst.Type();
                if (type == IR::Type::Void || type == IR::Type::Opaque) {
                    continue;
                }
                const auto name = TypeName(type);
                auto& line = locals[name];
                line += line.empty() ? name + " " : ",";
                line += Name(&inst);
                if (opcode == Opcode::Phi) {
                    line += fmt::format(",phi{}", inst.Definition<u32>());
                }
            }
        }
        for (const auto& [type, line] : locals) {
            out += line;
            out += ";\n";
        }
        for (u32 i = 0; i < 32; ++i) {
            if (stage.output_mask & (1u << i)) {
                out += fmt::format("dcp_p{}=vec4(0.0);\n", i);
            }
        }
        for (u32 i = 0; i < mrt_outputs.size(); ++i) {
            if (mrt_outputs[i]) {
                out += fmt::format("frag{}={}(0);\n", i, VectorType(mrt_class[i]));
            }
        }
        out += body;
        out += "}\n";
        stage.main = std::move(out);
    }

    const Profile& profile;
    const RuntimeInfo& runtime_info;
    const IR::Program& program;
    const Info& info;
    std::string prefix;
    Stage stage;
    std::string body;
    std::vector<const char*> helpers; ///< in dependency order
    std::unordered_map<IR::Block*, std::vector<std::pair<IR::Inst*, IR::Value>>> phi_moves;
    bool uses_user_data = false;
    bool uses_buffers = false;
    bool uses_transform = false;
    bool uses_base_vertex = false;
    bool uses_base_instance = false;
    bool vs_inputs_ready = false;
    u32 clip_distances = 0;
    std::map<u32, AmdGpu::NumberClass> vs_attributes;
    std::array<bool, IR::NumRenderTargets> mrt_outputs{};
    std::array<AmdGpu::NumberClass, IR::NumRenderTargets> mrt_class{};
};

std::string Qualifiers(const Info::Interpolation& interpolation) {
    return fmt::format("{}{}", QualifierName(interpolation.primary),
                       QualifierName(interpolation.auxiliary));
}

const VaryingInput* FindInput(const Stage* fragment, u32 location) {
    if (!fragment) {
        return nullptr;
    }
    const auto it = std::ranges::find(fragment->inputs, location, &VaryingInput::location);
    return it == fragment->inputs.end() ? nullptr : &*it;
}

std::string GeometrySource(const Stage& vertex, const Stage* fragment,
                           PrimitiveExpansion expansion) {
    const bool rect = expansion == PrimitiveExpansion::RectList;
    std::string out = "#version 330 core\n";
    out += rect ? "layout(triangles) in;\n" : "layout(lines_adjacency) in;\n";
    out += "layout(triangle_strip,max_vertices=4) out;\n";
    std::string copy_vertex, copy_blend;
    if (vertex.clip_distances != 0) {
        out += fmt::format("out float gl_ClipDistance[{}];\n", vertex.clip_distances);
    }
    // GLSL 3.30 cannot resize gl_in's array: constant indices size it implicitly.
    for (u32 clip = 0; clip < vertex.clip_distances; ++clip) {
        copy_vertex += fmt::format("gl_ClipDistance[{0}]=gl_in[i].gl_ClipDistance[{0}];", clip);
        copy_blend += fmt::format("gl_ClipDistance[{0}]=b.x*gl_in[0].gl_ClipDistance[{0}]+"
                                  "b.y*gl_in[1].gl_ClipDistance[{0}]+"
                                  "b.z*gl_in[2].gl_ClipDistance[{0}];",
                                  clip);
    }
    for (u32 i = 0; i < 32; ++i) {
        if (!(vertex.output_mask & (1u << i))) {
            continue;
        }
        const auto* input = FindInput(fragment, i);
        const auto qualifiers = input ? Qualifiers(input->interpolation) : std::string{};
        out += fmt::format("{0}in vec4 dcp_p{1}[];\n{0}out vec4 dcp_g{1};\n", qualifiers, i);
        copy_vertex += fmt::format("dcp_g{0}=dcp_p{0}[i];", i);
        copy_blend += fmt::format("dcp_g{0}=b.x*dcp_p{0}[0]+b.y*dcp_p{0}[1]+b.z*dcp_p{0}[2];", i);
    }
    out += fmt::format("void put(int i){{gl_Position=gl_in[i].gl_Position;{}EmitVertex();}}\n",
                       copy_vertex);
    if (rect) {
        // The rectangle's corner is the vertex whose neighbours share x with one and y with the
        // other; the fourth vertex mirrors it across the opposite edge.
        out += fmt::format("void put_mirrored(vec3 b){{gl_Position=b.x*gl_in[0].gl_Position+"
                           "b.y*gl_in[1].gl_Position+b.z*gl_in[2].gl_Position;{}EmitVertex();}}\n",
                           copy_blend);
        out += "void main(){\n"
               "bvec2 eq[3];\n"
               "for(int i=0;i<3;++i){eq[i]=equal(gl_in[i].gl_Position.xy,"
               "gl_in[(i+1)%3].gl_Position.xy);}\n"
               "vec3 b;bool edge[3];\n"
               "for(int i=0;i<3;++i){\n"
               "edge[i]=(eq[i].x&&eq[(i+2)%3].y)||(eq[i].y&&eq[(i+2)%3].x);\n"
               "b[i]=edge[i]?-1.0:1.0;}\n"
               "int first=edge[1]?1:(edge[2]?2:0);\n"
               "put(first);put((first+1)%3);put((first+2)%3);put_mirrored(b);\n"
               "EndPrimitive();}\n";
    } else {
        out += "void main(){put(0);put(1);put(3);put(2);EndPrimitive();}\n";
    }
    return out;
}

} // namespace

Stage EmitGLSL(const Profile& profile, const RuntimeInfo& runtime_info,
               const IR::Program& program) {
    return Emitter{profile, runtime_info, program}.Run();
}

Sources LinkStages(const Stage& vertex, const Stage* fragment, PrimitiveExpansion expansion) {
    constexpr const char* Version = "#version 330 core\n";
    const bool geometry = expansion != PrimitiveExpansion::None;
    Sources sources;
    sources.vertex = Version;
    for (u32 i = 0; i < 32; ++i) {
        if (vertex.output_mask & (1u << i)) {
            const auto* input = FindInput(fragment, i);
            const auto qualifiers = input ? Qualifiers(input->interpolation) : std::string{};
            sources.vertex += fmt::format("{}out vec4 dcp_p{};\n", qualifiers, i);
        }
    }
    sources.vertex += vertex.declarations;
    sources.vertex += vertex.main;
    if (geometry) {
        sources.geometry = GeometrySource(vertex, fragment, expansion);
    }
    sources.fragment = Version;
    if (!fragment) {
        sources.fragment += "void main(){}\n";
        return sources;
    }
    for (const auto& input : fragment->inputs) {
        if (vertex.output_mask & (1u << input.location)) {
            if (geometry) {
                sources.fragment +=
                    fmt::format("#define dcp_p{0} dcp_g{0}\n", input.location);
            }
            sources.fragment += fmt::format("{}in vec4 dcp_p{};\n",
                                            Qualifiers(input.interpolation), input.location);
        } else {
            sources.fragment += fmt::format("const vec4 dcp_p{}=vec4(0.0);\n", input.location);
        }
    }
    sources.fragment += fragment->declarations;
    sources.fragment += fragment->main;
    return sources;
}

} // namespace Shader::Backend::GLSL
