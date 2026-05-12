#version 460 core

layout (location = 1) uniform sampler2D u_hdrBuffer;
layout (location = 2) uniform float u_exposureFactor = 1.0;
layout (location = 3) uniform int u_debugBypass = 0;
layout (location = 4) uniform float u_contrast = 1.0;
layout (location = 5) uniform float u_saturation = 1.0;
layout (location = 6) uniform float u_vibrance = 0.0;
layout (location = 7) uniform float u_blackPoint = 0.0;
layout (location = 8) uniform float u_vignetteStrength = 0.0;
layout (location = 9) uniform float u_vignetteSoftness = 0.5;

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 fragColor;

layout (std430, binding = 0) readonly buffer exposures
{
  float readExposure;
  float writeExposure;
};

vec3 ACESFitted(vec3 color);

vec3 ApplyColorGrade(vec3 c)
{
  // Black point remap (deeper blacks without crushing midtones too hard)
  float bp = clamp(u_blackPoint, 0.0, 0.2);
  c = max((c - bp) / max(1e-4, 1.0 - bp), 0.0);

  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));

  // Saturation
  c = mix(vec3(luma), c, max(u_saturation, 0.0));

  // Vibrance (boost low-sat colors more than high-sat colors)
  float maxc = max(c.r, max(c.g, c.b));
  float minc = min(c.r, min(c.g, c.b));
  float sat  = maxc - minc;
  float vib  = clamp((1.0 - sat) * u_vibrance, 0.0, 1.0);
  vec3 avg = vec3((c.r + c.g + c.b) * (1.0 / 3.0));
  c += (c - avg) * vib;

  // Contrast (around middle gray)
  c = (c - 0.5) * u_contrast + 0.5;
  return clamp(c, 0.0, 1.0);
}

void main()
{
  vec3 hdrColor = texture(u_hdrBuffer, vTexCoord).rgb;
  if (u_debugBypass != 0)
  {
    // Pass-through: no ACES, no auto-exposure, no gamma. Used for debug viz.
    fragColor = vec4(clamp(hdrColor, 0.0, 1.0), 1.0);
    return;
  }
  // Apply exposure in HDR first, then tone-map. This preserves highlight rolloff
  // and avoids whitening the sky when exposure rises.
  vec3 mapped = ACESFitted(hdrColor * (u_exposureFactor * readExposure));
  mapped = pow(mapped, vec3(1 / 2.2));
  mapped = ApplyColorGrade(mapped);

  // Subtle vignette for focus and depth.
  vec2 p = vTexCoord * 2.0 - 1.0;
  float r = dot(p, p);
  float vig = 1.0 - clamp(r * mix(0.35, 0.75, clamp(u_vignetteSoftness, 0.0, 1.0)), 0.0, 1.0)
                    * clamp(u_vignetteStrength, 0.0, 1.0);
  mapped *= vig;

  fragColor = vec4(mapped, 1.0);
}

// The code in this file after this line was originally written by Stephen Hill (@self_shadow), who deserves all
// credit for coming up with this fit and implementing it. Buy him a beer next time you see him. :)
// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
const mat3 ACESInputMat =
{
  {0.59719, 0.35458, 0.04823},
  {0.07600, 0.90834, 0.01566},
  {0.02840, 0.13383, 0.83777}
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
const mat3 ACESOutputMat =
{
  { 1.60475, -0.53108, -0.07367},
  {-0.10208,  1.10813, -0.00605},
  {-0.00327, -0.07276,  1.07602}
};

vec3 RRTAndODTFit(vec3 v)
{
  vec3 a = v * (v + 0.0245786) - 0.000090537;
  vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return a / b;
}

vec3 ACESFitted(vec3 color)
{
  color = color * ACESInputMat;

  // Apply RRT and ODT
  color = RRTAndODTFit(color);

  color = color * ACESOutputMat;

  // Clamp to [0, 1]
  color = clamp(color, 0.0, 1.0);

  return color;
}
