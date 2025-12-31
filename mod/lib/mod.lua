local mod = require 'core/mods'

local this_name = mod.this_name

mod.hook.register("system_post_startup", "hdmi", function()
  package.cpath = package.cpath .. ";" .. paths.code .. this_name .. "/lib/?.so"
  hdmi_mod = require 'hdmi_mod'

  -- Save original screen functions
  local original_screen = {
    clear = screen.clear,
    move = screen.move,
    line = screen.line,
    rect = screen.rect,
    stroke = screen.stroke,
    fill = screen.fill,
    level = screen.level,
    line_width = screen.line_width,
    update = screen.update
  }

  -- Wrap screen functions to mirror drawing commands
  screen.clear = function()
    original_screen.clear()
    hdmi_mod.clear()
  end

  screen.move = function(x, y)
    original_screen.move(x, y)
    hdmi_mod.move(x, y)
  end

  screen.line = function(x, y)
    original_screen.line(x, y)
    hdmi_mod.line(x, y)
  end

  screen.rect = function(x, y, w, h)
    original_screen.rect(x, y, w, h)
    hdmi_mod.rect(x, y, w, h)
  end

  screen.stroke = function()
    original_screen.stroke()
    hdmi_mod.stroke()
  end

  screen.fill = function()
    original_screen.fill()
    hdmi_mod.fill()
  end

  screen.level = function(level)
    original_screen.level(level)
    hdmi_mod.level(level)
  end

  screen.line_width = function(width)
    original_screen.line_width(width)
    hdmi_mod.line_width(width)
  end

  -- Update function: update norns screen, then mirror to HDMI
  screen.update_default = function()
    _norns.screen_update()
    hdmi_mod.update()
  end

  -- Patch screensaver metro event handler to continue
  -- updating HDMI output after screensaver activates
  local original_ss_event = metro[36].event
  metro[36].event = function()
    original_ss_event()
    screen.update = function()
      hdmi_mod.update()
    end
  end

  -- Delay initialization until the first screen update
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
