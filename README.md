# wayshaders
## Display GLSL shaders on a transparent window in Wayland
Works well with [Hyprwinwrap](https://github.com/gen3vra/hyprwinwrap).

## Building
`apt install libwayland-dev libwayland-egl1 libwayland-egl++1 libegl1-mesa-dev libgl-dev wayland-protocols` (Debian example)

Run `./build.sh` in the project folder.

1. `git clone https://github.com/gen3vra/wayland-glsl-transparent-shaders`
2. `cd wayland-glsl-transparent-shaders`
3. `./build.sh`

## Installing

After building, copy the `wayshaders` binary to a location in your PATH:
```bash
sudo cp wayshaders /usr/local/bin/
```

### Set up shaders
Create a directory for shader files:
```bash
mkdir -p ~/.config/wayshaders
```

Place them in `~/.config/wayshaders/` with names like:
- `shader0.frag` (required)
- `shader1.frag` (optional)
- `shader2.frag` (optional, up to 32 total)

You can also provide custom vertex shaders:
- `shader0.vert`, `shader1.vert`, etc. (optional)

### Run
```bash
wayshaders
```

## Configuration
Settings are automatically generated in `~/.config/wayshaders/wayshaders` on first run. You can edit this file to customize behavior.
