/* libSceFont / libSceFontFt on FreeType, adapted from gr2_pc's runtime_gr2_font.c.
 * Units: scales are pixels per em (FreeType char size at 72 dpi); metrics are float pixels. */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#define FONT_OK 0
#define ERR_FATAL ((int32_t)0x80460001)
#define ERR_INVALID_PARAMETER ((int32_t)0x80460002)
#define ERR_INVALID_MEMORY ((int32_t)0x80460003)
#define ERR_INVALID_LIBRARY ((int32_t)0x80460004)
#define ERR_INVALID_FONT_HANDLE ((int32_t)0x80460005)
#define ERR_INVALID_RENDERER ((int32_t)0x80460007)
#define ERR_ALLOCATION_FAILED ((int32_t)0x80460010)
#define ERR_NO_SUPPORT_LIBRARY ((int32_t)0x80460018)
#define ERR_FONT_OPEN_FAILED ((int32_t)0x80460036)
#define ERR_NO_SUPPORT_CODE ((int32_t)0x80460041)
#define ERR_NO_SUPPORT_GLYPH ((int32_t)0x80460042)
#define ERR_NO_SUPPORT_SURFACE ((int32_t)0x80460050)
#define ERR_ALREADY_BOUND_RENDERER ((int32_t)0x80460060)
#define ERR_NOT_BOUND_RENDERER ((int32_t)0x80460061)

#define MEM_MAGIC 0x0F00
#define LIBRARY_MAGIC 0x0F01
#define FONT_MAGIC 0x0F02
#define RENDERER_MAGIC 0x0F07

/* ---- Guest structures ---- */
/* SceFontMemory (sceFontMemoryInit fills it; the game allocates 64 bytes). */
typedef struct {
    uint16_t kind, attributes;
    uint32_t region_size;
    void *region, *mspace;
    const void *interface;
    void *destroy_callback, *destroy_object;
    void *reserved, *parent;
} FontMemory;
typedef struct { float width, height, h_bearing_x, h_bearing_y, h_advance, v_bearing_x, v_bearing_y, v_advance; } GlyphMetrics;
typedef struct { float baseline_offset, line_advance, decoration_extent; } HorizontalLayout;
typedef struct { float baseline_offset_x, column_advance, decoration_span; } VerticalLayout;
typedef struct { float offset_x, offset_y, position_x, position_y; } Kerning;
typedef struct {
    void *buffer;
    int32_t width_byte;
    int8_t pixel_size_byte;
    uint8_t pad0, style_flag, pad2;
    int32_t width, height;
    uint32_t scissor_x0, scissor_y0, scissor_x1, scissor_y1;
    uint64_t reserved[11];
} RenderSurface;
typedef struct {
    const void *stage;
    struct { uint8_t *address; uint32_t width_byte; uint8_t pixel_size_byte, pixel_format; uint16_t pad; } image;
    struct { uint32_t x, y, w, h; } update;
    struct { float bearing_x, bearing_y, advance, stride; uint32_t width, height; } metrics;
} RenderOutput;
typedef struct { uint16_t tag, pad; uint32_t flags, subfont_index; int32_t unique_id; const void *reserved[2]; } OpenParams;
/* What sceFontSelectLibraryFt / sceFontSelectRendererFt return: opaque driver tables the
 * game hands back to the Create functions unchanged. */
typedef struct { uint32_t magic, reserved; void *reserved_ptr; uintptr_t functions[4]; } LibrarySelection;
typedef struct { uint32_t magic, size; uintptr_t functions[3]; } RendererSelection;
_Static_assert(sizeof(FontMemory)==64,"SceFontMemory");
_Static_assert(sizeof(GlyphMetrics)==32,"SceFontGlyphMetrics");
_Static_assert(sizeof(HorizontalLayout)==12,"SceFontHorizontalLayout");
_Static_assert(sizeof(VerticalLayout)==12,"SceFontVerticalLayout");
_Static_assert(sizeof(Kerning)==16,"SceFontKerning");
_Static_assert(sizeof(RenderSurface)==128,"SceFontRenderSurface");
_Static_assert(sizeof(RenderOutput)==64,"SceFontRenderOutput");
_Static_assert(sizeof(OpenParams)==32,"SceFontOpenDetail");

/* ---- Host objects behind the guest handles ---- */
typedef struct { uint16_t magic; const FontMemory *memory; uint64_t edition; int fonts; } Library;
typedef struct { uint16_t magic; const FontMemory *memory; uint64_t edition; } Renderer;
typedef struct {
    uint16_t magic;
    Library *library;
    Renderer *renderer;
    FT_Face face;
    float scale_w, scale_h;          /* sceFontSetScalePixel: metrics and layout */
    float render_w, render_h;        /* sceFontSetupRenderScalePixel: rendering */
    FT_F26Dot6 size_w, size_h;       /* char size currently set on the face */
    int game_font;                   /* opened on the game's own data, not a system font set */
    uint8_t guest_reserved[256];     /* the PS4 handle is a 256-byte object */
} Font;

/* Noto Sans and Noto Sans TC (third_party/fonts, SIL OFL 1.1) stand in for the firmware's system font sets.
 * The bytes are static, so faces opened on them never outlive their data. */
#ifdef _WIN32
#define DCP_EMBED(symbol, path) \
    __asm__(".section .rdata,\"dr\"\n.balign 16\n.global " #symbol "\n" #symbol ":\n.incbin \"" path "\"\n" \
            ".global " #symbol "_end\n" #symbol "_end:\n.text\n")
#else
#define DCP_EMBED(symbol, path) \
    __asm__(".section .rodata\n.balign 16\n.hidden " #symbol "\n.global " #symbol "\n" #symbol ":\n.incbin \"" path "\"\n" \
            ".hidden " #symbol "_end\n.global " #symbol "_end\n" #symbol "_end:\n.previous\n")
#endif
DCP_EMBED(dcp_noto_sans, DCP_NOTO_SANS_PATH);
DCP_EMBED(dcp_noto_sans_tc, DCP_NOTO_SANS_TC_PATH);
extern const unsigned char dcp_noto_sans[], dcp_noto_sans_end[];
extern const unsigned char dcp_noto_sans_tc[], dcp_noto_sans_tc_end[];

/* The game's own font covers Latin and Japanese; the set only supplies the glyphs it lacks. */
#define FONTSET_SST_STD_EUROPEAN_JPCJK 0x1807A444u
#define FONTSET_SST_STD_SCHINESE_GBCJK 0x180CAC44u
static const struct { uint32_t set; const char *name; const unsigned char *data, *end; } builtin_sets[]={
    {FONTSET_SST_STD_EUROPEAN_JPCJK,"Noto Sans",dcp_noto_sans,dcp_noto_sans_end},
    {FONTSET_SST_STD_SCHINESE_GBCJK,"Noto Sans TC",dcp_noto_sans_tc,dcp_noto_sans_tc_end},
};

static const LibrarySelection library_ft={0x53464C46u /* "FLFS" */,0,NULL,{0,0,0,0}};
static const RendererSelection renderer_ft={0x53524646u /* "FFRS" */,sizeof(RendererSelection),{0,0,0}};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static FT_Library freetype; /* created with the first library, kept for the process lifetime */
static const RuntimeComposedGlyph *composed_glyphs;
static size_t composed_count;
static int composed_cell;

void runtime_font_set_composed_glyphs(const RuntimeComposedGlyph *glyphs, size_t count, int cell) {
    pthread_mutex_lock(&lock);
    composed_glyphs=cell>0 ? glyphs : NULL; composed_count=composed_glyphs ? count : 0; composed_cell=cell;
    pthread_mutex_unlock(&lock);
}

static const RuntimeComposedGlyph *composed_glyph(uint32_t code) {
    for (size_t i=0;i<composed_count;++i) if (composed_glyphs[i].code==code) return &composed_glyphs[i];
    return NULL;
}

static Font *font_of(void *handle) {
    Font *font=handle;
    return font && font->magic==FONT_MAGIC && font->face ? font : NULL;
}

/* Char size in 26.6 at 72 dpi, so one em is `w` x `h` pixels. Called with the lock held. */
static int set_size(Font *font, float w, float h) {
    if (!(w>0.0f) || !(h>0.0f) || w>4096.0f || h>4096.0f) return -1;
    FT_F26Dot6 cw=(FT_F26Dot6)lrintf(w*64.0f), ch=(FT_F26Dot6)lrintf(h*64.0f);
    if (cw==font->size_w && ch==font->size_h) return 0;
    if (FT_Set_Char_Size(font->face,cw,ch,72,72)) { font->size_w=font->size_h=0; return -1; }
    font->size_w=cw; font->size_h=ch;
    return 0;
}

/* Bounding-box top and bottom (head.yMax / yMin) in 26.6 at the size currently set on the face. */
static void line_extent(const Font *font, FT_Long *top, FT_Long *bottom) {
    FT_Fixed y_scale=font->face->size->metrics.y_scale;
    *top=FT_MulFix(font->face->bbox.yMax,y_scale); *bottom=FT_MulFix(font->face->bbox.yMin,y_scale);
    if (!FT_IS_SCALABLE(font->face)) {
        *top=font->face->size->metrics.ascender; *bottom=font->face->size->metrics.descender;
    }
}

static int load_glyph(Font *font, uint32_t code) {
    FT_UInt index=FT_Get_Char_Index(font->face,code);
    return index && !FT_Load_Glyph(font->face,index,FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP) ? 0 : -1;
}

static void fill_metrics(const FT_Glyph_Metrics *m, GlyphMetrics *out) {
    out->width=m->width/64.0f; out->height=m->height/64.0f;
    out->h_bearing_x=m->horiBearingX/64.0f; out->h_bearing_y=m->horiBearingY/64.0f; out->h_advance=m->horiAdvance/64.0f;
    out->v_bearing_x=m->vertBearingX/64.0f; out->v_bearing_y=m->vertBearingY/64.0f; out->v_advance=m->vertAdvance/64.0f;
}

static int64_t floor_div64(int64_t v) { return v>=0 ? v/64 : -((-v+63)/64); }

/* ---- Memory, library, renderer ---- */
static ABI int32_t font_memory_init(FontMemory *memory, void *region, uint32_t size, const void *interface,
                                    void *mspace, void *destroy_callback, void *destroy_object) {
    if (!memory) return ERR_INVALID_PARAMETER;
    if (!interface && (!region || !size)) { memory->kind=0; return ERR_INVALID_PARAMETER; }
    memory->kind=MEM_MAGIC; memory->attributes=0; memory->region_size=size; memory->region=region;
    memory->mspace=mspace; memory->interface=interface; memory->destroy_callback=destroy_callback;
    memory->destroy_object=destroy_object; memory->reserved=NULL; memory->parent=mspace;
    return FONT_OK;
}

static ABI int32_t font_memory_term(FontMemory *memory) {
    if (!memory) return ERR_INVALID_PARAMETER;
    if (memory->kind!=MEM_MAGIC) return ERR_INVALID_MEMORY;
    if (memory->destroy_callback) {
        typedef void (ABI *Destroy)(FontMemory *, void *, void *);
        Destroy destroy=(Destroy)memory->destroy_callback;
        memory->kind=0;
        destroy(memory,memory->mspace,memory->destroy_object);
        return FONT_OK;
    }
    memset(memory,0,sizeof(*memory));
    return FONT_OK;
}

static ABI const LibrarySelection *font_select_library_ft(int32_t value) {
    (void)value; return &library_ft;
}
static ABI const RendererSelection *font_select_renderer_ft(int32_t value) {
    (void)value; return &renderer_ft;
}

static ABI int32_t font_create_library(const FontMemory *memory, const void *selection, uint64_t edition, Library **out) {
    if (!out) return ERR_INVALID_PARAMETER;
    *out=NULL;
    if (!memory || memory->kind!=MEM_MAGIC) return ERR_INVALID_MEMORY;
    if (!selection) return ERR_NO_SUPPORT_LIBRARY;
    Library *library=calloc(1,sizeof(*library));
    if (!library) return ERR_ALLOCATION_FAILED;
    pthread_mutex_lock(&lock);
    if (!freetype && FT_Init_FreeType(&freetype)) {
        freetype=NULL;
        pthread_mutex_unlock(&lock); free(library);
        fprintf(stderr,"Font: FreeType initialization failed\n");
        return ERR_FATAL;
    }
    pthread_mutex_unlock(&lock);
    library->magic=LIBRARY_MAGIC; library->memory=memory; library->edition=edition;
    *out=library;
    return FONT_OK;
}

static ABI int32_t font_destroy_library(Library **pointer) {
    if (!pointer) return ERR_INVALID_PARAMETER;
    Library *library=*pointer;
    if (!library || library->magic!=LIBRARY_MAGIC) return ERR_INVALID_LIBRARY;
    pthread_mutex_lock(&lock);
    library->magic=0;
    /* Fonts still open keep using it; the last sceFontCloseFont frees it. */
    int release=!library->fonts;
    pthread_mutex_unlock(&lock);
    if (release) free(library);
    *pointer=NULL;
    return FONT_OK;
}

static ABI int32_t font_support_external_fonts(Library *library, uint32_t max_fonts, uint32_t formats) {
    (void)max_fonts; (void)formats;
    if (!library || library->magic!=LIBRARY_MAGIC) return ERR_INVALID_LIBRARY;
    return FONT_OK;
}

static ABI int32_t font_support_system_fonts(Library *library) {
    return library && library->magic==LIBRARY_MAGIC ? FONT_OK : ERR_INVALID_LIBRARY;
}

static ABI int32_t font_attach_device_cache(Library *library, void *buffer, uint32_t size) {
    (void)buffer; (void)size;
    return library && library->magic==LIBRARY_MAGIC ? FONT_OK : ERR_INVALID_LIBRARY;
}

static ABI int32_t font_create_renderer(const FontMemory *memory, const void *selection, uint64_t edition, Renderer **out) {
    if (!out) return ERR_INVALID_PARAMETER;
    *out=NULL;
    if (!memory || memory->kind!=MEM_MAGIC) return ERR_INVALID_MEMORY;
    if (!selection) return ERR_NO_SUPPORT_LIBRARY;
    Renderer *renderer=calloc(1,sizeof(*renderer));
    if (!renderer) return ERR_ALLOCATION_FAILED;
    renderer->magic=RENDERER_MAGIC; renderer->memory=memory; renderer->edition=edition;
    *out=renderer;
    return FONT_OK;
}

static ABI int32_t font_destroy_renderer(Renderer **pointer) {
    if (!pointer) return ERR_INVALID_PARAMETER;
    Renderer *renderer=*pointer;
    if (!renderer || renderer->magic!=RENDERER_MAGIC) return ERR_INVALID_RENDERER;
    renderer->magic=0;
    /* Fonts may still point at it (PS4 leaves that to the caller); keep the block alive. */
    *pointer=NULL;
    return FONT_OK;
}

/* ---- Fonts ---- */
/* `data` must stay valid until the font is closed. */
static int32_t font_create(Library *library, const void *data, size_t size, const OpenParams *params, Font **out) {
    Font *font=calloc(1,sizeof(*font));
    if (!font) return ERR_ALLOCATION_FAILED;
    FT_Long index=params ? (FT_Long)params->subfont_index : 0;
    pthread_mutex_lock(&lock);
    FT_Error error=FT_New_Memory_Face(freetype,data,(FT_Long)size,index,&font->face);
    if (error) {
        pthread_mutex_unlock(&lock); free(font);
        fprintf(stderr,"Font: open failed (%zu bytes at %p, face %ld, FreeType error %d)\n",size,data,(long)index,error);
        return ERR_FONT_OPEN_FAILED;
    }
    if (!font->face->charmap) FT_Select_Charmap(font->face,FT_ENCODING_UNICODE);
    float scale=font->face->units_per_EM ? font->face->units_per_EM/64.0f : 16.0f;
    font->magic=FONT_MAGIC; font->library=library;
    font->scale_w=font->scale_h=font->render_w=font->render_h=scale;
    ++library->fonts;
    pthread_mutex_unlock(&lock);
    *out=font;
    return FONT_OK;
}

static ABI int32_t font_open_memory(Library *library, const void *data, uint32_t size, const OpenParams *params, Font **out) {
    if (!library || library->magic!=LIBRARY_MAGIC) { if (out) *out=NULL; return ERR_INVALID_LIBRARY; }
    if (!data || !size || !out) { if (out) *out=NULL; return ERR_INVALID_PARAMETER; }
    *out=NULL;
    /* The data stays owned by the guest, which keeps it mapped while the font is open. */
    int32_t result=font_create(library,data,size,params,out);
    if (result==FONT_OK) (*out)->game_font=1;
    return result;
}

/* The PS4 system font sets live in the firmware; known sets open a built-in font instead. */
static ABI int32_t font_open_font_set(Library *library, uint32_t set, uint32_t mode, const OpenParams *params, Font **out) {
    (void)mode;
    if (!library || library->magic!=LIBRARY_MAGIC) { if (out) *out=NULL; return ERR_INVALID_LIBRARY; }
    if (!out) return ERR_INVALID_PARAMETER;
    *out=NULL;
    for (size_t i=0;i<sizeof(builtin_sets)/sizeof(*builtin_sets);++i) {
        if (builtin_sets[i].set!=set) continue;
        int32_t result=font_create(library,builtin_sets[i].data,(size_t)(builtin_sets[i].end-builtin_sets[i].data),params,out);
        if (result==FONT_OK) printf("Font: system font set %#x -> built-in %s\n",set,builtin_sets[i].name);
        return result;
    }
    printf("Font: system font set %#x is not supported\n",set);
    return ERR_FONT_OPEN_FAILED;
}

static ABI int32_t font_close(Font *handle) {
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    if (!font) { pthread_mutex_unlock(&lock); return ERR_INVALID_FONT_HANDLE; }
    FT_Done_Face(font->face);
    font->face=NULL; font->magic=0;
    Library *library=font->library;
    int release=0;
    if (library && --library->fonts==0 && library->magic!=LIBRARY_MAGIC) release=1;
    pthread_mutex_unlock(&lock);
    if (release) free(library);
    free(font);
    return FONT_OK;
}

static ABI int32_t font_bind_renderer(Font *handle, Renderer *renderer) {
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    int32_t result=FONT_OK;
    if (!font) result=ERR_INVALID_FONT_HANDLE;
    else if (font->renderer) result=ERR_ALREADY_BOUND_RENDERER;
    else if (!renderer || renderer->magic!=RENDERER_MAGIC) result=ERR_INVALID_RENDERER;
    else {
        /* Binding starts the render style from the font's current one. */
        font->renderer=renderer; font->render_w=font->scale_w; font->render_h=font->scale_h;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

static ABI int32_t font_unbind_renderer(Font *handle) {
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    int32_t result=!font ? ERR_INVALID_FONT_HANDLE : !font->renderer ? ERR_NOT_BOUND_RENDERER : FONT_OK;
    if (result==FONT_OK) font->renderer=NULL;
    pthread_mutex_unlock(&lock);
    return result;
}

static ABI int32_t font_set_scale_pixel(Font *handle, float w, float h) {
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    if (font) { font->scale_w=w; font->scale_h=h; }
    pthread_mutex_unlock(&lock);
    return font ? FONT_OK : ERR_INVALID_FONT_HANDLE;
}

static ABI int32_t font_setup_render_scale_pixel(Font *handle, float w, float h) {
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    int32_t result=!font ? ERR_INVALID_FONT_HANDLE : !font->renderer ? ERR_NOT_BOUND_RENDERER : FONT_OK;
    if (result==FONT_OK) { font->render_w=w; font->render_h=h; }
    pthread_mutex_unlock(&lock);
    return result;
}

/* decorationExtent only grows with weight effects, which the game never sets. */
static ABI int32_t font_get_horizontal_layout(Font *handle, HorizontalLayout *layout) {
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    int32_t result=FONT_OK;
    if (!font) result=ERR_INVALID_FONT_HANDLE;
    else if (!layout) result=ERR_INVALID_PARAMETER;
    else if (set_size(font,font->scale_w,font->scale_h)) result=ERR_INVALID_PARAMETER;
    else {
        FT_Long top, bottom;
        line_extent(font,&top,&bottom);
        layout->baseline_offset=top/64.0f;
        layout->line_advance=(top-bottom)/64.0f;
        layout->decoration_extent=0.0f;
    }
    if (result!=FONT_OK && layout) memset(layout,0,sizeof(*layout));
    pthread_mutex_unlock(&lock);
    return result;
}

/* Column width and centre line from the font bounding box (head.xMax / xMin), at the font scale. */
static ABI int32_t font_get_vertical_layout(Font *handle, VerticalLayout *layout) {
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    int32_t result=FONT_OK;
    if (!font) result=ERR_INVALID_FONT_HANDLE;
    else if (!layout) result=ERR_INVALID_PARAMETER;
    else if (set_size(font,font->scale_w,font->scale_h)) result=ERR_INVALID_PARAMETER;
    else {
        FT_Fixed x_scale=font->face->size->metrics.x_scale;
        FT_Long left=FT_MulFix(font->face->bbox.xMin,x_scale), right=FT_MulFix(font->face->bbox.xMax,x_scale);
        if (!FT_IS_SCALABLE(font->face)) { left=0; right=font->face->size->metrics.max_advance; }
        layout->baseline_offset_x=-(right-left)/128.0f;
        layout->column_advance=(right-left)/64.0f;
        layout->decoration_span=0.0f;
    }
    if (result!=FONT_OK && layout) memset(layout,0,sizeof(*layout));
    pthread_mutex_unlock(&lock);
    return result;
}

static ABI void font_render_surface_init(RenderSurface *surface, void *buffer, int32_t width_byte,
                                         int32_t pixel_size_byte, int32_t width, int32_t height) {
    if (!surface) return;
    if (width<0) width=0;
    if (height<0) height=0;
    memset(surface,0,sizeof(*surface));
    surface->buffer=buffer; surface->width_byte=width_byte; surface->pixel_size_byte=(int8_t)pixel_size_byte;
    surface->width=width; surface->height=height;
    surface->scissor_x1=(uint32_t)width; surface->scissor_y1=(uint32_t)height;
}

static int ceil_int(float v) { return (int)ceilf(v); }
static int floor_int(float v) { return (int)floorf(v); }

typedef struct {
    const uint8_t *top_row;
    ptrdiff_t pitch;
    int width, height, mono, grays;
} Coverage;

static Coverage bitmap_coverage(const FT_Bitmap *bitmap) {
    Coverage c={NULL,bitmap->pitch,(int)bitmap->width,(int)bitmap->rows,bitmap->pixel_mode==FT_PIXEL_MODE_MONO,bitmap->num_grays};
    if (bitmap->pixel_mode==FT_PIXEL_MODE_GRAY || bitmap->pixel_mode==FT_PIXEL_MODE_MONO)
        /* A negative pitch stores the rows bottom-up: the top row is the last one in the buffer. */
        c.top_row=bitmap->pitch<0 ? bitmap->buffer-(ptrdiff_t)bitmap->pitch*(c.height-1) : bitmap->buffer;
    return c;
}

static uint8_t coverage_at(const Coverage *c, int64_t x, int64_t y) {
    const uint8_t *row=c->top_row+y*c->pitch;
    if (c->mono) return (row[x>>3]>>(7-(x&7)))&1 ? 255 : 0;
    return c->grays==256 || c->grays<2 ? row[x] : (uint8_t)(row[x]*255/(c->grays-1));
}

/* Writes `c` with its top-left at dest, clipped to the scissor; `update` gets the rectangle written. */
static void blit(RenderSurface *surface, int bpp, const Coverage *c, int64_t dest_x, int64_t dest_y, int update[4]) {
    int64_t clip_x0=surface->scissor_x0, clip_y0=surface->scissor_y0;
    int64_t clip_x1=surface->scissor_x1<(uint32_t)surface->width ? surface->scissor_x1 : (uint32_t)surface->width;
    int64_t clip_y1=surface->scissor_y1<(uint32_t)surface->height ? surface->scissor_y1 : (uint32_t)surface->height;
    if ((int64_t)surface->width_byte<clip_x1*bpp) clip_x1=surface->width_byte/bpp;
    int64_t x0=dest_x>clip_x0 ? dest_x : clip_x0, y0=dest_y>clip_y0 ? dest_y : clip_y0;
    int64_t x1=dest_x+c->width<clip_x1 ? dest_x+c->width : clip_x1, y1=dest_y+c->height<clip_y1 ? dest_y+c->height : clip_y1;
    if (x1>x0 && y1>y0 && c->top_row) {
        uint8_t *base=surface->buffer;
        for (int64_t row=y0;row<y1;++row) {
            uint8_t *target=base+row*surface->width_byte;
            for (int64_t column=x0;column<x1;++column) {
                uint8_t value=coverage_at(c,column-dest_x,row-dest_y);
                if (bpp==1) target[column]=value;
                else memset(target+column*4,value,4);
            }
        }
        update[0]=(int)x0; update[1]=(int)y0; update[2]=(int)(x1-x0); update[3]=(int)(y1-y0);
    } else {
        update[0]=(int)(x0<clip_x1 ? x0 : clip_x1); update[1]=(int)(y0<clip_y1 ? y0 : clip_y1);
        if (update[0]<0) update[0]=0;
        if (update[1]<0) update[1]=0;
        update[2]=update[3]=0;
    }
}

/* launcher/latin_font.cpp's ink threshold: the boxes the letters were composed against. */
#define COMPOSED_INK 40

typedef struct { int x0, y0, x1, y1; } InkBox; /* half-open */

static InkBox coverage_ink(const Coverage *c) {
    InkBox box={c->width,c->height,0,0};
    for (int y=0;c->top_row && y<c->height;++y)
        for (int x=0;x<c->width;++x)
            if (coverage_at(c,x,y)>COMPOSED_INK) {
                if (x<box.x0) box.x0=x;
                if (y<box.y0) box.y0=y;
                if (x+1>box.x1) box.x1=x+1;
                if (y+1>box.y1) box.y1=y+1;
            }
    return box;
}

static float cell_sample(const uint8_t *alpha, int cell, float u, float v) {
    int x=floor_int(u), y=floor_int(v);
    float fx=u-(float)x, fy=v-(float)y, sum=0.0f;
    for (int dy=0;dy<2;++dy)
        for (int dx=0;dx<2;++dx) {
            int sx=x+dx, sy=y+dy;
            if (sx<0 || sy<0 || sx>=cell || sy>=cell) continue;
            sum+=alpha[sy*cell+sx]*(dx ? fx : 1.0f-fx)*(dy ? fy : 1.0f-fy);
        }
    return sum;
}

/* The composed cell placed so that the atlas's base letter covers the font's own `base` ink: the letter keeps this
 * font's size, baseline and advance. Returns the pixels (caller frees) and their surface box. */
static uint8_t *fit_composed(const RuntimeComposedGlyph *glyph, int cell, const Coverage *base, int64_t base_x, int64_t base_y,
                             float box[4], int64_t *left, int64_t *top, int *width, int *height) {
    const Coverage atlas_base={glyph->base_alpha,cell,cell,cell,0,256}, letter={glyph->alpha,cell,cell,cell,0,256};
    const InkBox own=coverage_ink(base), from=coverage_ink(&atlas_base), ink=coverage_ink(&letter);
    if (own.x1<=own.x0 || from.x1<=from.x0 || ink.x1<=ink.x0) return NULL;
    const float scale_x=(float)(own.x1-own.x0)/(float)(from.x1-from.x0), scale_y=(float)(own.y1-own.y0)/(float)(from.y1-from.y0);
    const float origin_x=(float)(base_x+own.x0), origin_y=(float)(base_y+own.y0);
    box[0]=origin_x+(float)(ink.x0-from.x0)*scale_x; box[2]=origin_x+(float)(ink.x1-from.x0)*scale_x;
    box[1]=origin_y+(float)(ink.y0-from.y0)*scale_y; box[3]=origin_y+(float)(ink.y1-from.y0)*scale_y;
    *left=floor_int(box[0]); *top=floor_int(box[1]);
    *width=ceil_int(box[2])-(int)*left; *height=ceil_int(box[3])-(int)*top;
    uint8_t *pixels=*width>0 && *height>0 ? malloc((size_t)*width*(size_t)*height) : NULL;
    enum { SAMPLES=4 };
    for (int y=0;pixels && y<*height;++y)
        for (int x=0;x<*width;++x) {
            float sum=0.0f;
            for (int sy=0;sy<SAMPLES;++sy)
                for (int sx=0;sx<SAMPLES;++sx) {
                    float px=(float)(*left+x)+(sx+0.5f)/SAMPLES, py=(float)(*top+y)+(sy+0.5f)/SAMPLES;
                    sum+=cell_sample(glyph->alpha,cell,from.x0+(px-origin_x)/scale_x-0.5f,from.y0+(py-origin_y)/scale_y-0.5f);
                }
            float value=sum/(SAMPLES*SAMPLES);
            pixels[y**width+x]=(uint8_t)(value>255.0f ? 255.0f : value+0.5f);
        }
    return pixels;
}

/* Rasterizes `code` into the surface (pixels, y down), clipped to the scissor. Unlike the
 * ...Horizontal variant, y is the top of the line: the baseline is y plus the horizontal layout's
 * baseline offset at the render scale, which is what the game's glyph cells rely on. A missing
 * glyph returns ERR_NO_SUPPORT_GLYPH: the game then asks its next font. */
static ABI int32_t font_render_char_glyph_image(Font *handle, uint32_t code, RenderSurface *surface,
                                                float x, float y, GlyphMetrics *metrics, RenderOutput *output) {
    if (metrics) memset(metrics,0,sizeof(*metrics));
    if (output) memset(output,0,sizeof(*output));
    if (!code) return ERR_NO_SUPPORT_CODE;
    if (!surface || !metrics || !output) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Font *font=font_of(handle);
    int32_t result=FONT_OK;
    int bpp=surface->pixel_size_byte;
    const RuntimeComposedGlyph *composed=NULL;
    if (!font) result=ERR_INVALID_FONT_HANDLE;
    else if (!font->renderer) result=ERR_NOT_BOUND_RENDERER;
    else if (!surface->buffer || surface->width<=0 || surface->height<=0 || surface->width_byte<=0 || (bpp!=1 && bpp!=4))
        result=ERR_NO_SUPPORT_SURFACE;
    else if (!isfinite(x) || !isfinite(y) || fabsf(x)>1e6f || fabsf(y)>1e6f) result=ERR_INVALID_PARAMETER;
    else if (set_size(font,font->render_w,font->render_h)) result=ERR_INVALID_PARAMETER;
    else if (load_glyph(font,code)) {
        /* The game's own font draws a composed letter it lacks, so the letter matches its neighbours. */
        composed=font->game_font ? composed_glyph(code) : NULL;
        if (!composed || load_glyph(font,composed->base)) result=ERR_NO_SUPPORT_GLYPH;
    }
    if (result==FONT_OK) {
        FT_GlyphSlot slot=font->face->glyph;
        fill_metrics(&slot->metrics,metrics);
        FT_Long baseline, descent;
        line_extent(font,&baseline,&descent);
        const float pen_origin_y=y+baseline/64.0f;
        int64_t pen_x=llrintf(x*64.0f), pen_y=llrintf(pen_origin_y*64.0f);
        int64_t cell_x=floor_div64(pen_x), cell_y=floor_div64(pen_y);
        if (slot->format==FT_GLYPH_FORMAT_OUTLINE)
            FT_Outline_Translate(&slot->outline,(FT_Pos)(pen_x-cell_x*64),(FT_Pos)-(pen_y-cell_y*64));
        int update[4]={0,0,0,0};
        if (FT_Render_Glyph(slot,FT_RENDER_MODE_NORMAL)) result=ERR_NO_SUPPORT_GLYPH;
        else {
            Coverage coverage=bitmap_coverage(&slot->bitmap);
            int64_t dest_x=cell_x+slot->bitmap_left, dest_y=cell_y-slot->bitmap_top;
            uint8_t *fitted=NULL;
            if (composed) {
                float box[4];
                int width=0, height=0;
                fitted=fit_composed(composed,composed_cell,&coverage,dest_x,dest_y,box,&dest_x,&dest_y,&width,&height);
                if (!fitted) result=ERR_NO_SUPPORT_GLYPH;
                else {
                    coverage=(Coverage){fitted,width,width,height,0,256};
                    metrics->width=box[2]-box[0]; metrics->height=box[3]-box[1];
                    metrics->h_bearing_x=box[0]-x; metrics->h_bearing_y=pen_origin_y-box[1];
                }
            }
            if (result==FONT_OK) blit(surface,bpp,&coverage,dest_x,dest_y,update);
            free(fitted);
        }
        if (result==FONT_OK) {
            output->stage=NULL;
            output->image.address=(uint8_t *)surface->buffer+(size_t)update[1]*(size_t)surface->width_byte+(size_t)update[0]*(size_t)bpp;
            output->image.width_byte=(uint32_t)surface->width_byte;
            output->image.pixel_size_byte=(uint8_t)bpp;
            output->update.x=(uint32_t)update[0]; output->update.y=(uint32_t)update[1];
            output->update.w=(uint32_t)update[2]; output->update.h=(uint32_t)update[3];
            /* Image metrics: integer pixel box of the glyph relative to the pen position. */
            float left=x+metrics->h_bearing_x, top=pen_origin_y-metrics->h_bearing_y;
            float right=left+metrics->width, bottom=top+metrics->height;
            int left_i=floor_int(left), top_i=floor_int(top), right_i=ceil_int(right), bottom_i=ceil_int(bottom);
            int advance_i=ceil_int(x+metrics->h_advance);
            output->metrics.bearing_x=(float)left_i-x;
            output->metrics.bearing_y=pen_origin_y-(float)top_i;
            output->metrics.advance=(float)advance_i-x;
            output->metrics.stride=(float)(advance_i>right_i ? advance_i : right_i)-x;
            output->metrics.width=right_i>left_i ? (uint32_t)(right_i-left_i) : 0;
            output->metrics.height=bottom_i>top_i ? (uint32_t)(bottom_i-top_i) : 0;
        }
    }
    if (result!=FONT_OK) { memset(metrics,0,sizeof(*metrics)); memset(output,0,sizeof(*output)); }
    pthread_mutex_unlock(&lock);
    return result;
}

static const RuntimeExport exports[]={
    {"sceFontMemoryInit",font_memory_init},
    {"sceFontMemoryTerm",font_memory_term},
    {"sceFontSelectLibraryFt",font_select_library_ft},
    {"sceFontSelectRendererFt",font_select_renderer_ft},
    {"sceFontCreateLibraryWithEdition",font_create_library},
    {"sceFontDestroyLibrary",font_destroy_library},
    {"sceFontSupportExternalFonts",font_support_external_fonts},
    {"sceFontSupportSystemFonts",font_support_system_fonts},
    {"sceFontAttachDeviceCacheBuffer",font_attach_device_cache},
    {"sceFontCreateRendererWithEdition",font_create_renderer},
    {"sceFontDestroyRenderer",font_destroy_renderer},
    {"sceFontOpenFontMemory",font_open_memory},
    {"sceFontOpenFontSet",font_open_font_set},
    {"sceFontCloseFont",font_close},
    {"sceFontBindRenderer",font_bind_renderer},
    {"sceFontUnbindRenderer",font_unbind_renderer},
    {"sceFontSetScalePixel",font_set_scale_pixel},
    {"sceFontSetupRenderScalePixel",font_setup_render_scale_pixel},
    {"sceFontGetHorizontalLayout",font_get_horizontal_layout},
    {"sceFontRenderSurfaceInit",font_render_surface_init},
    {"sceFontGetVerticalLayout",font_get_vertical_layout},
    {"sceFontRenderCharGlyphImage",font_render_char_glyph_image},
};
uintptr_t runtime_font_resolve(const char *name) {
    return RUNTIME_LOOKUP(exports,name);
}
