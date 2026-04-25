#version 460 core

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) uniform sampler2D u_texture;
layout (location = 0) out vec4 fragColor;

void main()
{
  float fog = texture(u_texture, vTexCoord).r;
  // White fog: mix(scene, white, fog) via GL_SRC_ALPHA blend.
  fragColor = vec4(1.0, 1.0, 1.0, clamp(fog, 0.0, 1.0));
}
