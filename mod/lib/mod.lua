local mod = require 'core/mods'

local this_name = mod.this_name

mod.hook.register("system_post_startup", "hdmi", function()
  package.cpath = package.cpath .. ";" .. paths.code .. this_name .. "/lib/?.so"
  hdmi_mod = require 'hdmi_mod'

  -- replace the default update function
  screen.update_default = function()
    _norns.screen_update()
    hdmi_mod.update()
  end

  -- patch screensaver metro event handler to continue
  -- updating HDMI output after screensaver activates
  local original_ss_event = metro[36].event
  metro[36].event = function()
    original_ss_event()
    screen.update = function()
      hdmi_mod.update()
    end
  end

  -- delay initialization until the first screen update
  -- to ensure framebuffer device is ready
  screen.update = function()
    hdmi_mod.init()
    hdmi_mod.start()
    screen.update = screen.update_default
    screen.update()
  end

end)

mod.hook.register("system_pre_shutdown", "hdmi", function()
  hdmi_mod.cleanup()
end)
