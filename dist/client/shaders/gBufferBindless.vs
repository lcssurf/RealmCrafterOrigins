#version 460 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aTangent;

#ifdef HAS_SKINNING
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4  aBoneWeights;
layout (binding = 2, std430) readonly buffer BoneMatrices
{
  mat4 bones[];
};
#endif

layout (location = 0) uniform mat4 u_viewProj;

// Instanced skinned path: model matrix + material index come from the objects SSBO
// (one entry per instance, indexed by gl_InstanceID).
// Non-instanced path: model matrix and material index are plain uniforms — avoids
// declaring binding 0 at all, which on some drivers causes validation failures for
// the first draw in a session when no prior SSBO was bound to that slot.
#ifdef HAS_INSTANCED_SKINNING

struct ObjectUniforms
{
  mat4 modelMatrix;
  uint materialIndex;
};

layout (binding = 0, std430) readonly buffer uniforms
{
  ObjectUniforms objects[];
};

#else // non-instanced (dynamic static meshes and per-draw skinned)

// No explicit location — driver assigns automatically to avoid conflict with
// u_viewProj which occupies locations 0-3 (mat4 = 4 consecutive slots).
uniform mat4  u_modelMatrix;
uniform uint  u_materialIndex;

#endif

layout (location = 0) out VS_OUT
{
  vec3 vNormal;
  vec2 vTexCoord;
  flat uint vMaterialIndex;
  mat3 vTBN;
};

void main()
{
  vTexCoord = aTexCoord;

  vec3 pos     = aPos;
  vec3 normal  = aNormal;
  vec3 tangent = aTangent;

#ifdef HAS_SKINNING
  #ifdef HAS_INSTANCED_SKINNING
  const int kMaxBones = 64;
  mat4 skin = bones[gl_InstanceID * kMaxBones + aBoneIDs.x] * aBoneWeights.x
            + bones[gl_InstanceID * kMaxBones + aBoneIDs.y] * aBoneWeights.y
            + bones[gl_InstanceID * kMaxBones + aBoneIDs.z] * aBoneWeights.z
            + bones[gl_InstanceID * kMaxBones + aBoneIDs.w] * aBoneWeights.w;
  #else
  mat4 skin = bones[aBoneIDs.x] * aBoneWeights.x
            + bones[aBoneIDs.y] * aBoneWeights.y
            + bones[aBoneIDs.z] * aBoneWeights.z
            + bones[aBoneIDs.w] * aBoneWeights.w;
  #endif
  pos     = vec3(skin * vec4(aPos,     1.0));
  normal  = mat3(skin) * aNormal;
  tangent = mat3(skin) * aTangent;
#endif

#ifdef HAS_INSTANCED_SKINNING
  mat4  modelMatrix   = objects[gl_InstanceID].modelMatrix;
  vMaterialIndex      = objects[gl_InstanceID].materialIndex;
#else
  mat4  modelMatrix   = u_modelMatrix;
  vMaterialIndex      = u_materialIndex;
#endif

  vec3 wPos = vec3(modelMatrix * vec4(pos, 1.0));
  vNormal   = normalize(vec3(modelMatrix * vec4(normal, 0.0)));

  vec3 t = vec3(modelMatrix * vec4(tangent, 0.0));
  t = normalize(t - dot(t, vNormal) * vNormal);
  vec3 b = cross(vNormal, t);
  vTBN = mat3(t, b, vNormal);

  gl_Position = u_viewProj * vec4(wPos, 1.0);
}
