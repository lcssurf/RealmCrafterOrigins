#version 460 core
#include "common.h"
#include "pbr_common.h"

struct PointLight
{
  vec4 diffuse;

  vec4 position;
  float linear;
  float quadratic;
  float radiusSquared;
  float _padding;

  // Spot/Directional support (additive — light_type 0/Point below reads
  // neither field, so its code path is byte-for-byte the same math as
  // before this struct grew). spotParams.z: 0=Point 1=Spot 2=Directional.
  vec4 direction;
  vec4 spotParams;
};

layout (location = 0) in flat PointLight vLight;

layout (location = 2) uniform sampler2D gNormal;
layout (location = 3) uniform sampler2D gAlbedo;
layout (location = 4) uniform sampler2D gRMA;
layout (location = 5) uniform sampler2D gDepth;
layout (location = 6) uniform vec3 u_viewPos;
layout (location = 7) uniform mat4 u_invViewProj;

layout (location = 0) out vec4 fragColor;

float GetAttenuation(float distance, float linearFalloff, float quadraticFalloff)
{
  return 1.0 / (linearFalloff * distance + quadraticFalloff * (distance * distance));
}

void main()
{
  vec2 texSize = textureSize(gNormal, 0);
  vec2 texCoord = gl_FragCoord.xy / texSize;
  vec3 vNormal = oct_to_float32x3(texture(gNormal, texCoord).xy);
  vec3 vPos = WorldPosFromDepth(texture(gDepth, texCoord).r, texSize, u_invViewProj);
  //vec3 vNormal = texture(gNormal, texCoord).xyz;

  float distanceToLightSquared = dot(vPos - vLight.position.xyz, vPos - vLight.position.xyz);
  if (vNormal == vec3(0) || distanceToLightSquared > vLight.radiusSquared)
  {
    discard;
  }

  vec3 albedo = texture(gAlbedo, texCoord).rgb;

  float lightType = vLight.spotParams.z; // 0=Point 1=Spot 2=Directional

  // Point/Spot still fall off with distance (unchanged formula). Directional
  // is treated as constant intensity within its placed radius — see
  // AddDirectionalLocalLight's comment (pipeline.h) for why this isn't a
  // true infinite directional light.
  float attenuation = (lightType > 1.5)
      ? 1.0
      : GetAttenuation(sqrt(distanceToLightSquared), vLight.linear, vLight.quadratic);

  vec4 RMA = texture(gRMA, texCoord);
  float roughness = RMA[0];
  float metalness = RMA[1];
  float ambientOcclusion = RMA[2];

  vec3 N = normalize(vNormal);
  vec3 V = normalize(u_viewPos - vPos);
  // Point/Spot: L points from the surface toward the light's POSITION
  // (unchanged). Directional: L points opposite the configured travel
  // direction, same convention as the sun (DirLight) elsewhere.
  vec3 L = (lightType > 1.5)
      ? normalize(-vLight.direction.xyz)
      : normalize(vLight.position.xyz - vPos);
  vec3 H = normalize(V + L);
  vec3 F0 = mix(vec3(0.04), albedo, metalness);
  F0 = clamp(F0, vec3(0.01), vec3(0.99));
  vec3 radiance = vLight.diffuse.rgb * attenuation;

  // Spot cone falloff — smooth, not a hard cutoff: 0 outside the outer
  // cone angle, 1 inside the inner angle, smoothstep in between. Point and
  // Directional are unaffected (this branch only runs for lightType==1).
  if (lightType > 0.5 && lightType < 1.5) {
    float cosAngle = dot(normalize(vLight.direction.xyz), normalize(vPos - vLight.position.xyz));
    float spotFactor = smoothstep(vLight.spotParams.x, vLight.spotParams.y, cosAngle);
    radiance *= spotFactor;
  }

  float NDF = D_GGX(N, H, roughness);
  float G = G_Smith(N, V, L, roughness);
  vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

  vec3 kS = F;
  vec3 kD = vec3(1.0) - kS;
  kD *= 1.0 - metalness;

  // cook-torrance brdf
  vec3 numerator = NDF * G * F;
  float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
  vec3 specular = numerator / max(denominator, 0.001);

  float cosTheta = max(dot(N, L), 0.0);
  vec3 local = (kD * albedo / M_PI + specular) * radiance * cosTheta;
  //fragColor = vec4(local, 0.0);
  fragColor = vec4(local, 0.0) * smoothstep((vLight.radiusSquared), .4 * (vLight.radiusSquared), (distanceToLightSquared));
}