#version 460 core
#extension GL_ARB_bindless_texture : enable
#include "common.h"

struct Material
{
  uvec2 albedoHandle;
  uvec2 roughnessHandle;
  uvec2 metalnessHandle;
  uvec2 normalHandle;
  uvec2 ambientOcclusionHandle;
  uvec2 opacityHandle;
  vec4 albedoFactor;
  vec4 pbrFactors;
};

layout (location = 3) uniform bool u_materialOverride;
layout (location = 4) uniform vec3 u_albedoOverride;
layout (location = 5) uniform float u_roughnessOverride;
layout (location = 6) uniform float u_metalnessOverride;
layout (location = 7) uniform bool u_AOoverride;
layout (location = 8) uniform float u_ambientOcclusionOverride;
layout (location = 9) uniform float u_characterMask;

layout (binding = 1, std430) readonly buffer Materials
{
  Material materials[];
};

layout (location = 0) in VS_OUT
{
  vec3 vNormal;
  vec2 vTexCoord;
  flat uint vMaterialIndex;
  mat3 vTBN;
};

layout (location = 0) out vec4 gAlbedo;
layout (location = 1) out vec2 gNormal;
layout (location = 2) out vec4 gRMA;

void main()
{
  Material material = materials[vMaterialIndex];
  vec3 normal = normalize(vNormal);
  const bool hasAlbedo = (material.albedoHandle.x != 0 || material.albedoHandle.y != 0);
  const bool hasRoughness = (material.roughnessHandle.x != 0 || material.roughnessHandle.y != 0);
  const bool hasMetalness = (material.metalnessHandle.x != 0 || material.metalnessHandle.y != 0);
  const bool hasNormal = (material.normalHandle.x != 0 || material.normalHandle.y != 0);
  const bool hasAmbientOcclusion = (material.ambientOcclusionHandle.x != 0 || material.ambientOcclusionHandle.y != 0);
  vec3 baseFactor = max(material.albedoFactor.rgb, vec3(0.0));
  float roughnessFactor = clamp(material.pbrFactors.x, 0.0, 1.0);
  float metallicFactor = clamp(material.pbrFactors.y, 0.0, 1.0);
  float aoFactor = clamp(material.pbrFactors.z, 0.0, 1.0);

  // ORM packing is authored per-material by the import path.
  // pbrFactors.w = 1 -> packed ORM (R=AO,G=roughness,B=metallic),
  // pbrFactors.w = 0 -> non-packed (roughness/metalness sampled independently).
  // Keep an equality fallback for pre-flagged data.
  const bool ormPacked = (material.pbrFactors.w > 0.5) ||
      (hasRoughness && hasMetalness &&
       all(equal(material.roughnessHandle, material.metalnessHandle)));

  // Apply tangent-space normal map when provided.
  if (hasNormal)
  {
    vec3 tn = texture(sampler2D(material.normalHandle), vTexCoord).rgb * 2.0 - 1.0;
    // glTF/OpenGL normal maps: +Y is up in tangent space. If the import path
    // emitted DirectX normals (+Y down) the result looks inverted; flip here.
    normal = normalize(vTBN * tn);
  }

  gNormal = float32x3_to_oct(normalize(normal));
  // Fallback for assets without albedo texture binding: use the imported
  // material factor instead of ignoring authoring metadata.
  vec4 color = vec4(baseFactor, 1.0);
  if (hasAlbedo)
  {
    color = texture(sampler2D(material.albedoHandle), vTexCoord).rgba;
    color.rgb *= baseFactor;
  }
  // Opacity cutout: only when an explicit opacity map is registered.
  // Albedo alpha is NOT used for cutout — many solid meshes (buildings, props, NPCs)
  // have near-zero alpha in parts of their albedo texture due to premultiplied-alpha
  // export or UV seam padding, which must not create visible holes.
  const bool hasOpacity = (material.opacityHandle.x != 0 || material.opacityHandle.y != 0);
  if (hasOpacity) {
      if (texture(sampler2D(material.opacityHandle), vTexCoord).r < 0.1) discard;
  }

  gAlbedo.rgb = color.rgb;
  gAlbedo.a = 1.0; // unused
  // gRMA: R=roughness, G=metallic, B=AO, A=character mask (readability pass).
  gRMA.rgba = vec4(clamp(roughnessFactor, 0.04, 1.0),
                   metallicFactor,
                   aoFactor,
                   clamp(u_characterMask, 0.0, 1.0));
  if (!u_materialOverride)
  {
    if (ormPacked)
    {
      vec3 orm = texture(sampler2D(material.roughnessHandle), vTexCoord).rgb;
      gRMA[0] = clamp(orm.g * roughnessFactor, 0.04, 1.0);  // roughness
      gRMA[1] = clamp(orm.b * metallicFactor, 0.0, 1.0);    // metalness
      gRMA[2] = clamp(orm.r * aoFactor, 0.0, 1.0);          // occlusion
    }
    else
    {
      if (hasRoughness)
      {
        gRMA[0] = clamp(texture(sampler2D(material.roughnessHandle), vTexCoord).r * roughnessFactor, 0.04, 1.0);
      }
      if (hasMetalness)
      {
        gRMA[1] = clamp(texture(sampler2D(material.metalnessHandle), vTexCoord).r * metallicFactor, 0.0, 1.0);
      }
    }
    if (hasAmbientOcclusion)
    {
      gRMA[2] = clamp(texture(sampler2D(material.ambientOcclusionHandle), vTexCoord).r * aoFactor, 0.0, 1.0);
    }
  }
  else
  {
    gAlbedo.rgb = u_albedoOverride;
    gRMA[0] = u_roughnessOverride;
    gRMA[1] = u_metalnessOverride;
    gRMA[2] = u_ambientOcclusionOverride;
  }
  if (u_AOoverride)
  {
    gRMA[2] = u_ambientOcclusionOverride;
  }
}
