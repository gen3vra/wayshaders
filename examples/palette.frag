#version 460 core
uniform vec2 u_resolution;

uniform vec3 u_color0;
uniform vec3 u_color1;
uniform vec3 u_color2;
uniform vec3 u_color3;
uniform vec3 u_color4;
uniform vec3 u_color5;
uniform vec3 u_color6;
uniform vec3 u_color7;
uniform vec3 u_color8;
uniform vec3 u_color9;
uniform vec3 u_color10;
uniform vec3 u_color11;
uniform vec3 u_color12;
uniform vec3 u_color13;
uniform vec3 u_color14;
uniform vec3 u_color15;
uniform vec3 u_background;
uniform vec3 u_foreground;

out vec4 fragColor;

void main()
{
    vec3 palette[18] = vec3[](
        u_color0,  u_color1,  u_color2,  u_color3,  u_color4,  u_color5,
        u_color6,  u_color7,  u_color8,  u_color9,  u_color10, u_color11,
        u_color12, u_color13, u_color14, u_color15, u_background, u_foreground
    );

    const vec2 grid = vec2(6.0, 3.0);
    vec2 uv = gl_FragCoord.xy / u_resolution;
    uv.y = 1.0 - uv.y;

    vec2 cell = min(uv * grid, grid - 0.001);
    int index = int(cell.y) * int(grid.x) + int(cell.x);

    vec2 cellFract = fract(cell);
    vec2 distToEdge = min(cellFract, 1.0 - cellFract);
    float swatch = step(0.06, min(distToEdge.x, distToEdge.y));

    fragColor = vec4(palette[index] * swatch, swatch);
}
