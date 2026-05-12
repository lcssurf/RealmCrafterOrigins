#version 460 core

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) uniform sampler2D u_texture;
layout (location = 1) uniform vec3 u_fogColor = vec3(0.70, 0.80, 0.93);
layout (location = 2) uniform float u_fogDensityScale = 1.0;
layout (location = 0) out vec4 fragColor;

void main()
{
  float fog = texture(u_texture, vTexCoord).r;
  // Clear-day atmospheric fog: subtler and less gray to keep open-sky feel.
  fog = clamp(pow(max(fog, 0.0), 1.55) * 0.12 * max(u_fogDensityScale, 0.0), 0.0, 0.22);
  vec3 fogColor = u_fogColor;
  fragColor = vec4(fogColor, fog);
}
