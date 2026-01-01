local mod = require 'core/mods'

local this_name = mod.this_name

-- Console mode state
local hdmi_output_mode = "norns_screen"  -- or "console"
local keyboard_target = "norns"           -- or "console"
local original_keyboard_code = nil
local original_keyboard_char = nil

mod.hook.register("system_post_startup", "hdmi", function()
  package.cpath = package.cpath .. ";" .. paths.code .. this_name .. "/lib/?.so"
  hdmi_mod = require 'hdmi_mod'

  -- Save original screen functions
  local original_screen = {
    clear = screen.clear,
    move = screen.move,
    line = screen.line,
    line_rel = screen.line_rel,
    aa = screen.aa,
    rect = screen.rect,
    stroke = screen.stroke,
    fill = screen.fill,
    level = screen.level,
    line_width = screen.line_width,
    font_face = screen.font_face,
    font_size = screen.font_size,
    text = screen.text,
    text_center = screen.text_center,
    text_right = screen.text_right,
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

  screen.line_rel = function(x, y)
    original_screen.line_rel(x, y)
    hdmi_mod.line_rel(x, y)
  end

  screen.aa = function(state)
    original_screen.aa(state)
    hdmi_mod.aa(state)
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

  screen.font_face = function(index)
    original_screen.font_face(index)
    hdmi_mod.font_face(index)
  end

  screen.font_size = function(size)
    original_screen.font_size(size)
    hdmi_mod.font_size(size)
  end

  screen.text = function(str)
    original_screen.text(str)
    hdmi_mod.text(str)
  end

  screen.text_center = function(str)
    original_screen.text_center(str)
    hdmi_mod.text_center(str)
  end

  screen.text_right = function(str)
    original_screen.text_right(str)
    hdmi_mod.text_right(str)
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

  -- Console mode functions
  local function switch_hdmi_mode(mode)
    if mode == "console" then
      -- Stop norns screen mirroring
      hdmi_mod.stop()
      -- Map console 1 (tty1) to framebuffer 1 (HDMI)
      local success = hdmi_mod.map_console_to_fb(1, 1)
      if success then
        print("hdmi-mod: HDMI switched to console mode")
      else
        print("hdmi-mod: Failed to map console to HDMI")
      end
    else  -- norns_screen
      -- Map console back to fb0 (norns OLED)
      hdmi_mod.map_console_to_fb(1, 0)
      -- Resume norns screen mirroring
      hdmi_mod.start()
      print("hdmi-mod: HDMI switched to norns screen mode")
    end
  end

  local function switch_keyboard_mode(mode)
    if mode == "console" then
      -- Open TTY for injection
      local success = hdmi_mod.open_tty("/dev/tty1")
      if success then
        print("hdmi-mod: Keyboard routed to console")
      else
        print("hdmi-mod: Failed to open /dev/tty1")
      end
    else  -- norns
      -- Close TTY
      hdmi_mod.close_tty()
      print("hdmi-mod: Keyboard routed to norns")
    end
  end

  -- Map special keys to TTY control codes
  local special_keys = {
    ENTER = "\n",
    BACKSPACE = "\x7f",  -- DEL character
    TAB = "\t",
    ESC = "\x1b",
    -- Arrow keys (ANSI escape sequences)
    UP = "\x1b[A",
    DOWN = "\x1b[B",
    RIGHT = "\x1b[C",
    LEFT = "\x1b[D",
    -- Control sequences
    -- CTRL+C is handled via keyboard state
  }

  local function inject_special_key(key_code)
    local char = special_keys[key_code]
    if char then
      -- For multi-character sequences (like arrow keys), inject each char
      for i = 1, #char do
        hdmi_mod.inject_tty_char(char:sub(i, i))
      end
      return true
    end
    return false
  end

  -- Convert Ctrl+key to control codes
  local function handle_ctrl_key(key_code)
    -- Ctrl+A through Ctrl+Z map to ASCII 1-26
    -- Check if it's a letter key (A-Z)
    local letter_keys = {
      A=1, B=2, C=3, D=4, E=5, F=6, G=7, H=8, I=9, J=10,
      K=11, L=12, M=13, N=14, O=15, P=16, Q=17, R=18, S=19,
      T=20, U=21, V=22, W=23, X=24, Y=25, Z=26
    }

    local ctrl_code = letter_keys[key_code]
    if ctrl_code then
      -- Inject the control character
      hdmi_mod.inject_tty_char(string.char(ctrl_code))
      return true
    end
    return false
  end

  -- Save original keyboard handlers
  original_keyboard_code = keyboard.code
  original_keyboard_char = keyboard.char

  -- Override keyboard handlers for routing
  keyboard.code = function(code, value)
    if keyboard_target == "console" then
      -- Only process key press (value == 1), ignore release (value == 0)
      if value == 1 then
        -- Check for Ctrl+key combinations first
        if keyboard.state["LEFTCTRL"] or keyboard.state["RIGHTCTRL"] then
          -- Handle Ctrl+letter (Ctrl+R=0x12, Ctrl+Z=0x1A, etc.)
          if handle_ctrl_key(code) then
            return  -- Handled, don't process further
          end
        end

        -- Handle special keys (Enter, arrows, etc.)
        if inject_special_key(code) then
          return  -- Handled
        end

        -- Regular printable characters will be handled by keyboard.char
      end
    else
      -- Route to norns
      if original_keyboard_code then
        original_keyboard_code(code, value)
      end
    end
  end

  keyboard.char = function(char)
    if keyboard_target == "console" then
      -- Inject character into TTY
      hdmi_mod.inject_tty_char(char)
    else
      -- Route to norns
      if original_keyboard_char then
        original_keyboard_char(char)
      end
    end
  end

  -- Add mod parameters
  params:add_group("HDMI", 2)

  params:add{
    type = "option",
    id = "hdmi_output_mode",
    name = "HDMI Output",
    options = {"norns screen", "console"},
    default = 1,
    action = function(value)
      hdmi_output_mode = (value == 1) and "norns_screen" or "console"
      switch_hdmi_mode(hdmi_output_mode)
    end
  }

  params:add{
    type = "option",
    id = "hdmi_keyboard_target",
    name = "Keyboard Target",
    options = {"norns", "console"},
    default = 1,
    action = function(value)
      keyboard_target = (value == 1) and "norns" or "console"
      switch_keyboard_mode(keyboard_target)
    end
  }

end)

mod.hook.register("system_pre_shutdown", "hdmi", function()
  hdmi_mod.cleanup()
end)
