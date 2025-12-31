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

std::map<cairo_surface_t*, FramebufferInfo*> surface_fb_map;

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

int create_framebuffer_output(cairo_surface_t* surface, const char* device_path) {
    auto it = surface_fb_map.find(surface);
    if (it != surface_fb_map.end()) {
        MSG("a framebuffer output already exists for this surface");
        return 0;
    }

    // Open framebuffer device
    int fd = open(device_path, O_RDWR);
    if (fd < 0) {
        MSG("error opening framebuffer device " << device_path << ": " << strerror(errno));
        return 0;
    }

    // Get fixed screen information
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        MSG("error reading fixed screen info: " << strerror(errno));
        close(fd);
        return 0;
    }

    // Get variable screen information
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        MSG("error reading variable screen info: " << strerror(errno));
        close(fd);
        return 0;
    }

    // Store framebuffer info
    FramebufferInfo* fbinfo = new FramebufferInfo();
    fbinfo->fd = fd;
    fbinfo->width = vinfo.xres;
    fbinfo->height = vinfo.yres;
    fbinfo->bpp = vinfo.bits_per_pixel;
    fbinfo->line_length = finfo.line_length;
    fbinfo->screen_size = finfo.smem_len;

    // Memory map the framebuffer
    fbinfo->mapped_mem = (uint8_t*)mmap(0, fbinfo->screen_size,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0);

    if (fbinfo->mapped_mem == MAP_FAILED) {
        MSG("error mapping framebuffer: " << strerror(errno));
        close(fd);
        delete fbinfo;
        return 0;
    }

    surface_fb_map[surface] = fbinfo;

    MSG("framebuffer output created: " << fbinfo->width << "x" << fbinfo->height
        << " @ " << fbinfo->bpp << "bpp (device: " << device_path << ")");

    // Update scaling based on actual resolution
    output_width = fbinfo->width;
    output_height = fbinfo->height;
    scale_x = output_width / 128;
    scale_y = scale_x; // Keep square pixels
    offset_y = (output_height - (64 * scale_y)) / 2;

    MSG("scaling: " << scale_x << "x" << scale_y << ", offset_y: " << offset_y);

    return 0;
}

int destroy_framebuffer_output(cairo_surface_t* surface) {
    auto it = surface_fb_map.find(surface);
    if (it == surface_fb_map.end()) {
        MSG("No framebuffer output exists for this surface");
        return 0;
    }

    FramebufferInfo* fbinfo = it->second;

    if (fbinfo->mapped_mem != MAP_FAILED) {
        munmap(fbinfo->mapped_mem, fbinfo->screen_size);
    }

    if (fbinfo->fd >= 0) {
        close(fbinfo->fd);
    }

    delete fbinfo;
    surface_fb_map.erase(it);

    MSG("framebuffer output destroyed");
    return 0;
}

int initialize_hdmi() {
    if (!initialized && !failed) {
        MSG("HDMI output service initializing");
        initialized = true;

        // create the default framebuffer output
        cairo_t* ctx = (cairo_t*)screen_context_get_current();
        if (ctx == NULL) {
            MSG("failed to get screen context");
            failed = true;
            return 0;
        }

        cairo_surface_t* surface = cairo_get_target(ctx);

        // Try /dev/fb0 first, then /dev/fb1
        int result = create_framebuffer_output(surface, "/dev/fb0");
        if (surface_fb_map.find(surface) == surface_fb_map.end()) {
            result = create_framebuffer_output(surface, "/dev/fb1");
        }

        if (surface_fb_map.find(surface) == surface_fb_map.end()) {
            MSG("failed to open any framebuffer device");
            failed = true;
            initialized = false;
            return 0;
        }

        MSG("HDMI output service initialized");
    }
    return 0;
}

int cleanup_hdmi() {
    running = false;
    if (initialized) {
        initialized = false;

        for (auto& kv : surface_fb_map) {
            FramebufferInfo* fbinfo = kv.second;
            if (fbinfo->mapped_mem != MAP_FAILED) {
                munmap(fbinfo->mapped_mem, fbinfo->screen_size);
            }
            if (fbinfo->fd >= 0) {
                close(fbinfo->fd);
            }
            delete fbinfo;
        }
        surface_fb_map.clear();

        MSG("HDMI output service stopped");
    }
    return 0;
}

int send_surface_to_framebuffer(cairo_surface_t* surface)
{
    if (initialized && !failed) {
        // locate the framebuffer output
        auto it = surface_fb_map.find(surface);
        if (it == surface_fb_map.end()) {
            // no framebuffer output registered for this surface
            return 0;
        }
        FramebufferInfo* fbinfo = it->second;

        // prepare the surface
        if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
            cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            return 0;
        }
        cairo_surface_flush(surface);

        // get surface data and scale/copy to framebuffer
        unsigned char* data = cairo_image_surface_get_data(surface);
        if (data != NULL) {
            int src_width = cairo_image_surface_get_width(surface);
            int src_height = cairo_image_surface_get_height(surface);
            int src_stride = cairo_image_surface_get_stride(surface);

            scale_and_copy_buffer(data, src_width, src_height, src_stride,
                                fbinfo->mapped_mem, fbinfo->width, fbinfo->height,
                                fbinfo->line_length);
        }
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
        cairo_t* ctx = (cairo_t*)screen_context_get_current();
        if (ctx == NULL) {
            return 0;
        }
        cairo_surface_t* surface = cairo_get_target(ctx);
        return send_surface_to_framebuffer(surface);
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

static int hdmi_mod_create_image_output(lua_State *l) {
    lua_check_num_args(2);
    _image_t *i = _image_check(l, 1);
    const char *device = luaL_checkstring(l, 2);

    if (i->surface != NULL) {
        return create_framebuffer_output((cairo_surface_t*)i->surface, device);
    }
    return 0;
}

static int hdmi_mod_destroy_image_output(lua_State *l) {
    lua_check_num_args(1);
    _image_t *i = _image_check(l, 1);

    if (i->surface != NULL) {
        return destroy_framebuffer_output((cairo_surface_t*)i->surface);
    }
    return 0;
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
    {"create_image_output", hdmi_mod_create_image_output},
    {"destroy_image_output", hdmi_mod_destroy_image_output},
    {"set_scale", hdmi_mod_set_scale},
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
