# assets/
App icon, tray glyphs and any images bundled with the app. Fonts come from the system (Segoe UI).

## Icon
The icon is an isometric "module cube" filling a hexagon. Its faces show the three module kinds:
orbit (top), equalizer bars (left) and spectrogram (right). It contains no text, so it does not
depend on the product name.

| File | Use |
| --- | --- |
| `app.ico` | The exe, window and taskbar icon (`app/app.rc`). Cube seams match a dark surface. |
| `icon/icon-on-dark.svg`, `.png` | Master for dark backgrounds. |
| `icon/icon-on-light.svg`, `.png` | Same icon with seams that match a light background (docs, website). |
| `icon/app-on-light.ico` | Light-seam `.ico`, not used by the app yet. |
| `tray/tray-white.svg`, `.ico` | Monochrome tray glyph for a dark taskbar. |
| `tray/tray-black.svg`, `.ico` | Monochrome tray glyph for a light taskbar. |

Sizes: `app.ico` has 16, 24, 32, 48, 64, 128 and 256 px. The tray icons have 16, 20, 24, 32 and 48 px
(100% to 300% display scaling). When the app gets a tray icon, pick white or black from the
Windows theme setting.

## Regenerating
Everything here comes from one script. Do not edit the SVGs by hand.

    python tools/gen_icons.py              # SVGs only (Python standard library)
    python tools/gen_icons.py --render     # also PNGs and .ico files (needs rsvg-convert on PATH)
