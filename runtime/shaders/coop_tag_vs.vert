#version 450
// The co-op name tags drawn into the game's frame before its HUD (vk_tags.c). The vertices are Dear ImGui's
// (ImDrawVert: position in pixels, font atlas uv, RGBA8 colour), laid out around the tag's anchor at (0, 0); the anchor
// is the point above the player's head, projected with the frame's own camera. The scene's linear depth (view distance
// divided by the far plane, written by the game's G-buffer pass) is read at nine points around the anchor: where
// something is nearer than the head, that tap is covered, and the tag fades with the share of covered taps, so it is
// hidden behind walls and hills and fades in and out at their edges.
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 col;

layout(binding = 1) uniform sampler2D lin_depth;

layout(push_constant) uniform PC {
    vec4 anchor;   // xy: the anchor in target pixels, zw: 2 / target size
    vec4 depth;    // x: the anchor's view depth (metres), y: far plane, z: tap spacing (0..1 of the screen), w: 1 test depth
    vec4 misc;     // x: alpha, y: 1 when the target holds linear colour (sRGB colours are decoded), z: pixel scale
} pc;

layout(location = 0) out vec2 o_uv;
layout(location = 1) out vec4 o_col;

void main() {
    vec2 p = pc.anchor.xy + pos * pc.misc.z;
    gl_Position = vec4(p * pc.anchor.zw - 1.0, 0.0, 1.0);
    float vis = 1.0;
    if (pc.depth.w > 0.5) {
        vec2 a = pc.anchor.xy * pc.anchor.zw * 0.5;
        float open = 0.0;
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                float d = textureLod(lin_depth, a + vec2(i, j) * pc.depth.z, 0.0).x * pc.depth.y;
                open += d > pc.depth.x - 0.5 ? 1.0 : 0.0;   // half a metre of slack for the player's own head
            }
        vis = smoothstep(0.15, 0.85, open / 9.0);
    }
    vec3 c = pc.misc.y > 0.5 ? pow(col.rgb, vec3(2.2)) : col.rgb;
    o_col = vec4(c, col.a * pc.misc.x * vis);
    o_uv = uv;
}
