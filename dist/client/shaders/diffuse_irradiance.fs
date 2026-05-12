#version 460 core

in vec3 vLocalPos;
layout (binding = 0) uniform samplerCube u_envCube;
uniform float u_iblClamp;

layout (location = 0) out vec4 fragColor;

const float PI = 3.14159265359;

void main()
{
    vec3 N = normalize(vLocalPos);

    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    vec3 irradiance = vec3(0.0);
    float sampleDelta = 0.025;
    float nrSamples = 0.0;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            vec3 env = texture(u_envCube, sampleVec).rgb;
            env = min(env, vec3(max(u_iblClamp, 0.001)));
            irradiance += env * cos(theta) * sin(theta);
            nrSamples += 1.0;
        }
    }
    irradiance = PI * irradiance * (1.0 / nrSamples);
    fragColor = vec4(irradiance, 1.0);
}
