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

struct ObjectUniforms
{
  mat4 modelMatrix;
  uint materialIndex;
};

layout (binding = 0, std430) readonly buffer uniforms
{
  ObjectUniforms objects[];
};

layout (location = 0) out VS_OUT
{
  vec3 vNormal;
  vec2 vTexCoord;
  flat uint vMaterialIndex;
  mat3 vTBN;
};

void main()
{
// Per-instance model: gl_InstanceID for batched instanced draws, gl_DrawID otherwise.
#ifdef HAS_INSTANCED_SKINNING
  ObjectUniforms obj = objects[gl_InstanceID];
#else
  ObjectUniforms obj = objects[gl_DrawID];
#endif
  vMaterialIndex = obj.materialIndex;
  vTexCoord = aTexCoord;

  vec3 pos     = aPos;
  vec3 normal  = aNormal;
  vec3 tangent = aTangent;

#ifdef HAS_SKINNING
  #ifdef HAS_INSTANCED_SKINNING
  // bones[] is packed as N×64 blocks: [inst0_bone0..63 | inst1_bone0..63 | ...]
  // The constant 64 must match kMaxBones in model.h.
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

  vec3 wPos = vec3(obj.modelMatrix * vec4(pos, 1.0));
  vNormal   = normalize(vec3(obj.modelMatrix * vec4(normal, 0.0)));

  // Build a world-space tangent basis. Re-orthogonalise the tangent against
  // the (possibly non-perfect) normal so TBN stays orthonormal even when the
  // source tangents come from authored meshes.
  vec3 t = vec3(obj.modelMatrix * vec4(tangent, 0.0));
  t = normalize(t - dot(t, vNormal) * vNormal);
  vec3 b = cross(vNormal, t);
  vTBN = mat3(t, b, vNormal);

  gl_Position = u_viewProj * vec4(wPos, 1.0);
}
