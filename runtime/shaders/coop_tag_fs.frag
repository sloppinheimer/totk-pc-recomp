#version 450
// The co-op name tags (vk_tags.c, shaders/coop_tag_vs.vert): the vertex colour times the font atlas, as Dear ImGui draws.
layout(binding = 0) uniform sampler2D font;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 o;
void main() { o = col * texture(font, uv); }
