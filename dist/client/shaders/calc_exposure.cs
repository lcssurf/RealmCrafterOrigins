#version 460 core
#define NUM_BUCKETS 128

layout (std430, binding = 0) buffer exposures
{
  float readExposure;
  float writeExposure;
};

layout (std430, binding = 1) buffer histogram
{
  coherent int buckets[NUM_BUCKETS];
};

layout (location = 0) uniform float u_dt;
layout (location = 1) uniform float u_adjustmentSpeed;
layout (location = 2) uniform float u_logLowLum;
layout (location = 3) uniform float u_logMaxLum;
layout (location = 4) uniform float u_targetLuminance = 0.22;
layout (location = 5) uniform float u_minValidFraction = 0.20;
layout (location = 6) uniform uint u_histogramSampleCount = 1u;
layout (location = 7) uniform float u_maxStepUp = 0.08;
layout (location = 8) uniform float u_maxStepDown = 0.08;

float map(float val, float r1s, float r1e, float r2s, float r2e)
{
  return (val - r1s) / (r1e - r1s) * (r2e - r2s) + r2s;
}

// TODO: test performance of this kernel vs simply running it on the CPU
layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
void main()
{
  float temp = readExposure;
  readExposure = writeExposure;
  writeExposure = temp;

  uint weightedSum = 0;
  uint validPixels = 0;
  for (int i = 0; i < NUM_BUCKETS; i++)
  {
    uint c = uint(max(buckets[i], 0));
    weightedSum += c * uint(i + 1);
    validPixels += c;
    buckets[i] = 0;
  }

  if (validPixels == 0u) {
    writeExposure = readExposure;
    return;
  }

  float meanLuminance = exp(map(float(weightedSum) / float(validPixels), 0.0, NUM_BUCKETS, u_logLowLum, u_logMaxLum));
  if (isnan(meanLuminance) || isinf(meanLuminance)) meanLuminance = 1.0;
  meanLuminance = max(meanLuminance, 0.0001);

  // When the sky dominates the frame, depth-rejected pixels can leave too few
  // valid samples and make exposure jump. Blend toward previous luminance when
  // coverage is low so "looking at the sun" behaves like a camera effect, not
  // a global relight of the whole scene.
  float sampleCount = max(1.0, float(u_histogramSampleCount));
  float validFrac = clamp(float(validPixels) / sampleCount, 0.0, 1.0);
  float coverT = smoothstep(u_minValidFraction, min(1.0, u_minValidFraction * 2.0), validFrac);
  float prevLuminance = u_targetLuminance / max(readExposure, 0.0001);
  float stableLuminance = mix(prevLuminance, meanLuminance, coverT);

  float exposureTarget = u_targetLuminance / stableLuminance;
  exposureTarget = clamp(exposureTarget, 0.01, 64.0);

  float stepUp = clamp(u_maxStepUp, 0.0, 0.95);
  float stepDown = clamp(u_maxStepDown, 0.0, 0.95);
  float minTarget = readExposure * (1.0 - stepDown);
  float maxTarget = readExposure * (1.0 + stepUp);
  exposureTarget = clamp(exposureTarget, minTarget, maxTarget);

  writeExposure = mix(readExposure, exposureTarget, clamp(u_dt * u_adjustmentSpeed, 0.0, 1.0));
}
