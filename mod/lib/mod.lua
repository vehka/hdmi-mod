local mod = require 'core/mods'

local this_name = mod.this_name

-- Settings file path
local settings_path = _path.data .. "hdmi_mod_settings.lua"

-- Console mode state
local hdmi_output_mode = "norns_screen"  -- or "console"
local keyboard_target = "norns"           -- or "console"
local original_keyboard_code = nil
local original_keyboard_char = nil

-- Settings persistence
local function save_settings()
  local f = io.open(settings_path, "w")
  if not f then return end
  io.output(f)
  io.write("return {\n")
  io.write("  hdmi_output_mode = '" .. hdmi_output_mode .. "',\n")
  io.write("  keyboard_target = '" .. keyboard_target .. "',\n")
  io.write("}\n")
  io.close(f)
  print("hdmi-mod: settings saved")
end

local function load_settings()
  local f = io.open(settings_path, "r")
  if f then
    io.close(f)
    local settings = dofile(settings_path)
    hdmi_output_mode = settings.hdmi_output_mode or "norns_screen"
    keyboard_target = settings.keyboard_target or "norns"
    print("hdmi-mod: settings loaded")
    return settings
  end
  return nil
end

-- Forward declare console mode functions (defined after hook)
local switch_hdmi_mode
local switch_keyboard_mode

mod.hook.register("system_post_startup", "hdmi", function()
  package.cpath = package.cpath .. ";" .. paths.code .. this_name .. "/lib/?.so"
  hdmi_mod = require 'hdmi_mod'

  -- Load saved settings
  load_settings()

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

    -- Apply saved settings after initialization
    if hdmi_output_mode == "console" then
      switch_hdmi_mode("console")
    end
    if keyboard_target == "console" then
      switch_keyboard_mode("console")
    end

    screen.update()
  end

  -- Define console mode functions
  switch_hdmi_mode = function(mode)
    if mode == "console" then
      -- Stop norns screen mirroring
      hdmi_mod.stop()
      -- Clear framebuffer
      hdmi_mod.clear_framebuffer()
      -- Map console 2 (tty2) to framebuffer 1 (HDMI)
      local success = hdmi_mod.map_console_to_fb(2, 1)
      if success then
        -- Write test message to tty2 to verify it's working
        os.execute("echo '\n\nHDMI Console Active (tty2)\nLogin: we\n' > /dev/tty2")

        -- Try to start getty on tty2 if not already running
        os.execute("(ps aux | grep -q '[a]getty.*tty2') || openvt -c 2 -s -w -- login -f we &")

        -- Switch to tty2 to activate console on HDMI
        os.execute("chvt 2")
        print("hdmi-mod: HDMI switched to console mode (tty2)")
      else
        print("hdmi-mod: Failed to map console to HDMI")
      end
    else  -- norns_screen
      -- Switch back to tty1
      os.execute("chvt 1")
      -- Map console back to fb0 (norns OLED)
      hdmi_mod.map_console_to_fb(2, 0)
      -- Resume norns screen mirroring
      hdmi_mod.start()
      print("hdmi-mod: HDMI switched to norns screen mode")
    end
  end

  -- Map norns keyboard layout to console keymap
  local function apply_console_keymap()
    -- Get current norns keyboard layout - try different methods
    local norns_layout = keyboard.layout or keyboard.active_layout or "US"

    -- Debug: print what layout we detected
    print("hdmi-mod: Detected norns keyboard layout: " .. tostring(norns_layout))

    -- Map norns layout names to console keymap names
    local keymap_mapping = {
      US = "us",
      FR = "fr",
      DE = "de",
      UK = "uk",
      ES = "es",
      IT = "it",
      PT = "pt",
      SE = "se",
      NO = "no",
      DK = "dk",
      FI = "fi-latin1",  -- fi-latin1 is more common on Linux
      NL = "nl",
      BE = "be",
      CH = "ch",
      AT = "de",
      PL = "pl",
      CZ = "cz",
      RU = "ru",
      JP = "jp106",
      KR = "kr",
    }

    local console_keymap = keymap_mapping[norns_layout:upper()] or "us"

    print("hdmi-mod: Applying console keymap: " .. console_keymap)

    -- Load the keymap - use proper syntax for specific tty
    local cmd = "loadkeys " .. console_keymap .. " 2>&1"
    local handle = io.popen(cmd)
    local result = handle:read("*a")
    handle:close()

    if result and result ~= "" then
      print("hdmi-mod: loadkeys output: " .. result)
    end

    print("hdmi-mod: Console keymap applied")
  end

  switch_keyboard_mode = function(mode)
    if mode == "console" then
      -- Open TTY for injection (use tty2)
      local success = hdmi_mod.open_tty("/dev/tty2")
      if success then
        -- Apply norns keyboard layout to console
        apply_console_keymap()
        print("hdmi-mod: Keyboard routed to console (tty2)")
      else
        print("hdmi-mod: Failed to open /dev/tty2")
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

end)

-- Menu UI object
local menu_ui = {
  selected = 1,
  settings = {
    {id = "hdmi_output", name = "HDMI Output", type = "option",
     options = {"norns screen", "console"}, value = 1},
    {id = "keyboard_target", name = "Keyboard Target", type = "option",
     options = {"norns", "console"}, value = 1},
  }
}

menu_ui.init = function()
  -- Update menu values from current settings
  for _, setting in ipairs(menu_ui.settings) do
    if setting.id == "hdmi_output" then
      setting.value = (hdmi_output_mode == "norns_screen") and 1 or 2
    elseif setting.id == "keyboard_target" then
      setting.value = (keyboard_target == "norns") and 1 or 2
    end
  end
end

menu_ui.deinit = function()
  -- Called when menu is closed
end

menu_ui.key = function(n, z)
  if z == 1 then  -- Key press
    if n == 2 then
      -- K2: Exit menu
      mod.menu.exit()
    elseif n == 3 then
      -- K3: Toggle selected setting (not used, E3 handles this)
    end
  end
end

menu_ui.enc = function(n, delta)
  if n == 2 then
    -- E2: Navigate settings
    menu_ui.selected = util.clamp(menu_ui.selected + delta, 1, #menu_ui.settings)
  elseif n == 3 then
    -- E3: Change setting value
    local setting = menu_ui.settings[menu_ui.selected]
    if setting.type == "option" then
      setting.value = util.clamp(setting.value + delta, 1, #setting.options)

      -- Apply setting change
      if setting.id == "hdmi_output" then
        hdmi_output_mode = (setting.value == 1) and "norns_screen" or "console"
        switch_hdmi_mode(hdmi_output_mode)
        save_settings()
      elseif setting.id == "keyboard_target" then
        keyboard_target = (setting.value == 1) and "norns" or "console"
        switch_keyboard_mode(keyboard_target)
        save_settings()
      end
    end
  end
  mod.menu.redraw()
end

menu_ui.redraw = function()
  screen.clear()

  -- Title
  screen.level(15)
  screen.move(0, 10)
  screen.text("HDMI Settings")

  -- Draw settings
  local y = 25
  for i, setting in ipairs(menu_ui.settings) do
    local is_selected = (i == menu_ui.selected)
    screen.level(is_selected and 15 or 4)

    -- Setting name
    screen.move(0, y)
    screen.text(setting.name)

    -- Setting value
    if setting.type == "option" then
      local value_text = setting.options[setting.value]
      screen.move(127, y)
      screen.text_right(value_text)
    end

    y = y + 12
  end

  screen.update()
end

-- Register menu UI
mod.menu.register(mod.this_name, menu_ui)

mod.hook.register("system_pre_shutdown", "hdmi", function()
  hdmi_mod.cleanup()
end)
