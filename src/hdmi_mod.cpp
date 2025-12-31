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

int initialize_hdmi() {
    if (!initialized && !failed) {
        MSG("HDMI output service initializing");

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
static cairo_font_face_t* current_font_face = NULL;

static int hdmi_mod_font_face(lua_State *l) {
    lua_check_num_args(1);
    int font_index = luaL_checkinteger(l, 1);

    if (mirror_ctx != NULL) {
        // Call the norns screen function to get the font
        // This is a bit of a hack - we rely on norns having loaded the fonts
        screen_font_face(font_index);

        // For now, just use cairo's default font
        // TODO: Load actual norns fonts if needed
        cairo_select_font_face(mirror_ctx, "monospace",
                             CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
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
