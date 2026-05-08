#version 460 core

// Shadow pass for non-instanced skinned draws (DynamicDrawRequest with
// bone_ssbo set, used by Actor::SubmitBlended during animation crossfades).

layout (location = 0) in vec3  aPos;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4  aBoneWeights;

layout (binding = 2, std430) readonly buffer BoneMatrices
{
    mat4 bones[];
};

uniform mat4 u_lightMatrix;
uniform mat4 u_model;

void main()
{
    mat4 skin = bones[aBoneIDs.x] * aBoneWeights.x
              + bones[aBoneIDs.y] * aBoneWeights.y
              + bones[aBoneIDs.z] * aBoneWeights.z
              + bones[aBoneIDs.w] * aBoneWeights.w;
    vec3 pos = vec3(skin * vec4(aPos, 1.0));
    gl_Position = u_lightMatrix * u_model * vec4(pos, 1.0);
}
