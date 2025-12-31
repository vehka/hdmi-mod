#ifndef __HDMI_MOD_H__
#define __HDMI_MOD_H__

#define HDMI_MOD_VERSION "0.1"

#ifndef HDMI_MOD_API
#define HDMI_MOD_API __attribute__ ((visibility ("default")))
#endif

extern "C" {

#include "lua.h"
#include "lauxlib.h"

HDMI_MOD_API int luaopen_hdmi_mod(lua_State *L);

} // extern "C"

#endif
