// SPDX-License-Identifier: GPL-2.0-or-later
// DEBUG_SAULO: see vk_ultrawide_capture.h.
#include "video_core/renderer_vulkan/vk_ultrawide_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <png.h>

#include "dcp_ultrawide_capture.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

#include <vulkan/vulkan_format_traits.hpp>

namespace UltrawideCapture {
namespace {

namespace fs = std::filesystem;

enum class Phase : u32 { Idle, Armed, Recording, Writing };
enum CopyKind : int { Thumb, Shot, Direct };

constexpr u32 kArmFlips = 2; // the guest builds the next frame ahead: its commands are recorded before it is drawn
constexpr u64 kGuestReach = 0x20000;
constexpr u32 kMaxThumbs = 6144;
constexpr u32 kThumbsPerChunk = 256;
constexpr u64 kThumbSlotBytes = u64(kThumbBoxWidth) * kThumbBoxHeight * 4;
constexpr u32 kMaxShots = 64;
constexpr u32 kMaxPictures = 240;
constexpr double kLargeDraw = 0.15;
constexpr u32 kChangeThreshold = 6;
constexpr u32 kMaxTextures = 4;

std::atomic<Phase> phase{Phase::Idle};
u32 armed_flips = 0; // GPU thread

std::mutex config_mutex;
std::string config_logs, config_line;
bool config_guest_hooks = false;

struct GuestCommand {
    DcpCaptureExecuted executed{};
    DcpCaptureQueued queued{};
    bool has_queued = false;
};

std::mutex guest_mutex;
std::unordered_map<u64, DcpCaptureQueued> queued_commands;
std::map<u64, GuestCommand> executed_commands;
std::atomic<u64> guest_queued{0}, guest_executed{0};

struct TextureUse {
    VAddr address{};
    vk::Format format{};
    u32 width{}, height{};
    bool storage{};
};

struct DrawRow {
    u32 index{}, run{};
    VAddr packet{};
    std::optional<GuestCommand> guest;
    VAddr target{};
    vk::Format target_format{};
    Size target_size{};
    u32 mip{}, slice{}, attachments{};
    VAddr depth{};
    bool clears{}, proxy{}, indexed{}, indirect{};
    float viewport[4]{};
    s32 screen_scissor[4]{};
    u32 generic_scissor[4]{};
    u64 vs{}, ps{};
    u32 indices{}, instances{}, primitive{};
    AmdGpu::BlendControl blend{};
    u32 write_mask{};
    std::vector<TextureUse> textures;
    u32 texture_count{};
    s32 thumb = -1, before = -1;
    Size thumb_size{}, before_size{};
    // Filled by the writer.
    bool footprint_known{};
    Rect footprint{};
    double coverage{};
    bool left{}, right{};
    std::string picture, pass_picture;
};

struct ShotData {
    std::string name;
    u32 after_draw{};
    s32 pass = -1;
    Size size{};
    bool bgra{};
    std::unique_ptr<VideoCore::Buffer> buffer; // until TakePixels
    std::vector<u8> pixels;
};

/// Recorded on the GPU thread; after TakePixels it holds no GPU object and the writer thread owns it.
struct CaptureFrame {
    std::time_t started{};
    std::vector<DrawRow> draws;
    std::vector<std::unique_ptr<VideoCore::Buffer>> thumb_chunks; // until TakePixels
    std::vector<u8> thumb_pixels;
    u32 thumbs{};
    bool thumbs_full{}, shots_full{};
    std::vector<ShotData> shots;
    std::optional<ShotData> final;
    SkipCounts metadata_only, passes_skipped;
    u32 runs{};
    std::vector<TextureUse> pending_textures;
    u32 pending_texture_count{};
    std::optional<GuestCommand> pending_guest;
    VAddr pending_packet{};
    s32 pending_before = -1;
    Size pending_before_size{};
    bool has_previous{};
    TargetRef previous_image{};
    VAddr previous_target{};
    u32 previous_mip{}, previous_slice{};
    s32 previous_pass = -1;
    bool previous_proxy{};
    u32 previous_draw{};
};

// GPU thread, while recording; a plain pointer, so no Vulkan buffer is freed by a static destructor at exit.
CaptureFrame* frame = nullptr;

// Blit destinations, kept for the process for the same reason.
VideoCore::UniqueImage* scaled_images[2][2]{}; // [Thumb/Shot][unorm/srgb]

std::optional<GuestCommand> LookupGuest(VAddr packet) {
    if (!packet) {
        return {};
    }
    std::scoped_lock lk{guest_mutex};
    auto it = executed_commands.upper_bound(packet);
    if (it == executed_commands.begin()) {
        return {};
    }
    --it;
    if (packet - it->first > kGuestReach) {
        return {};
    }
    return it->second;
}

void ClearGuest() {
    std::scoped_lock lk{guest_mutex};
    queued_commands.clear();
    executed_commands.clear();
}

vk::ImageLayout TrackedLayout(const VideoCore::Image& image, u32 mip, u32 slice) {
    const auto& states = image.backing->subresource_states;
    const u32 index = mip * image.info.resources.layers + slice;
    return states.empty() || index >= states.size() ? image.backing->state.layout : states[index].layout;
}

Size SourceSize(const VideoCore::Image& image, u32 mip) {
    return {std::max(image.info.size.width >> mip, 1u), std::max(image.info.size.height >> mip, 1u)};
}

Size CopySize(const VideoCore::Image& image, u32 mip, int kind) {
    const Size source = SourceSize(image, mip);
    if (kind == Direct) {
        return source;
    }
    return kind == Thumb ? FitBox(source, kThumbBoxWidth, kThumbBoxHeight)
                         : FitBox(source, kShotBoxWidth, kShotBoxHeight);
}

bool IsBgra8(vk::Format format) {
    return format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
}

bool IsRgba8(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
}

vk::Image ScaledImage(const Vulkan::Instance& instance, int kind, bool srgb) {
    auto*& slot = scaled_images[kind == Thumb ? 0 : 1][srgb ? 1 : 0];
    if (!slot) {
        slot = new VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        const vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
            .extent = {kind == Thumb ? kThumbBoxWidth : kShotBoxWidth,
                       kind == Thumb ? kThumbBoxHeight : kShotBoxHeight, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        slot->Create(info);
    }
    return slot->image;
}

std::unique_ptr<VideoCore::Buffer> ReadbackBuffer(const Vulkan::Instance& instance, u64 bytes) {
    auto buffer = std::make_unique<VideoCore::Buffer>(instance, 0, bytes, VideoCore::MemoryType::HostCached,
                                                      "DEBUG_SAULO ultrawide capture");
    if (buffer->mapped_data.size() < bytes) {
        return nullptr;
    }
    return buffer;
}

void TakeShot(ShotData& shot) {
    const u64 bytes = u64(shot.size.width) * shot.size.height * 4;
    if (shot.buffer && shot.buffer->mapped_data.size() >= bytes) {
        shot.buffer->Invalidate(0, bytes);
        shot.pixels.assign(shot.buffer->mapped_data.begin(), shot.buffer->mapped_data.begin() + bytes);
    }
    shot.buffer.reset();
}

/// Priority thread, once the GPU has finished the frame: copies every readback into memory the frame owns and frees
/// the buffers, so the writer thread touches no GPU object.
void TakePixels(CaptureFrame& f) {
    f.thumb_pixels.resize(u64(f.thumbs) * kThumbSlotBytes);
    for (u32 chunk = 0; chunk < f.thumb_chunks.size(); ++chunk) {
        const u32 first = chunk * kThumbsPerChunk;
        if (first >= f.thumbs) {
            break;
        }
        const u64 bytes = u64(std::min(kThumbsPerChunk, f.thumbs - first)) * kThumbSlotBytes;
        auto& buffer = *f.thumb_chunks[chunk];
        buffer.Invalidate(0, bytes);
        std::memcpy(f.thumb_pixels.data() + u64(first) * kThumbSlotBytes, buffer.mapped_data.data(), bytes);
    }
    f.thumb_chunks.clear();
    for (auto& shot : f.shots) {
        TakeShot(shot);
    }
    if (f.final) {
        TakeShot(*f.final);
    }
}

const u8* ThumbData(const CaptureFrame& f, s32 slot) {
    return f.thumb_pixels.data() + u64(u32(slot)) * kThumbSlotBytes;
}

/// RGB without the target's alpha, which is not what was on screen.
bool WritePng(const fs::path& path, const u8* pixels, Size size, bool bgra) {
    std::vector<u8> rgb(u64(size.width) * size.height * 3);
    for (u64 i = 0; i < u64(size.width) * size.height; ++i) {
        rgb[i * 3 + 0] = pixels[i * 4 + (bgra ? 2 : 0)];
        rgb[i * 3 + 1] = pixels[i * 4 + 1];
        rgb[i * 3 + 2] = pixels[i * 4 + (bgra ? 0 : 2)];
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = size.width;
    image.height = size.height;
    image.format = PNG_FORMAT_RGB;
    return png_image_write_to_file(&image, path.string().c_str(), 0, rgb.data(), 0, nullptr) != 0;
}

std::string PassFileText(s32 pass) {
    std::string text = pass < 0 ? "none" : LayerText(pass);
    std::replace(text.begin(), text.end(), ':', '_');
    return text;
}

std::string Hex(u64 value) {
    char text[24];
    std::snprintf(text, sizeof(text), "%#llx", static_cast<unsigned long long>(value));
    return text;
}

std::string FormatText(vk::Format format) {
    std::string text = vk::to_string(format);
    return text.empty() ? "?" : text;
}

std::string RectText(const Rect& r) {
    if (r.Empty()) {
        return "-";
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%u %u %u %u", r.left, r.top, r.right, r.bottom);
    return text;
}

std::string BlendText(const AmdGpu::BlendControl& b) {
    if (!b.enable) {
        return "off";
    }
    char text[64];
    std::snprintf(text, sizeof(text), "src %u dst %u func %u; alpha %u %u %u%s", u32(b.color_src_factor),
                  u32(b.color_dst_factor), u32(b.color_func), u32(b.alpha_src_factor),
                  u32(b.alpha_dst_factor), u32(b.alpha_func), b.separate_alpha_blend ? " separate" : "");
    return text;
}

Size GuestCanvas(const GuestCommand& g) {
    return Canvas(g.executed.projection[0], g.executed.projection[1]);
}

std::string CanvasText(const GuestCommand& g) {
    const Size c = GuestCanvas(g);
    if (!c.width) {
        return "?";
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%ux%u%s", c.width, c.height, Widened(c) ? " widened" : "");
    return text;
}

void WriteCsv(const fs::path& path, const CaptureFrame& f) {
    FILE* out = std::fopen(path.string().c_str(), "w");
    if (!out) {
        return;
    }
    std::fprintf(out,
                 "draw,run,pass,pass_family,pass_set,priority,canvas,projection,view_scale,pass_viewport,command,"
                 "vtable,callers,quad_kind,quad_vertices,quad_bounds,quad_texture,quad_matrix,packet,target,"
                 "target_format,target_size,target_mip,target_slice,attachments,depth,clears,viewport,"
                 "screen_scissor,generic_scissor,vs,ps,indexed,indirect,indices,instances,primitive,blend,"
                 "write_mask,texture_count,textures,proxy,footprint,coverage,left_side,right_side,picture,"
                 "pass_picture\n");
    for (const auto& d : f.draws) {
        std::fprintf(out, "%u,%u,", d.index, d.run);
        if (d.guest) {
            const auto& e = d.guest->executed;
            const auto& q = d.guest->queued;
            std::fprintf(out, "%s,%s,%d,%u,%s,%g %g %g %g,%g,%g %g %g %g,%s,%s,", LayerText(e.layer).c_str(),
                         PassFamily(e.layer), e.set_index, e.priority, CanvasText(*d.guest).c_str(),
                         e.projection[0], e.projection[1], e.projection[2], e.projection[3], e.view_scale,
                         e.viewport[0], e.viewport[1], e.viewport[2], e.viewport[3], Hex(e.command).c_str(),
                         Hex(e.vtable).c_str());
            std::fprintf(out, "%s,", d.guest->has_queued ? CallersText(q.callers, 4).c_str() : "-");
            if (d.guest->has_queued && q.quad) {
                std::fprintf(out, "%d,%d,%g %g %g %g,%s,%d,", q.quad_kind, q.quad_vertices, q.quad_left,
                             q.quad_top, q.quad_right, q.quad_bottom, Hex(q.quad_texture).c_str(), q.quad_matrix);
            } else {
                std::fprintf(out, "-,-,-,-,-,");
            }
        } else {
            std::fprintf(out, "-,?,-,-,-,-,-,-,-,-,-,-,-,-,-,-,");
        }
        std::fprintf(out, "%s,%s,%s,%ux%u,%u,%u,%u,%s,%d,", AddressText(d.packet).c_str(),
                     AddressText(d.target).c_str(), d.target ? FormatText(d.target_format).c_str() : "-",
                     d.target_size.width, d.target_size.height, d.mip, d.slice, d.attachments,
                     AddressText(d.depth).c_str(), d.clears);
        std::fprintf(out, "%g %g %g %g,%d %d %d %d,%u %u %u %u,", d.viewport[0], d.viewport[1], d.viewport[2],
                     d.viewport[3], d.screen_scissor[0], d.screen_scissor[1], d.screen_scissor[2],
                     d.screen_scissor[3], d.generic_scissor[0], d.generic_scissor[1], d.generic_scissor[2],
                     d.generic_scissor[3]);
        std::fprintf(out, "%016llx,%016llx,%d,%d,%u,%u,%u,%s,%x,%u,", static_cast<unsigned long long>(d.vs),
                     static_cast<unsigned long long>(d.ps), d.indexed, d.indirect, d.indices, d.instances,
                     d.primitive, BlendText(d.blend).c_str(), d.write_mask, d.texture_count);
        std::string textures;
        for (const auto& t : d.textures) {
            textures += (textures.empty() ? "" : " | ") + Hex(t.address) + " " + FormatText(t.format) + " " +
                        std::to_string(t.width) + "x" + std::to_string(t.height) + (t.storage ? " storage" : "");
        }
        std::fprintf(out, "%s,%d,%s,%s,%d,%d,%s,%s\n", textures.empty() ? "-" : textures.c_str(), d.proxy,
                     d.footprint_known ? RectText(d.footprint).c_str() : "?",
                     d.footprint_known ? std::to_string(d.coverage).c_str() : "?", d.left, d.right,
                     d.picture.empty() ? "-" : d.picture.c_str(),
                     d.pass_picture.empty() ? "-" : d.pass_picture.c_str());
    }
    std::fclose(out);
}

struct RunSummary {
    u32 first{}, last{}, draws{};
    s32 pass = -1;
    VAddr target{};
    std::string canvas = "?";
    Rect footprint{};
    bool left{}, right{};
    std::string picture;
};

void WriteSummary(const fs::path& path, const CaptureFrame& f, const std::string& name, const std::string& line,
                  bool guest_hooks, u32 pictures) {
    FILE* out = std::fopen(path.string().c_str(), "w");
    if (!out) {
        return;
    }
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&f.started));
    const Size display = f.final ? f.final->size : Size{};
    const VAddr main_target = f.draws.empty() ? 0 : f.draws.back().target;
    Size main_size{};
    for (const auto& d : f.draws) {
        if (d.target == main_target) {
            main_size = d.target_size;
        }
    }
    const SideAreas sides = SidesOf(main_size);
    u32 matched = 0;
    for (const auto& d : f.draws) {
        matched += d.guest.has_value();
    }
    std::fprintf(out, "DEBUG_SAULO ultrawide capture %s, %s\n%s\n", name.c_str(), stamp, line.c_str());
    std::fprintf(out, "guest hooks: %s\n", guest_hooks ? "installed" : "not installed (the line above says why): no pass or command columns");
    std::fprintf(out, "final picture %ux%u (final.png); last target %s %ux%u\n", display.width, display.height,
                 Hex(main_target).c_str(), main_size.width, main_size.height);
    if (sides.left.Empty()) {
        std::fprintf(out, "side areas: none (the target is not wider than 16:9)\n");
    } else {
        std::fprintf(out, "side areas: x %u..%u and %u..%u (outside the centred 16:9 %u px)\n", sides.left.left,
                     sides.left.right, sides.right.left, sides.right.right, sides.right.left - sides.left.right);
    }
    std::fprintf(out, "draws %zu, matched to a guest command %u; pass pictures %zu; draw pictures %u%s%s\n",
                 f.draws.size(), matched, f.shots.size(), pictures,
                 f.thumbs_full ? "; footprints stop at the thumbnail limit" : "",
                 f.shots_full ? "; pass pictures stop at their limit" : "");
    std::fprintf(out, "metadata-only draws (no thumbnail): %u (%s); pass pictures skipped: %u (%s)\n",
                 f.metadata_only.Total(), SkipCountsText(f.metadata_only).c_str(), f.passes_skipped.Total(),
                 SkipCountsText(f.passes_skipped).c_str());
    std::fprintf(out, "guest commands seen while armed: queued %llu, executed %llu\n",
                 static_cast<unsigned long long>(guest_queued.load()),
                 static_cast<unsigned long long>(guest_executed.load()));

    std::vector<RunSummary> runs;
    for (const auto& d : f.draws) {
        const s32 pass = d.guest ? d.guest->executed.layer : -1;
        if (runs.empty() || runs.back().pass != pass || runs.back().target != d.target ||
            d.run != f.draws[runs.back().last].run) {
            RunSummary r;
            r.first = d.index;
            r.pass = pass;
            r.target = d.target;
            r.canvas = d.guest ? CanvasText(*d.guest) : "?";
            runs.push_back(r);
        }
        auto& r = runs.back();
        r.last = d.index;
        ++r.draws;
        if (d.footprint_known) {
            r.footprint = Union(r.footprint, d.footprint);
            r.left |= d.left;
            r.right |= d.right;
        }
        if (!d.pass_picture.empty()) {
            r.picture = d.pass_picture;
        }
    }
    std::fprintf(out, "\nRuns (consecutive draws of one guest pass into one target), in draw order:\n");
    std::fprintf(out, "  %-8s %-7s %-16s %-12s %-6s %-11s %-22s %-5s %s\n", "pass", "family", "canvas", "target",
                 "draws", "first-last", "footprint", "sides", "picture after the run");
    for (const auto& r : runs) {
        char range[24];
        std::snprintf(range, sizeof(range), "%u-%u", r.first, r.last);
        std::string side = r.left ? "L" : "";
        side += r.right ? "R" : "";
        std::fprintf(out, "  %-8s %-7s %-16s %-12s %-6u %-11s %-22s %-5s %s\n", LayerText(r.pass).c_str(),
                     PassFamily(r.pass), r.canvas.c_str(), Hex(r.target).c_str(), r.draws, range,
                     RectText(r.footprint).c_str(), side.empty() ? "-" : side.c_str(),
                     r.picture.empty() ? "-" : r.picture.c_str());
    }

    const auto list = [&](const char* title, auto&& wanted) {
        std::fprintf(out, "\n%s\n", title);
        u32 count = 0;
        for (const auto& d : f.draws) {
            if (!wanted(d)) {
                continue;
            }
            ++count;
            const std::string callers =
                d.guest && d.guest->has_queued ? CallersText(d.guest->queued.callers, 4) : "-";
            std::fprintf(out, "  draw %-5u pass %-8s %-7s footprint %-22s cover %5.1f%% %s%s textures %u callers %s "
                              "vtable %s%s%s\n",
                         d.index, d.guest ? LayerText(d.guest->executed.layer).c_str() : "-",
                         d.guest ? PassFamily(d.guest->executed.layer) : "?", RectText(d.footprint).c_str(),
                         d.coverage * 100.0, d.left ? "L" : "", d.right ? "R" : "", d.texture_count,
                         callers.c_str(), d.guest ? Hex(d.guest->executed.vtable).c_str() : "-",
                         d.picture.empty() ? "" : " picture ", d.picture.c_str());
        }
        if (!count) {
            std::fprintf(out, "  none\n");
        }
    };
    list("Draws that change the side areas:", [](const DrawRow& d) { return d.left || d.right; });
    list("Draws that change at least 15% of their target:",
         [](const DrawRow& d) { return d.footprint_known && d.coverage >= kLargeDraw; });
    std::fprintf(out, "\nEvery draw: draws.csv. Footprints compare %ux%u-box thumbnails before and after each draw, "
                      "so they are coarse and miss thin or faint changes.\n",
                 kThumbBoxWidth, kThumbBoxHeight);
    std::fclose(out);
}

void Write(CaptureFrame& f) {
    std::string logs, line;
    bool guest_hooks;
    {
        std::scoped_lock lk{config_mutex};
        logs = config_logs;
        line = config_line;
        guest_hooks = config_guest_hooks;
    }
    std::error_code ec;
    const fs::path root = fs::path(logs.empty() ? "." : logs) / "ultrawide-capture";
    fs::create_directories(root, ec);
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        names.push_back(entry.path().filename().string());
    }
    const std::string name = CaptureDirName(NextCaptureIndex(names));
    const fs::path dir = root / name;
    fs::create_directories(dir / "passes", ec);
    fs::create_directories(dir / "draws", ec);

    std::map<std::pair<VAddr, u32>, std::pair<s32, Size>> last_thumb;
    for (auto& d : f.draws) {
        if (d.thumb < 0) {
            continue;
        }
        const auto key = std::make_pair(d.target, d.mip << 16 | d.slice);
        s32 before = d.before;
        Size before_size = d.before_size;
        if (before < 0) {
            if (const auto it = last_thumb.find(key); it != last_thumb.end()) {
                before = it->second.first;
                before_size = it->second.second;
            }
        }
        if (before >= 0 && before_size == d.thumb_size) {
            u64 cells = 0;
            const Rect changed =
                ChangedCells(ThumbData(f, before), ThumbData(f, d.thumb), d.thumb_size, kChangeThreshold, &cells);
            d.footprint = CellsToPixels(changed, d.thumb_size, d.target_size);
            d.coverage = double(cells) / (u64(d.thumb_size.width) * d.thumb_size.height);
            const SideAreas sides = SidesOf(d.target_size);
            d.left = Overlaps(d.footprint, sides.left);
            d.right = Overlaps(d.footprint, sides.right);
            d.footprint_known = true;
        }
        last_thumb[key] = {d.thumb, d.thumb_size};
    }

    std::vector<Footprint> feet;
    for (const auto& d : f.draws) {
        feet.push_back({d.left || d.right, d.footprint_known ? d.coverage : 0.0});
    }
    u32 pictures = 0;
    for (const size_t i : PickPictures(feet, kLargeDraw, kMaxPictures)) {
        auto& d = f.draws[i];
        if (d.thumb < 0) {
            continue;
        }
        char file[64];
        std::snprintf(file, sizeof(file), "draws/draw_%04u_pass_%s.png", d.index,
                      PassFileText(d.guest ? d.guest->executed.layer : -1).c_str());
        const std::string relative = file;
        if (WritePng(dir / relative, ThumbData(f, d.thumb), d.thumb_size, false)) {
            d.picture = relative;
            ++pictures;
        }
    }
    const auto has_pixels = [](const ShotData& shot) {
        return shot.pixels.size() >= u64(shot.size.width) * shot.size.height * 4;
    };
    for (auto& shot : f.shots) {
        if (has_pixels(shot) && WritePng(dir / shot.name, shot.pixels.data(), shot.size, shot.bgra) &&
            shot.after_draw < f.draws.size()) {
            f.draws[shot.after_draw].pass_picture = shot.name;
        }
    }
    if (f.final && has_pixels(*f.final)) {
        WritePng(dir / "final.png", f.final->pixels.data(), f.final->size, f.final->bgra);
    }
    WriteCsv(dir / "draws.csv", f);
    WriteSummary(dir / "summary.txt", f, name, line, guest_hooks, pictures);
    std::printf("DEBUG_SAULO ultrawide capture complete: %s (%zu draws, %zu pass pictures, %u draw pictures)\n",
                dir.string().c_str(), f.draws.size(), f.shots.size(), pictures);
}

} // namespace

void Configure(const char* logs_dir, const char* ultrawide_line, bool guest_hooks) {
    std::scoped_lock lk{config_mutex};
    config_logs = logs_dir ? logs_dir : "";
    config_line = ultrawide_line ? ultrawide_line : "";
    config_guest_hooks = guest_hooks;
    std::printf("DEBUG_SAULO ultrawide capture: F9 records one frame into %s (guest hooks %s)\n",
                (fs::path(config_logs.empty() ? "." : config_logs) / "ultrawide-capture").string().c_str(),
                guest_hooks ? "on" : "off");
}

void Press(bool repeat) {
    static const bool pad_recording = std::getenv("DCP_PAD_RECORD") != nullptr;
    if (repeat || pad_recording) {
        return;
    }
    Phase expected = Phase::Idle;
    if (!phase.compare_exchange_strong(expected, Phase::Armed)) {
        std::printf("DEBUG_SAULO ultrawide capture busy, F9 ignored\n");
        return;
    }
    ClearGuest();
    guest_queued = 0;
    guest_executed = 0;
    active.store(true, std::memory_order_release);
    std::printf("DEBUG_SAULO ultrawide capture armed\n");
}

bool GuestWanted() {
    const Phase now = phase.load(std::memory_order_relaxed);
    return now == Phase::Armed || now == Phase::Recording;
}

void Queued(const DcpCaptureQueued& queued) {
    guest_queued.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lk{guest_mutex};
    queued_commands[queued.command] = queued;
}

void Executed(const DcpCaptureExecuted& executed) {
    guest_executed.fetch_add(1, std::memory_order_relaxed);
    GuestCommand command;
    command.executed = executed;
    std::scoped_lock lk{guest_mutex};
    if (const auto it = queued_commands.find(executed.command); it != queued_commands.end()) {
        command.queued = it->second;
        command.has_queued = true;
    }
    executed_commands[executed.command_buffer] = command;
}

} // namespace UltrawideCapture

namespace Vulkan {

namespace UC = UltrawideCapture;

/// Records a copy of one color subresource, scaled into the thumbnail or shot box (or whole, `Direct`), to `buffer`
/// at `offset`. The image returns to the layout the runtime tracks for it.
bool Rasterizer::CaptureCopy(VideoCore::Image& image, u32 mip, u32 slice, int kind, vk::Buffer buffer, u64 offset) {
    if (!image.backing) {
        return false;
    }
    const auto& info = image.info;
    const vk::ImageLayout layout = UC::TrackedLayout(image, mip, slice);
    const vk::Format format = image.backing->image.image_ci.format;
    const char* numeric = vk::componentNumericFormat(format, 0);
    const auto features = instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    const UC::SnapshotSource source_info{
        .backing = true,
        .depth = bool(info.props.is_depth),
        .multisample = info.num_samples > 1,
        .integer = !std::strcmp(numeric, "UINT") || !std::strcmp(numeric, "SINT"),
        .mip = mip,
        .levels = info.resources.levels,
        .slice = slice,
        .layers = info.resources.layers,
        .layout_known = layout != vk::ImageLayout::eUndefined && layout != vk::ImageLayout::ePreinitialized,
        .blit_source = bool(features & vk::FormatFeatureFlagBits::eBlitSrc),
        .rgba8 = UC::IsRgba8(format) || UC::IsBgra8(format),
    };
    if (!UC::SnapshotPossible(source_info, kind == UC::Direct)) {
        return false;
    }
    const UC::Size source = UC::SourceSize(image, mip);
    const UC::Size size = UC::CopySize(image, mip, kind);
    const vk::Image scaled =
        kind == UC::Direct ? vk::Image{} : UC::ScaledImage(instance, kind, !std::strcmp(numeric, "SRGB"));
    const vk::Filter filter = features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear
                                  ? vk::Filter::eLinear
                                  : vk::Filter::eNearest;
    runtime.FlushBarriers();
    scheduler.EndRendering();
    scheduler.Record([source_image = image.GetImage(), layout, mip, slice, source, size, scaled, filter, buffer,
                      offset](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange source_range{vk::ImageAspectFlagBits::eColor, mip, 1, slice, 1};
        const vk::ImageSubresourceRange scaled_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 source_to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = layout,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange = source_range,
        };
        const vk::ImageMemoryBarrier2 source_back{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange = source_range,
        };
        const vk::BufferMemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = offset,
            .size = u64(size.width) * size.height * 4,
        };
        if (!scaled) {
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &source_to_copy,
            });
            cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                     vk::BufferImageCopy{
                                         .bufferOffset = offset,
                                         .imageSubresource{vk::ImageAspectFlagBits::eColor, mip, slice, 1},
                                         .imageExtent{size.width, size.height, 1},
                                     });
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &to_host,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &source_back,
            });
            return;
        }
        const std::array<vk::ImageMemoryBarrier2, 2> to_blit{
            source_to_copy,
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = scaled,
                .subresourceRange = scaled_range,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = u32(to_blit.size()),
            .pImageMemoryBarriers = to_blit.data(),
        });
        const vk::ImageBlit region{
            .srcSubresource{vk::ImageAspectFlagBits::eColor, mip, slice, 1},
            .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(source.width), s32(source.height), 1}},
            .dstSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(size.width), s32(size.height), 1}},
        };
        cmdbuf.blitImage(source_image, vk::ImageLayout::eTransferSrcOptimal, scaled,
                         vk::ImageLayout::eTransferDstOptimal, region, filter);
        const std::array<vk::ImageMemoryBarrier2, 2> after_blit{
            source_back,
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = scaled,
                .subresourceRange = scaled_range,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = u32(after_blit.size()),
            .pImageMemoryBarriers = after_blit.data(),
        });
        cmdbuf.copyImageToBuffer(scaled, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                 vk::BufferImageCopy{
                                     .bufferOffset = offset,
                                     .imageSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .imageExtent{size.width, size.height, 1},
                                 });
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &to_host,
        });
    });
    return true;
}

namespace {

/// A thumbnail slot of the frame; -1 when the limit is reached or no memory is left.
s32 NextThumbSlot(const Instance& instance, UC::CaptureFrame& f, vk::Buffer& buffer, u64& offset) {
    if (f.thumbs >= UC::kMaxThumbs) {
        f.thumbs_full = true;
        return -1;
    }
    const u32 chunk = f.thumbs / UC::kThumbsPerChunk;
    if (chunk >= f.thumb_chunks.size()) {
        auto memory = UC::ReadbackBuffer(instance, UC::kThumbSlotBytes * UC::kThumbsPerChunk);
        if (!memory) {
            f.thumbs_full = true;
            return -1;
        }
        f.thumb_chunks.push_back(std::move(memory));
    }
    buffer = f.thumb_chunks[chunk]->Handle();
    offset = u64(f.thumbs % UC::kThumbsPerChunk) * UC::kThumbSlotBytes;
    return s32(f.thumbs);
}

} // namespace

/// The draw's colour target 0, or none (`ref` invalid) when the pipeline does not write it or no image is bound.
VideoCore::Image* Rasterizer::CaptureTarget(const GraphicsPipeline* pipeline, UC::TargetRef& ref) {
    const VideoCore::ImageId image_id = cb_descs[0].first;
    const u32 mrt_mask = pipeline->GetGraphicsKey().mrt_mask;
    ref = {};
    if (!(mrt_mask & 1) || !image_id) {
        return nullptr;
    }
    auto& image = texture_cache.GetImage(image_id);
    ref = UC::ColourTarget(mrt_mask, image_id.index, image.image_uid);
    return ref.Valid() ? &image : nullptr;
}

/// The previous draw's target as that draw left it: the end of a run of one guest pass into one target.
void Rasterizer::CapturePassPicture() {
    auto& f = *UC::frame;
    VideoCore::Image* previous = nullptr;
    const UC::Skip skip = UC::PictureSkip(f.previous_image, f.previous_proxy, f.shots.size(), UC::kMaxShots,
                                          [&](u32 slot, u64 uid) {
                                              previous = texture_cache.TryGetImage(VideoCore::ImageId{slot}, uid);
                                              return previous != nullptr;
                                          });
    if (skip != UC::Skip::None) {
        f.passes_skipped.Count(skip);
        f.shots_full |= skip == UC::Skip::Limit;
        return;
    }
    const UC::Size size = UC::CopySize(*previous, f.previous_mip, UC::Shot);
    auto memory = UC::ReadbackBuffer(instance, u64(size.width) * size.height * 4);
    if (!memory || !CaptureCopy(*previous, f.previous_mip, f.previous_slice, UC::Shot, memory->Handle(), 0)) {
        f.passes_skipped.Count(UC::Skip::Refused);
        return;
    }
    char name[80];
    std::snprintf(name, sizeof(name), "passes/pass_%02zu_after_draw_%04u_pass_%s.png", f.shots.size(),
                  f.previous_draw, UC::PassFileText(f.previous_pass).c_str());
    f.shots.push_back({name, f.previous_draw, f.previous_pass, size, false, std::move(memory)});
}

void Rasterizer::CaptureBeforeDraw(const GraphicsPipeline* pipeline) {
    if (!UC::frame) {
        return;
    }
    auto& f = *UC::frame;
    f.pending_packet = capture_packet;
    f.pending_guest = UC::LookupGuest(capture_packet);
    f.pending_before = -1;
    const s32 pass = f.pending_guest ? f.pending_guest->executed.layer : -1;
    UC::TargetRef ref;
    VideoCore::Image* image = CaptureTarget(pipeline, ref);
    const auto& desc = cb_descs[0].second;
    const VAddr target = image ? image->info.guest_address : 0;
    const u32 mip = image ? desc.view_info.range.base.level : 0;
    const u32 slice = image ? desc.view_info.range.base.layer : 0;
    const bool proxy = push_data.scene_size != 0;
    const bool new_target = !f.has_previous || target != f.previous_target || mip != f.previous_mip ||
                            slice != f.previous_slice;
    if (f.has_previous && (new_target || pass != f.previous_pass)) {
        ++f.runs;
        CapturePassPicture();
    }
    if (new_target && image && !proxy) {
        vk::Buffer buffer;
        u64 offset = 0;
        const s32 slot = NextThumbSlot(instance, f, buffer, offset);
        if (slot >= 0 && CaptureCopy(*image, mip, slice, UC::Thumb, buffer, offset)) {
            ++f.thumbs;
            f.pending_before = slot;
            f.pending_before_size = UC::CopySize(*image, mip, UC::Thumb);
        }
    }
}

void Rasterizer::CaptureAfterDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                                  const AmdGpu::Regs& regs, u32 num_indices, u32 num_instances, bool indexed,
                                  bool indirect) {
    if (!UC::frame) {
        return;
    }
    auto& f = *UC::frame;
    UC::DrawRow d;
    d.index = u32(f.draws.size());
    d.run = f.runs;
    d.packet = f.pending_packet;
    d.guest = std::move(f.pending_guest);
    f.pending_guest.reset();
    UC::TargetRef ref;
    VideoCore::Image* image = CaptureTarget(pipeline, ref);
    const auto& desc = cb_descs[0].second;
    if (image) {
        d.target = image->info.guest_address;
        d.target_format = image->info.pixel_format;
        d.mip = desc.view_info.range.base.level;
        d.slice = desc.view_info.range.base.layer;
        d.target_size = UC::SourceSize(*image, d.mip);
    }
    d.attachments = state.num_color_attachments;
    d.depth = regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthAddress() : 0;
    for (u32 i = 0; i < state.num_color_attachments; ++i) {
        d.clears |= state.color_attachments[i].is_clear != 0;
    }
    d.proxy = push_data.scene_size != 0;
    d.indexed = indexed;
    d.indirect = indirect;
    const auto& vp = regs.viewports[0];
    d.viewport[0] = vp.xoffset - std::fabs(vp.xscale);
    d.viewport[1] = vp.yoffset - std::fabs(vp.yscale);
    d.viewport[2] = 2.0f * std::fabs(vp.xscale);
    d.viewport[3] = 2.0f * std::fabs(vp.yscale);
    d.screen_scissor[0] = regs.screen_scissor.top_left_x;
    d.screen_scissor[1] = regs.screen_scissor.top_left_y;
    d.screen_scissor[2] = regs.screen_scissor.bottom_right_x;
    d.screen_scissor[3] = regs.screen_scissor.bottom_right_y;
    d.generic_scissor[0] = regs.generic_scissor.top_left_x;
    d.generic_scissor[1] = regs.generic_scissor.top_left_y;
    d.generic_scissor[2] = regs.generic_scissor.bottom_right_x;
    d.generic_scissor[3] = regs.generic_scissor.bottom_right_y;
    const auto stages = pipeline->GetStages();
    const auto* vs = stages[u32(Shader::SwStage::Vertex)];
    const auto* ps = stages[u32(Shader::SwStage::Fragment)];
    d.vs = vs ? vs->pgm_hash : 0;
    d.ps = ps ? ps->pgm_hash : 0;
    d.indices = num_indices;
    d.instances = num_instances;
    d.primitive = u32(regs.primitive_type);
    d.blend = regs.blend_control[0];
    d.write_mask = regs.color_target_mask.GetMask(0);
    d.textures = std::move(f.pending_textures);
    d.texture_count = f.pending_texture_count;
    f.pending_textures.clear();
    f.pending_texture_count = 0;
    d.before = f.pending_before;
    d.before_size = f.pending_before_size;
    f.pending_before = -1;
    UC::Skip skip = UC::PictureSkip(ref, d.proxy, f.thumbs, UC::kMaxThumbs, [](u32, u64) { return true; });
    if (skip == UC::Skip::None) {
        vk::Buffer buffer;
        u64 offset = 0;
        const s32 slot = NextThumbSlot(instance, f, buffer, offset);
        if (slot < 0) {
            skip = UC::Skip::Limit;
        } else if (!CaptureCopy(*image, d.mip, d.slice, UC::Thumb, buffer, offset)) {
            skip = UC::Skip::Refused;
        } else {
            ++f.thumbs;
            d.thumb = slot;
            d.thumb_size = UC::CopySize(*image, d.mip, UC::Thumb);
        }
    }
    f.metadata_only.Count(skip);
    f.has_previous = true;
    f.previous_image = ref;
    f.previous_target = d.target;
    f.previous_mip = d.mip;
    f.previous_slice = d.slice;
    f.previous_pass = d.guest ? d.guest->executed.layer : -1;
    f.previous_proxy = d.proxy;
    f.previous_draw = d.index;
    f.draws.push_back(std::move(d));
}

void Rasterizer::CaptureSampled(const VideoCore::Image& image, bool storage) {
    if (!UC::frame) {
        return;
    }
    auto& f = *UC::frame;
    ++f.pending_texture_count;
    if (f.pending_textures.size() < UC::kMaxTextures) {
        f.pending_textures.push_back({image.info.guest_address, image.info.pixel_format, image.info.size.width,
                                      image.info.size.height, storage});
    }
}

void Rasterizer::CaptureFlip(VideoCore::Image& display) {
    const UC::Phase now = UC::phase.load(std::memory_order_acquire);
    if (now == UC::Phase::Armed) {
        if (++UC::armed_flips < UC::kArmFlips) {
            return;
        }
        UC::armed_flips = 0;
        UC::frame = new UC::CaptureFrame();
        UC::frame->started = std::time(nullptr);
        UC::phase.store(UC::Phase::Recording, std::memory_order_release);
        std::printf("DEBUG_SAULO ultrawide capture frame begin\n");
        return;
    }
    if (now != UC::Phase::Recording || !UC::frame) {
        return;
    }
    auto& f = *UC::frame;
    if (f.has_previous) {
        CapturePassPicture();
    }
    for (const int kind : {int(UC::Direct), int(UC::Shot)}) {
        const UC::Size size = UC::CopySize(display, 0, kind);
        auto memory = UC::ReadbackBuffer(instance, u64(size.width) * size.height * 4);
        if (memory && CaptureCopy(display, 0, 0, kind, memory->Handle(), 0)) {
            const bool bgra = kind == UC::Direct && UC::IsBgra8(display.backing->image.image_ci.format);
            f.final = UC::ShotData{"final.png", u32(f.draws.size()), -1, size, bgra, std::move(memory)};
            break;
        }
    }
    UC::phase.store(UC::Phase::Writing, std::memory_order_release);
    UC::active.store(false, std::memory_order_release);
    UC::ClearGuest();
    std::shared_ptr<UC::CaptureFrame> job{std::exchange(UC::frame, nullptr)};
    scheduler.DeferPriorityOperation([job] {
        UC::TakePixels(*job);
        std::thread([job] {
            UC::Write(*job);
            UC::phase.store(UC::Phase::Idle, std::memory_order_release);
        }).detach();
    });
}

} // namespace Vulkan

extern "C" void dcpgpu_debug_saulo_capture_configure(const char* logs_dir, const char* ultrawide_line,
                                                     int guest_hooks) {
    UltrawideCapture::Configure(logs_dir, ultrawide_line, guest_hooks != 0);
}

extern "C" int dcpgpu_debug_saulo_capture_wanted(void) {
    return UltrawideCapture::GuestWanted() ? 1 : 0;
}

extern "C" void dcpgpu_debug_saulo_capture_queued(const DcpCaptureQueued* queued) {
    UltrawideCapture::Queued(*queued);
}

extern "C" void dcpgpu_debug_saulo_capture_executed(const DcpCaptureExecuted* executed) {
    UltrawideCapture::Executed(*executed);
}
