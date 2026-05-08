#version 460 core

// Shadow pass for instanced skinned draws (SkinnedInstancedEntry in pipeline.cpp).
// Reads model matrices from binding 0 indexed by gl_InstanceID, and bone matrices
// from binding 2 packed as N x kMaxBones blocks (matches gBufferBindless.vs).

layout (location = 0) in vec3  aPos;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4  aBoneWeights;

struct ObjectUniforms
{
    mat4 modelMatrix;
    uint materialIndex;
};

layout (binding = 0, std430) readonly buffer Uniforms
{
    ObjectUniforms objects[];
};

layout (binding = 2, std430) readonly buffer BoneMatrices
{
    mat4 bones[];
};

uniform mat4 u_lightMatrix;

void main()
{
    const int kMaxBones = 64;
    mat4 skin = bones[gl_InstanceID * kMaxBones + aBoneIDs.x] * aBoneWeights.x
              + bones[gl_InstanceID * kMaxBones + aBoneIDs.y] * aBoneWeights.y
              + bones[gl_InstanceID * kMaxBones + aBoneIDs.z] * aBoneWeights.z
              + bones[gl_InstanceID * kMaxBones + aBoneIDs.w] * aBoneWeights.w;
    vec3 pos = vec3(skin * vec4(aPos, 1.0));
    gl_Position = u_lightMatrix * objects[gl_InstanceID].modelMatrix * vec4(pos, 1.0);
}
