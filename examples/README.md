# Example Shaders
Copy + paste into your .config/wayshaders dir (may need to create it)

Make sure to name it "shader[n].frag"; for one singular shader `shader0.frag`.

## Palette colors
Shaders can pull colors from `.config/wayshaders/colors`
- 18 hex colors (`#rrggbb`), one per line. Declare any of these uniforms to receive them:

- `u_color0` … `u_color15` — lines 1–16
- `u_background` — line 17
- `u_foreground` — line 18

`pkill -HUP wayshaders` to reload
Without a colors file every uniform defaults to light grey, except `u_background` which defaults to black.
