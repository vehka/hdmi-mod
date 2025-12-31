# hdmi-mod for norns

A [norns](https://monome.org/norns) mod to output the contents of the LCD screen to an HDMI display in real-time via the Raspberry Pi's framebuffer device.

This mod is adapted from [ndi-mod](https://github.com/Dewb/ndi-mod) by replacing network streaming with direct HDMI output.

## How it works

The mod intercepts norns' screen updates and writes the scaled framebuffer directly to the HDMI output device (`/dev/fb0` or `/dev/fb1`). The 128x64 pixel norns screen is scaled up to fit common HDMI resolutions (1920x1080, etc.) using nearest-neighbor scaling.

## Requirements

- norns (shield or factory) with HDMI output capability
- Raspberry Pi CM3+ or similar with HDMI port
- HDMI display connected at boot time

## Installation

### From source

1. Clone or download this repository to your development machine
2. Build the mod:
   ```bash
   ./build-hdmi.sh
   ```
3. Copy to norns:
   ```bash
   scp -r build-hdmi/hdmi-mod we@norns.local:/home/we/dust/code/
   ```

### Enabling the mod

1. On norns, navigate to **SYSTEM > MODS**
2. Scroll to **HDMI-MOD** and turn enc 3 clockwise to enable (`+`)
3. Navigate to **SYSTEM > RESTART** to activate

## Configuration

The mod automatically detects the framebuffer resolution and scales the norns display accordingly.

### Custom scaling

You can adjust scaling from a norns script:

```lua
if hdmi_mod then
  hdmi_mod.set_scale(10, 10)  -- 10x horizontal, 10x vertical
end
```

### Framebuffer setup (Raspberry Pi)

Ensure your Pi is configured to output to HDMI:

1. Edit `/boot/config.txt` and ensure HDMI is enabled:
   ```
   hdmi_force_hotplug=1
   hdmi_drive=2
   ```

2. For specific resolutions, you can add:
   ```
   hdmi_group=1
   hdmi_mode=16  # 1920x1080 @ 60Hz
   ```

3. For 32-bit color depth (recommended), edit `/boot/cmdline.txt` and add:
   ```
   video=HDMI-A-1:1920x1080@60,margin_left=0,margin_right=0,margin_top=0,margin_bottom=0
   ```

## Features

- **Real-time output**: Screen updates appear on HDMI immediately
- **Low overhead**: Direct framebuffer writing is very efficient
- **Automatic scaling**: Adapts to your display resolution
- **Screensaver support**: Continues updating even when screensaver is active

## Performance

The mod adds minimal CPU overhead (typically <1%) since it's writing directly to the framebuffer without encoding or network transmission.

## Scripting API

### Core functions

```lua
hdmi_mod.init()              -- Initialize HDMI output
hdmi_mod.start()             -- Start sending frames
hdmi_mod.stop()              -- Stop sending frames
hdmi_mod.cleanup()           -- Clean up resources
hdmi_mod.is_running()        -- Check if running
```

### Scaling

```lua
hdmi_mod.set_scale(x, y)     -- Set horizontal and vertical scale factors
```

### Advanced: Multiple outputs

You can create additional outputs for offscreen images:

```lua
local img = screen.create_image(256, 128)
hdmi_mod.create_image_output(img, "/dev/fb1")
-- ... render to img ...
hdmi_mod.destroy_image_output(img)
```

## Troubleshooting

### No output on HDMI

1. Check that HDMI display is connected before boot
2. Verify framebuffer devices exist: `ls -l /dev/fb*`
3. Check mod logs in maiden for error messages
4. Ensure `hdmi_force_hotplug=1` is set in `/boot/config.txt`

### Display is garbled

1. Try different color depth settings in `/boot/cmdline.txt`
2. Adjust scaling with `hdmi_mod.set_scale(x, y)`
3. Verify your HDMI display supports the configured resolution

### Performance issues

The mod is designed to be lightweight, but if you experience issues:
1. Reduce scaling factor
2. Lower HDMI resolution in `/boot/config.txt`
3. Check CPU usage in **SYSTEM > STATS**

## Technical details

### Architecture

- **Capture**: Reads Cairo surface from norns screen context (128x64 ARGB32)
- **Scale**: Nearest-neighbor upscaling to match framebuffer resolution
- **Output**: Direct memory-mapped write to `/dev/fb0` or `/dev/fb1`

### Scaling algorithm

The mod uses simple nearest-neighbor scaling to maintain the pixelated aesthetic of the norns display. The output is centered with letterboxing/pillarboxing as needed.

### Differences from ndi-mod

- No network streaming (HDMI is local only)
- No external dependencies (no NDI SDK)
- Simpler codebase (~400 lines vs ~250 lines)
- Lower latency (no network encoding/decoding)

## Building from source

Requirements:
- CMake 3.2+
- C++ compiler with C++11 support
- lua-5.3 development headers
- Cairo development headers
- norns source (linked as `dep/norns`)

```bash
./build-hdmi.sh
```

This creates:
- `build-hdmi/hdmi-mod/` - mod directory
- `build-hdmi/hdmi-mod.zip` - distribution package

## License

Licensed under [The MIT License](https://mit-license.org/). This software is provided as-is, without warranty of any kind, use at your own risk.

Based on [ndi-mod](https://github.com/Dewb/ndi-mod) by Dewb.

## Future enhancements

Potential improvements:
- DRM/KMS API support for more control
- Hardware-accelerated scaling
- Multiple HDMI output support
- Custom color palettes/filters
- Rotation support
