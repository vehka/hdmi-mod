# HDMI-MOD Implementation TODO

## Overview

This mod uses the **Lua proxy/mirror approach** to replicate norns screen output to HDMI. Instead of trying to access norns' internal cairo surface (which requires APIs not available in all norns versions), we:

1. Create our own cairo surface (128x64) in C
2. Intercept all Lua `screen.*` drawing calls
3. Replay each drawing command to both the norns screen AND our mirror surface
4. Copy our mirror surface to the HDMI framebuffer on each update

This approach is more compatible across norns versions but requires implementing wrappers for all screen API functions.

## Branch Information

- **Branch**: `mirror-surface-proxy`
- **Approach**: Lua proxy pattern with mirrored cairo surface
- **Why**: The direct surface access approach fails on norns versions where `screen_context_get_current()` returns void and `screen_context_get_primary()` doesn't exist

## Implementation Status

### ✅ Core Functions (Implemented)

#### Drawing
- `clear()` - Clear screen
- `move(x, y)` - Move drawing position
- `line(x, y)` - Draw line to point
- `rect(x, y, w, h)` - Draw rectangle
- `stroke()` - Stroke current path
- `fill()` - Fill current path
- `level(value)` - Set brightness (0-15)
- `line_width(w)` - Set line width

#### Text & Fonts
- `text(str)` - Draw left-aligned text
- `text_center(str)` - Draw center-aligned text
- `text_right(str)` - Draw right-aligned text
- `font_size(size)` - Set font size
- `font_face(index)` - Set font face (**INCOMPLETE**: currently uses cairo default "monospace" instead of actual norns fonts)

#### System
- `init()` - Initialize HDMI output
- `cleanup()` - Cleanup resources
- `update()` - Copy mirror surface to HDMI framebuffer
- `start()` / `stop()` - Control output
- `is_running()` - Check if running
- `set_scale(x, y)` - Set scaling factors

### ⏳ Drawing Functions (TODO)

#### Line/Path Styling
- `aa(state)` - Enable/disable anti-aliasing
- `line_cap(style)` - Set line cap style ("butt", "round", "square")
- `line_join(style)` - Set line join style ("miter", "round", "bevel")
- `miter_limit(limit)` - Set miter limit

#### Relative Movement
- `move_rel(x, y)` - Move relative to current position
- `line_rel(x, y)` - Draw line relative to current position

#### Shapes
- `arc(x, y, r, angle1, angle2)` - Draw arc
- `circle(x, y, r)` - Draw circle
- `curve(x1, y1, x2, y2, x3, y3)` - Draw cubic Bézier curve
- `curve_rel(x1, y1, x2, y2, x3, y3)` - Draw curve with relative coords
- `pixel(x, y)` - Draw single pixel

#### Path Operations
- `close()` - Close current path

### ⏳ Text Functions (TODO)

- `text_trim(str, w)` - Draw text trimmed to width
- `text_rotate(x, y, str, degrees)` - Draw rotated text
- `text_center_rotate(x, y, str, degrees)` - Draw center-aligned rotated text
- `text_extents(str)` - Calculate text width (may not need mirroring - just returns data)

### ⏳ Transform Functions (TODO)

- `rotate(r)` - Rotate by r radians
- `translate(x, y)` - Move origin position
- `save()` - Save drawing state
- `restore()` - Restore drawing state (exists in C API but not in our impl)

### ⏳ Blending (TODO)

- `blend_mode(index)` - Set cairo blending operator
  - Cairo supports many blend modes (Over, XOR, Add, Multiply, Screen, Overlay, etc.)
  - See: https://www.cairographics.org/operators/

### 🔍 Image Functions (Special Handling Required)

These functions work with image buffers and require special consideration:

#### Display Functions
- `display_png(filename, x, y)` - Display PNG file
- `display_image(image, x, y)` - Display image buffer
- `display_image_region(image, left, top, width, height, x, y)` - Display image region

**Implementation approach:**
- These functions display images that are **external to the drawing commands**
- We need to intercept the image display and replicate it on our mirror surface
- May require accessing the image buffer data or using cairo's image surface functions
- Priority: MEDIUM (many scripts don't use images)

#### Image Buffer Management
- `load_png(filename)` - Load PNG into buffer (returns image object)
- `create_image(width, height)` - Create image buffer
- `draw_to(image, func)` - Direct drawing to image buffer instead of screen

**Implementation approach:**
- `load_png` and `create_image` return image objects
- We may need to track these image objects and create corresponding mirror images
- `draw_to` temporarily redirects drawing - we'd need to redirect our mirror context too
- This is complex and may not be necessary for basic screen mirroring
- Priority: LOW (advanced feature, not commonly used)

#### Pixel Access
- `peek(x, y, w, h)` - Read pixel data from screen
- `poke(x, y, w, h, s)` - Write pixel data to screen

**Implementation approach:**
- These directly manipulate pixel buffers
- `peek` reads from screen - no mirroring needed
- `poke` writes to screen - we need to replicate to our mirror surface
- Can be implemented using cairo's pixel access functions
- Priority: MEDIUM (some scripts use this for effects)

### 🔧 Font Loading (HIGH PRIORITY)

**Current issue**: Using cairo default "monospace" font instead of actual norns fonts

**Solution needed**:
1. Load TTF fonts from norns resources: `/home/we/norns/resources/`
2. Map norns font indices (1-69) to actual font files
3. Use `cairo_ft_font_face_create_for_ft_face()` to load TTF fonts
4. Store font faces and switch between them based on `font_face()` calls

**Key norns fonts**:
- Index 1: norns.ttf (default)
- Index 2-14: Roboto family
- Index 15-24: Vera family
- Index 25-67: Various bitmap/fixed fonts
- Index 68: Particle
- Index 69: 04B_03 (aliased to norns.ttf)

**Implementation priority**: HIGH - text looks wrong without proper fonts

## Architecture Notes

### Why This Approach?

The original approach tried to access norns' internal cairo surface via:
1. `screen_context_get_current()` - returns `void` (async, posts to Lua)
2. `screen_context_get_primary()` - doesn't exist in older norns versions
3. `extern cairo_t *cr_primary` - not exported as a symbol

None of these work, so we use the proxy pattern instead.

### Performance Considerations

- Every drawing command is executed twice (norns screen + mirror)
- This is acceptable for 60fps norns scripts
- The scaling and framebuffer copy happens only once per `screen.update()`
- Most overhead is in the framebuffer copy, not the cairo operations

### Compatibility

This approach should work across all norns versions since:
- It doesn't rely on internal matron APIs
- It only uses standard Lua function wrapping
- The cairo operations are standard and well-supported

### Future: If Upstream Fix Happens

If norns gets a proper API to access the screen surface (e.g., `screen_context_get_primary()` returns a value), we could:
1. Switch back to direct surface access approach (simpler)
2. Remove all the wrapper functions
3. Just copy norns' surface to framebuffer on update

Keep this branch (`mirror-surface-proxy`) as a fallback for older norns versions.

## Testing Checklist

- [ ] Basic shapes (lines, rectangles, circles, arcs)
- [ ] Text with different fonts and sizes
- [ ] Text alignment (left, center, right)
- [ ] Rotated text
- [ ] Transform operations (rotate, translate)
- [ ] Path operations (curves, close)
- [ ] Blend modes
- [ ] Image display
- [ ] Pixel operations (poke)
- [ ] Multiple concurrent scripts
- [ ] Screen saver behavior
- [ ] Different HDMI resolutions

## Known Limitations

1. **Fonts**: Currently using monospace instead of norns fonts
2. **Images**: Not yet implemented - scripts using images won't display them on HDMI
3. **Direct C calls**: Scripts that bypass Lua screen API (use `_norns.*` directly) won't be mirrored
4. **Performance**: Slight overhead from double drawing (should be negligible)

## Development Workflow

1. Implement missing function in C (hdmi_mod.cpp)
2. Add to function registration table
3. Create Lua wrapper in mod.lua
4. Test with norns script that uses the function
5. Update this TODO.md
