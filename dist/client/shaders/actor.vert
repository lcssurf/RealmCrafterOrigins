#version 460 core
layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUV;
layout(location = 3) in vec3  aTangent;
layout(location = 4) in ivec4 aBoneIds;
layout(location = 5) in vec4  aBoneWeights;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform mat4 uBones[64];
uniform bool uSkinned;

out vec3 vWorldPos;
out vec2 vUV;
out mat3 vTBN;

void main() {
    vec3 localPos;
    vec3 localNorm;
    vec3 localTang;

    if (uSkinned) {
        mat4 skin = aBoneWeights.x * uBones[aBoneIds.x]
                  + aBoneWeights.y * uBones[aBoneIds.y]
                  + aBoneWeights.z * uBones[aBoneIds.z]
                  + aBoneWeights.w * uBones[aBoneIds.w];
        localPos  = (skin * vec4(aPos,     1.0)).xyz;
        localNorm = mat3(skin) * aNormal;
        localTang = mat3(skin) * aTangent;
    } else {
        localPos  = aPos;
        localNorm = aNormal;
        localTang = aTangent;
    }

    vec4 wp   = uModel * vec4(localPos, 1.0);
    vWorldPos = wp.xyz;
    vUV       = aUV;

    mat3 normalMat = mat3(transpose(inverse(uModel)));
    vec3 N = normalize(normalMat * localNorm);
    vec3 T = normalize(normalMat * localTang);
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    gl_Position = uProj * uView * wp;
}
