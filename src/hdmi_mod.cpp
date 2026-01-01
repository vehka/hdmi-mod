#include "hdmi_mod.h"

#include <iostream>
#include <map>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>

// lua
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

extern "C" {
// matron
#include "lua_eval.h"
#include "hardware/screen.h"
// cairo
#include <cairo.h>
#include <cairo-ft.h>
// freetype
#include <ft2build.h>
#include FT_FREETYPE_H
// local copies of private weaver types/methods
#include <weaver_image.h>
}

struct FramebufferInfo {
    int fd;
    uint8_t* mapped_mem;
    size_t screen_size;
    int width;
    int height;
    int bpp;
    int line_length;
};

// Our own cairo surface and context for mirroring screen drawing
static cairo_surface_t* mirror_surface = NULL;
static cairo_t* mirror_ctx = NULL;

// Single framebuffer info for HDMI output
static FramebufferInfo* hdmi_fb = NULL;

// Font management
#define NUM_FONTS 69
static FT_Library ft_library;
static FT_Face ft_faces[NUM_FONTS];
static cairo_font_face_t* font_faces[NUM_FONTS];
static const char* font_paths[NUM_FONTS];
static bool fonts_initialized = false;

static bool running = false;
static bool initialized = false;
static bool failed = false;

// Default scaling factors and output resolution
static int output_width = 1920;
static int output_height = 1080;
static int scale_x = 15;  // 1920 / 128
static int scale_y = 15;  // 960 / 64 (with letterboxing)
static int offset_y = 60;  // Center vertically in 1080p

//
// core functions
//

#define MSG(contents) \
   std::cerr << "hdmi-mod: " << contents << "\n"

void scale_and_copy_buffer(unsigned char* src, int src_width, int src_height, int src_stride,
                           uint8_t* dst, int dst_width, int dst_height, int dst_line_length) {
    // Simple nearest-neighbor scaling
    // norns screen is 128x64 ARGB32, we'll scale it up and center it

    // Calculate actual output dimensions maintaining aspect ratio
    int out_height = scale_y * src_height;
    int out_width = scale_x * src_width;

    // Center the output in the framebuffer
    int x_offset = (dst_width - out_width) / 2;
    int y_offset = offset_y;

    // Clear the buffer first (black background)
    memset(dst, 0, dst_line_length * dst_height);

    for (int y = 0; y < src_height; y++) {
        for (int x = 0; x < src_width; x++) {
            // Get source pixel (ARGB32 format)
            uint32_t* src_pixel = (uint32_t*)(src + y * src_stride + x * 4);
            uint32_t pixel = *src_pixel;

            // Extract RGB (ignoring alpha since norns uses grayscale)
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;

            // Scale up the pixel
            for (int dy = 0; dy < scale_y; dy++) {
                for (int dx = 0; dx < scale_x; dx++) {
                    int out_x = x_offset + x * scale_x + dx;
                    int out_y = y_offset + y * scale_y + dy;

                    if (out_x >= 0 && out_x < dst_width && out_y >= 0 && out_y < dst_height) {
                        uint32_t* dst_pixel = (uint32_t*)(dst + out_y * dst_line_length + out_x * 4);
                        // Write as RGBA/XRGB (most common format)
                        *dst_pixel = (r << 16) | (g << 8) | b;
                    }
                }
            }
        }
    }
}

int open_hdmi_framebuffer(const char* device_path) {
    if (hdmi_fb != NULL) {
        MSG("framebuffer already open");
        return 0;
    }

    // Open framebuffer device
    int fd = open(device_path, O_RDWR);
    if (fd < 0) {
        MSG("error opening framebuffer device " << device_path << ": " << strerror(errno));
        return -1;
    }

    // Get fixed screen information
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        MSG("error reading fixed screen info: " << strerror(errno));
        close(fd);
        return -1;
    }

    // Get variable screen information
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        MSG("error reading variable screen info: " << strerror(errno));
        close(fd);
        return -1;
    }

    // Store framebuffer info
    hdmi_fb = new FramebufferInfo();
    hdmi_fb->fd = fd;
    hdmi_fb->width = vinfo.xres;
    hdmi_fb->height = vinfo.yres;
    hdmi_fb->bpp = vinfo.bits_per_pixel;
    hdmi_fb->line_length = finfo.line_length;
    hdmi_fb->screen_size = finfo.smem_len;

    // Memory map the framebuffer
    hdmi_fb->mapped_mem = (uint8_t*)mmap(0, hdmi_fb->screen_size,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0);

    if (hdmi_fb->mapped_mem == MAP_FAILED) {
        MSG("error mapping framebuffer: " << strerror(errno));
        close(fd);
        delete hdmi_fb;
        hdmi_fb = NULL;
        return -1;
    }

    MSG("framebuffer output created: " << hdmi_fb->width << "x" << hdmi_fb->height
        << " @ " << hdmi_fb->bpp << "bpp (device: " << device_path << ")");

    // Update scaling based on actual resolution
    output_width = hdmi_fb->width;
    output_height = hdmi_fb->height;
    scale_x = output_width / 128;
    scale_y = scale_x; // Keep square pixels
    offset_y = (output_height - (64 * scale_y)) / 2;

    MSG("scaling: " << scale_x << "x" << scale_y << ", offset_y: " << offset_y);

    return 0;
}

void close_hdmi_framebuffer() {
    if (hdmi_fb == NULL) {
        return;
    }

    if (hdmi_fb->mapped_mem != MAP_FAILED) {
        munmap(hdmi_fb->mapped_mem, hdmi_fb->screen_size);
    }

    if (hdmi_fb->fd >= 0) {
        close(hdmi_fb->fd);
    }

    delete hdmi_fb;
    hdmi_fb = NULL;

    MSG("framebuffer output closed");
}

void init_fonts() {
    if (fonts_initialized) {
        return;
    }

    // Initialize FreeType
    FT_Error status = FT_Init_FreeType(&ft_library);
    if (status != 0) {
        MSG("ERROR: FreeType init failed");
        return;
    }

    // Initialize font paths (matching norns font indices)
    int idx = 0;
    font_paths[idx++] = "norns.ttf";               // 1
    font_paths[idx++] = "liquid.ttf";              // 2 (ALEPH)
    font_paths[idx++] = "Roboto-Thin.ttf";         // 3
    font_paths[idx++] = "Roboto-Light.ttf";        // 4
    font_paths[idx++] = "Roboto-Regular.ttf";      // 5
    font_paths[idx++] = "Roboto-Medium.ttf";       // 6
    font_paths[idx++] = "Roboto-Bold.ttf";         // 7
    font_paths[idx++] = "Roboto-Black.ttf";        // 8
    font_paths[idx++] = "Roboto-ThinItalic.ttf";   // 9
    font_paths[idx++] = "Roboto-LightItalic.ttf";  // 10
    font_paths[idx++] = "Roboto-Italic.ttf";       // 11
    font_paths[idx++] = "Roboto-MediumItalic.ttf"; // 12
    font_paths[idx++] = "Roboto-BoldItalic.ttf";   // 13
    font_paths[idx++] = "Roboto-BlackItalic.ttf";  // 14
    font_paths[idx++] = "VeraBd.ttf";              // 15
    font_paths[idx++] = "VeraBI.ttf";              // 16
    font_paths[idx++] = "VeraIt.ttf";              // 17
    font_paths[idx++] = "VeraMoBd.ttf";            // 18
    font_paths[idx++] = "VeraMoBI.ttf";            // 19
    font_paths[idx++] = "VeraMoIt.ttf";            // 20
    font_paths[idx++] = "VeraMono.ttf";            // 21
    font_paths[idx++] = "VeraSeBd.ttf";            // 22
    font_paths[idx++] = "VeraSe.ttf";              // 23
    font_paths[idx++] = "Vera.ttf";                // 24
    // Bitmap fonts
    font_paths[idx++] = "bmp/tom-thumb.bdf";       // 25
    font_paths[idx++] = "bmp/creep.bdf";           // 26
    font_paths[idx++] = "bmp/ctrld-fixed-10b.bdf"; // 27
    font_paths[idx++] = "bmp/ctrld-fixed-10r.bdf"; // 28
    font_paths[idx++] = "bmp/ctrld-fixed-13b.bdf"; // 29
    font_paths[idx++] = "bmp/ctrld-fixed-13b-i.bdf"; // 30
    font_paths[idx++] = "bmp/ctrld-fixed-13r.bdf"; // 31
    font_paths[idx++] = "bmp/ctrld-fixed-13r-i.bdf"; // 32
    font_paths[idx++] = "bmp/ctrld-fixed-16b.bdf"; // 33
    font_paths[idx++] = "bmp/ctrld-fixed-16b-i.bdf"; // 34
    font_paths[idx++] = "bmp/ctrld-fixed-16r.bdf"; // 35
    font_paths[idx++] = "bmp/ctrld-fixed-16r-i.bdf"; // 36
    font_paths[idx++] = "bmp/scientifica-11.bdf";  // 37
    font_paths[idx++] = "bmp/scientificaBold-11.bdf"; // 38
    font_paths[idx++] = "bmp/scientificaItalic-11.bdf"; // 39
    font_paths[idx++] = "bmp/ter-u12b.bdf";        // 40
    font_paths[idx++] = "bmp/ter-u12n.bdf";        // 41
    font_paths[idx++] = "bmp/ter-u14b.bdf";        // 42
    font_paths[idx++] = "bmp/ter-u14n.bdf";        // 43
    font_paths[idx++] = "bmp/ter-u14v.bdf";        // 44
    font_paths[idx++] = "bmp/ter-u16b.bdf";        // 45
    font_paths[idx++] = "bmp/ter-u16n.bdf";        // 46
    font_paths[idx++] = "bmp/ter-u16v.bdf";        // 47
    font_paths[idx++] = "bmp/ter-u18b.bdf";        // 48
    font_paths[idx++] = "bmp/ter-u18n.bdf";        // 49
    font_paths[idx++] = "bmp/ter-u20b.bdf";        // 50
    font_paths[idx++] = "bmp/ter-u20n.bdf";        // 51
    font_paths[idx++] = "bmp/ter-u22b.bdf";        // 52
    font_paths[idx++] = "bmp/ter-u22n.bdf";        // 53
    font_paths[idx++] = "bmp/ter-u24b.bdf";        // 54
    font_paths[idx++] = "bmp/ter-u24n.bdf";        // 55
    font_paths[idx++] = "bmp/ter-u28b.bdf";        // 56
    font_paths[idx++] = "bmp/ter-u28n.bdf";        // 57
    font_paths[idx++] = "bmp/ter-u32b.bdf";        // 58
    font_paths[idx++] = "bmp/ter-u32n.bdf";        // 59
    font_paths[idx++] = "bmp/unscii-16-full.pcf";  // 60
    font_paths[idx++] = "bmp/unscii-16.pcf";       // 61
    font_paths[idx++] = "bmp/unscii-8-alt.pcf";    // 62
    font_paths[idx++] = "bmp/unscii-8-fantasy.pcf"; // 63
    font_paths[idx++] = "bmp/unscii-8-mcr.pcf";    // 64
    font_paths[idx++] = "bmp/unscii-8.pcf";        // 65
    font_paths[idx++] = "bmp/unscii-8-tall.pcf";   // 66
    font_paths[idx++] = "bmp/unscii-8-thin.pcf";   // 67
    font_paths[idx++] = "Particle.ttf";            // 68
    font_paths[idx++] = "norns.ttf";               // 69 (alias for 04B_03)

    // Load all fonts
    char full_path[512];
    const char* home = getenv("HOME");
    if (!home) home = "/home/we";

    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(full_path, sizeof(full_path), "%s/norns/resources/%s", home, font_paths[i]);

        status = FT_New_Face(ft_library, full_path, 0, &ft_faces[i]);
        if (status != 0) {
            MSG("Warning: couldn't load font " << i+1 << ": " << font_paths[i]);
            font_faces[i] = NULL;
        } else {
            font_faces[i] = cairo_ft_font_face_create_for_ft_face(ft_faces[i], 0);
            if (!font_faces[i]) {
                MSG("Warning: couldn't create cairo font face for " << font_paths[i]);
            }
        }
    }

    MSG("Loaded " << NUM_FONTS << " fonts");
    fonts_initialized = true;
}

void cleanup_fonts() {
    if (!fonts_initialized) {
        return;
    }

    for (int i = 0; i < NUM_FONTS; i++) {
        if (font_faces[i]) {
            cairo_font_face_destroy(font_faces[i]);
            font_faces[i] = NULL;
        }
        if (ft_faces[i]) {
            FT_Done_Face(ft_faces[i]);
            ft_faces[i] = NULL;
        }
    }

    if (ft_library) {
        FT_Done_FreeType(ft_library);
        ft_library = NULL;
    }

    fonts_initialized = false;
}

int initialize_hdmi() {
    if (!initialized && !failed) {
        MSG("HDMI output service initializing");

        // Initialize fonts
        init_fonts();

        // Create our own cairo surface for mirroring (128x64, ARGB32 like norns)
        mirror_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 64);
        if (cairo_surface_status(mirror_surface) != CAIRO_STATUS_SUCCESS) {
            MSG("failed to create mirror surface");
            failed = true;
            return 0;
        }

        // Create cairo context for our surface
        mirror_ctx = cairo_create(mirror_surface);
        if (cairo_status(mirror_ctx) != CAIRO_STATUS_SUCCESS) {
            MSG("failed to create mirror context");
            cairo_surface_destroy(mirror_surface);
            mirror_surface = NULL;
            failed = true;
            return 0;
        }

        // Set default drawing state to match norns
        cairo_set_source_rgb(mirror_ctx, 1.0, 1.0, 1.0);
        cairo_set_line_width(mirror_ctx, 1.0);
        cairo_set_line_cap(mirror_ctx, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(mirror_ctx, CAIRO_LINE_JOIN_ROUND);

        MSG("mirror surface created: 128x64");

        // Try to open framebuffer device
        int result = open_hdmi_framebuffer("/dev/fb0");
        if (result < 0) {
            result = open_hdmi_framebuffer("/dev/fb1");
        }

        if (hdmi_fb == NULL) {
            MSG("failed to open any framebuffer device");
            cairo_destroy(mirror_ctx);
            cairo_surface_destroy(mirror_surface);
            mirror_ctx = NULL;
            mirror_surface = NULL;
            failed = true;
            return 0;
        }

        initialized = true;
        MSG("HDMI output service initialized");
    }
    return 0;
}

int cleanup_hdmi() {
    running = false;
    if (initialized) {
        initialized = false;

        // Destroy our cairo context and surface
        if (mirror_ctx != NULL) {
            cairo_destroy(mirror_ctx);
            mirror_ctx = NULL;
        }
        if (mirror_surface != NULL) {
            cairo_surface_destroy(mirror_surface);
            mirror_surface = NULL;
        }

        // Close framebuffer
        close_hdmi_framebuffer();

        // Cleanup fonts
        cleanup_fonts();

        MSG("HDMI output service stopped");
    }
    return 0;
}

int send_mirror_to_framebuffer()
{
    if (!initialized || failed || hdmi_fb == NULL || mirror_surface == NULL) {
        return 0;
    }

    // Prepare the mirror surface
    if (cairo_surface_get_type(mirror_surface) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_surface_status(mirror_surface) != CAIRO_STATUS_SUCCESS) {
        return 0;
    }
    cairo_surface_flush(mirror_surface);

    // Get surface data and scale/copy to framebuffer
    unsigned char* data = cairo_image_surface_get_data(mirror_surface);
    if (data != NULL) {
        int src_width = cairo_image_surface_get_width(mirror_surface);
        int src_height = cairo_image_surface_get_height(mirror_surface);
        int src_stride = cairo_image_surface_get_stride(mirror_surface);

        scale_and_copy_buffer(data, src_width, src_height, src_stride,
                            hdmi_fb->mapped_mem, hdmi_fb->width, hdmi_fb->height,
                            hdmi_fb->line_length);
    }
    return 0;
}

//
// lua method implementations
//

static int hdmi_mod_init(lua_State *l) {
    lua_check_num_args(0);
    return initialize_hdmi();
}

static int hdmi_mod_cleanup(lua_State *l) {
    lua_check_num_args(0);
    return cleanup_hdmi();
}

static int hdmi_mod_update(lua_State *l) {
    lua_check_num_args(0);
    if (running) {
        return send_mirror_to_framebuffer();
    }
    return 0;
}

// Screen drawing wrapper functions - these mirror the norns screen API
// and draw to our mirror surface

static int hdmi_mod_clear(lua_State *l) {
    lua_check_num_args(0);
    if (mirror_ctx != NULL) {
        cairo_save(mirror_ctx);
        cairo_set_operator(mirror_ctx, CAIRO_OPERATOR_CLEAR);
        cairo_paint(mirror_ctx);
        cairo_restore(mirror_ctx);
    }
    return 0;
}

static int hdmi_mod_move(lua_State *l) {
    lua_check_num_args(2);
    double x = luaL_checknumber(l, 1);
    double y = luaL_checknumber(l, 2);
    if (mirror_ctx != NULL) {
        cairo_move_to(mirror_ctx, x, y);
    }
    return 0;
}

static int hdmi_mod_line(lua_State *l) {
    lua_check_num_args(2);
    double x = luaL_checknumber(l, 1);
    double y = luaL_checknumber(l, 2);
    if (mirror_ctx != NULL) {
        cairo_line_to(mirror_ctx, x, y);
    }
    return 0;
}

static int hdmi_mod_rect(lua_State *l) {
    lua_check_num_args(4);
    double x = luaL_checknumber(l, 1);
    double y = luaL_checknumber(l, 2);
    double w = luaL_checknumber(l, 3);
    double h = luaL_checknumber(l, 4);
    if (mirror_ctx != NULL) {
        cairo_rectangle(mirror_ctx, x, y, w, h);
    }
    return 0;
}

static int hdmi_mod_stroke(lua_State *l) {
    lua_check_num_args(0);
    if (mirror_ctx != NULL) {
        cairo_stroke(mirror_ctx);
    }
    return 0;
}

static int hdmi_mod_fill(lua_State *l) {
    lua_check_num_args(0);
    if (mirror_ctx != NULL) {
        cairo_fill(mirror_ctx);
    }
    return 0;
}

static int hdmi_mod_level(lua_State *l) {
    lua_check_num_args(1);
    int level = luaL_checkinteger(l, 1);
    if (mirror_ctx != NULL) {
        double brightness = level / 15.0;
        cairo_set_source_rgb(mirror_ctx, brightness, brightness, brightness);
    }
    return 0;
}

static int hdmi_mod_line_width(lua_State *l) {
    lua_check_num_args(1);
    double width = luaL_checknumber(l, 1);
    if (mirror_ctx != NULL) {
        cairo_set_line_width(mirror_ctx, width);
    }
    return 0;
}

// Font and text functions

static int hdmi_mod_font_face(lua_State *l) {
    lua_check_num_args(1);
    int font_index = luaL_checkinteger(l, 1);

    if (mirror_ctx != NULL && fonts_initialized) {
        // Lua uses 1-based indexing, C uses 0-based
        int idx = font_index - 1;

        if (idx >= 0 && idx < NUM_FONTS && font_faces[idx] != NULL) {
            cairo_set_font_face(mirror_ctx, font_faces[idx]);
        } else {
            MSG("Warning: invalid font index " << font_index);
        }
    }
    return 0;
}

static int hdmi_mod_font_size(lua_State *l) {
    lua_check_num_args(1);
    double size = luaL_checknumber(l, 1);
    if (mirror_ctx != NULL) {
        cairo_set_font_size(mirror_ctx, size);
    }
    return 0;
}

static int hdmi_mod_text(lua_State *l) {
    lua_check_num_args(1);
    const char* text = luaL_checkstring(l, 1);
    if (mirror_ctx != NULL) {
        cairo_show_text(mirror_ctx, text);
    }
    return 0;
}

static int hdmi_mod_text_center(lua_State *l) {
    lua_check_num_args(1);
    const char* text = luaL_checkstring(l, 1);
    if (mirror_ctx != NULL) {
        cairo_text_extents_t extents;
        cairo_text_extents(mirror_ctx, text, &extents);

        double x, y;
        cairo_get_current_point(mirror_ctx, &x, &y);
        cairo_move_to(mirror_ctx, x - extents.width / 2.0, y);
        cairo_show_text(mirror_ctx, text);
    }
    return 0;
}

static int hdmi_mod_text_right(lua_State *l) {
    lua_check_num_args(1);
    const char* text = luaL_checkstring(l, 1);
    if (mirror_ctx != NULL) {
        cairo_text_extents_t extents;
        cairo_text_extents(mirror_ctx, text, &extents);

        double x, y;
        cairo_get_current_point(mirror_ctx, &x, &y);
        cairo_move_to(mirror_ctx, x - extents.width, y);
        cairo_show_text(mirror_ctx, text);
    }
    return 0;
}

static int hdmi_mod_start(lua_State *l) {
    lua_check_num_args(0);
    running = true;
    return 0;
}

static int hdmi_mod_stop(lua_State *l) {
    lua_check_num_args(0);
    running = false;
    return 0;
}

static int hdmi_mod_is_running(lua_State *l) {
    lua_check_num_args(0);
    lua_pushboolean(l, running);
    return 1;
}

static int hdmi_mod_set_scale(lua_State *l) {
    lua_check_num_args(2);
    scale_x = luaL_checkinteger(l, 1);
    scale_y = luaL_checkinteger(l, 2);
    offset_y = (output_height - (64 * scale_y)) / 2;
    MSG("scaling updated: " << scale_x << "x" << scale_y << ", offset_y: " << offset_y);
    return 0;
}

//
// module definition
//

static const luaL_Reg mod[] = {
    {NULL, NULL}
};

static luaL_Reg func[] = {
    {"init", hdmi_mod_init},
    {"cleanup", hdmi_mod_cleanup},
    {"update", hdmi_mod_update},
    {"start", hdmi_mod_start},
    {"stop", hdmi_mod_stop},
    {"is_running", hdmi_mod_is_running},
    {"set_scale", hdmi_mod_set_scale},
    // Screen drawing wrapper functions
    {"clear", hdmi_mod_clear},
    {"move", hdmi_mod_move},
    {"line", hdmi_mod_line},
    {"rect", hdmi_mod_rect},
    {"stroke", hdmi_mod_stroke},
    {"fill", hdmi_mod_fill},
    {"level", hdmi_mod_level},
    {"line_width", hdmi_mod_line_width},
    // Font and text functions
    {"font_face", hdmi_mod_font_face},
    {"font_size", hdmi_mod_font_size},
    {"text", hdmi_mod_text},
    {"text_center", hdmi_mod_text_center},
    {"text_right", hdmi_mod_text_right},
    {NULL, NULL}
};

HDMI_MOD_API int luaopen_hdmi_mod(lua_State *L) {
    lua_newtable(L);

    for (int i = 0; mod[i].name; i++) {
        mod[i].func(L);
    }

    luaL_setfuncs(L, func, 0);

    lua_pushstring(L, "VERSION");
    lua_pushstring(L, HDMI_MOD_VERSION);
    lua_rawset(L, -3);

    return 1;
}
