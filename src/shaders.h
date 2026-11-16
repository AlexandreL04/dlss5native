// Compute shaders of the colour stage (see colorstage.h for the pipeline they form).
//
// The codecs, the divisor governor, the exposure feed, the pedestal, the detail-stability filter and the
// edge-aware transport follow the RenoDX DLSS Neural Rendering add-on (v8.5), MIT License,
// Copyright (c) 2025 Carlos Lopez Jr.
// https://github.com/Andarael/RenoDX-DLSS5-Generic - https://github.com/clshortfuse/renodx
#pragma once

static const char* kColorStageHLSL = R"(
Texture2D<float4> tHdr : register(t0);       // copy of the game's DLSS output rectangle (copy path only)
Texture2D<float4> tPin : register(t1);       // proxy handed to the model (working resolution)
Texture2D<float4> tPout : register(t2);      // model output (working resolution)
Texture2D<float2> tMotion : register(t3);    // the game's motion vectors
Texture2D<float4> tHistory : register(t4);   // previous filtered edit: a, c.r, c.b, proxy log-luminance
Texture2D<float4> tResolved : register(t5);  // resolved frame before the pedestal pass
Texture2D<float4> tBlocks : register(t6);    // 32x32 block statistics
Texture2D<float4> tExposure : register(t7);  // the game's exposure texture
Texture2D<float4> tPanel : register(t8);     // settings panel
Texture2D<float> tGuide : register(t9);      // log2 luminance of the frame over each working pixel's footprint
Texture2D<float4> tShaped : register(t10);   // stabilized model output (working resolution)
RWTexture2D<float4> uOut : register(u0);
RWTexture2D<float4> uScale : register(u1);   // .r divisor in use, .g reading, .b raw exposure, .a state
RWTexture2D<float4> uHistory : register(u2);
RWTexture2D<float4> uCommit : register(u3);  // .r measured divisor, .g tracking, .b held exposure divisor, .a guard tripped
RWTexture2D<float4> uGame : register(u4);    // the game's DLSS output itself (in-place path)
RWTexture2D<float> uGuide : register(u5);
cbuffer C : register(b0) {
    float fixedDivisor; float chromaClamp; float lumaClamp; float colorStrength;
    uint w; uint h; uint flags; float dt;
    float attack; float release; float snap; float alpha;
    float2 motionScale; uint2 motionBase;
    uint2 motionGrid; uint encoding; uint curve;
    float transferStrength; uint transferMode; float darkGate; float pedestalCap;
    float pedestalGate; uint darkGateMode; float preExposure; float exposureScale;
    uint exposureSource; uint exposureGuard; uint nrW; uint nrH;
    uint2 srcBase; uint2 panelPos;
    uint2 panelSize; float panelWhite; float lookStrength;
    float lookBrighten; float lookDarken; float lookColour; float lookHue;
    float lookShadows; float lookMidtones; float lookHighlights; float lookMaxBrighten;
    float lookMaxDarken; float lookMaxColour; uint pad0; uint pad1;
};
static const uint kSkipModel = 1u, kTemporal = 2u, kMotion = 4u, kHistoryValid = 8u, kGateInInputUnits = 16u, kShaped = 32u, kLook = 64u;
static const float kMedianTarget = 0.30;
static const float kEngageStops = 0.15;
static const float kSettleStops = 0.02;
static const float kReadableFloor = 1.0 / 1024.0;
static const uint kReadableSamples = 36u;
static const float kRatioFloor = 1.0 / 1024.0;
static const float kGuideFloor = 1e-8;
static const float3 kW = float3(0.212639, 0.715169, 0.072192);
static const float kDisplayToeA = 0.014;
static const float kDisplayToeB = 0.001;
static const float kDisplayKnee = 0.75;
static const float kDisplayShoulderRate = 5.770780;

float maxc(float3 c) { return max(c.r, max(c.g, c.b)); }
float Luminance(float3 c) { return dot(c, kW); }
bool finite1(float v) { return (asuint(v) & 0x7F800000u) != 0x7F800000u; }
bool finite3(float3 c) { return !any((asuint(c) & 0x7F800000u) == 0x7F800000u); }
float3 Guarded(float3 c) { return float3(finite1(c.r) ? c.r : (c.r > 0.0 ? 65504.0 : 0.0), finite1(c.g) ? c.g : (c.g > 0.0 ? 65504.0 : 0.0), finite1(c.b) ? c.b : (c.b > 0.0 ? 65504.0 : 0.0)); }
float3 SrgbEncode(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 SrgbDecode(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 SrgbDecodeExtended(float3 e) { float3 a = abs(e); return sign(e) * (a <= 0.04045 ? a / 12.92 : pow((a + 0.055) / 1.055, 2.4)); }
float3 SrgbEncodeExtended(float3 l) { float3 a = abs(l); return sign(l) * (a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - 0.055); }
float3 LinearToPQ(float3 lin) {
    lin = saturate(lin);
    float3 x = pow(lin, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * x) / (1.0 + 18.6875 * x), 78.84375);
}
float3 PQToLinear(float3 pq) {
    pq = saturate(pq);
    float3 x = pow(pq, 1.0 / 78.84375);
    return pow(max(x - 0.8359375, 0.0) / (18.8515625 - 18.6875 * x), 1.0 / 0.1593017578125);
}
float FloorTrust(float3 proxyLinear) { return smoothstep(1.0 / 1024.0, 1.0 / 64.0, maxc(proxyLinear)); }
float BlackRestoreWeight(float blockMaximum, float gate, bool smoothGate) {
    return smoothGate && gate > 0.0 ? 1.0 - smoothstep(0.5 * gate, gate, blockMaximum) : (blockMaximum <= gate ? 1.0 : 0.0);
}
uint ScaleBin(float v) {
    float stops = clamp(log2(max(v, 5.96046448e-8)), -24.0, 24.0);
    return min((uint)floor((stops + 24.0) * (256.0 / 48.0)), 255u);
}

// The game's frame. In place it is read and written through one unordered-access view of the game's own
// texture; on the copy path it is read from a copy and the result goes to a texture copied back afterwards.
#if IN_PLACE
float4 RawFrame(int2 pixel) { return uGame[uint2(pixel) + srcBase]; }
void WriteFinal(int2 pixel, float4 value) { uGame[uint2(pixel) + srcBase] = value; }
#else
float4 RawFrame(int2 pixel) { return tHdr.Load(int3(pixel, 0)); }
void WriteFinal(int2 pixel, float4 value) { uOut[uint2(pixel)] = value; }
#endif
// The frame in its own linear units (SDR keeps its code values).
float4 LoadFrame(int2 pixel) {
    float4 v = RawFrame(pixel);
    if (encoding == 2u) v.rgb = PQToLinear(v.rgb);
    else if (encoding == 3u) v.rgb = SrgbDecodeExtended(v.rgb);
    return v;
}
float3 EncodeFrame(float3 lin) {
    if (encoding == 2u) return LinearToPQ(max(lin, 0.0));
    if (encoding == 3u) return SrgbEncodeExtended(lin);
    return lin;
}
float3 FrameLinear(float3 frame) { return encoding == 0u ? SrgbDecode(frame) : frame; }
float Divisor() { return fixedDivisor > 0.0 ? fixedDivisor : max(uScale[uint2(0, 0)].r, 1e-8); }

float DisplayCurve(float m) {
    float t = m + kDisplayToeA * m / (m + kDisplayToeB);
    return t > kDisplayKnee ? kDisplayKnee + (1.0 - kDisplayKnee) * (1.0 - exp(-kDisplayShoulderRate * (t - kDisplayKnee))) : t;
}
float DisplayCurveLogSlope(float m) {
    float t = m + kDisplayToeA * m / (m + kDisplayToeB);
    float toeSlope = 1.0 + kDisplayToeA * kDisplayToeB / ((m + kDisplayToeB) * (m + kDisplayToeB));
    float shoulderSlope = t > kDisplayKnee ? (1.0 - kDisplayKnee) * kDisplayShoulderRate * exp(-kDisplayShoulderRate * (t - kDisplayKnee)) : 1.0;
    float f = DisplayCurve(m);
    float encodeSlope = f > 0.0031308 ? (1.055 / 2.4) * pow(f, 1.0 / 2.4 - 1.0) : 12.92;
    return m * toeSlope * shoulderSlope * encodeSlope;
}
float DisplayCurveInverse(float u) {
    float t = u > kDisplayKnee ? kDisplayKnee - log(max(1.0 - (u - kDisplayKnee) / (1.0 - kDisplayKnee), 1e-7)) / kDisplayShoulderRate : u;
    float b = t - kDisplayToeA - kDisplayToeB;
    float root = sqrt(b * b + 4.0 * t * kDisplayToeB);
    return b < 0.0 ? 2.0 * t * kDisplayToeB / (root - b) : 0.5 * (b + root);
}

float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }
float3 ToOkLab(float3 color) {
    const float3x3 rgbToLms = { 0.4122214708, 0.5363325363, 0.0514459929, 0.2119034982, 0.6806995451, 0.1073969566, 0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lmsToLab = { 0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050, 0.4505937099, 0.0259040371, 0.7827717662, -0.8086757660 };
    return mul(lmsToLab, CbrtSigned(mul(rgbToLms, color)));
}
float3 FromOkLab(float3 lab) {
    const float3x3 labToLms = { 1.0, 0.3963377774, 0.2158037573, 1.0, -0.1055613458, -0.0638541728, 1.0, -0.0894841775, -1.2914855480 };
    const float3x3 lmsToRgb = { 4.0767416621, -3.3077115913, 0.2309699292, -1.2684380046, 2.6097574011, -0.3413193965, -0.0041960863, -0.7034186147, 1.7076147010 };
    float3 lms = mul(labToLms, lab);
    return mul(lmsToRgb, lms * lms * lms);
}
float3 ClampAp1(float3 color) {
    const float3x3 bt709ToAp1 = { 0.613097, 0.339523, 0.047379, 0.070194, 0.916354, 0.013452, 0.020616, 0.109570, 0.869815 };
    const float3x3 ap1ToBt709 = { 1.705051, -0.621792, -0.083259, -0.130256, 1.140805, -0.010548, -0.024003, -0.128969, 1.152972 };
    return mul(ap1ToBt709, max(0.0, mul(bt709ToAp1, color)));
}
float3 HueOkLab(float3 incorrect, float3 correct) {
    float3 incorrectLab = ToOkLab(incorrect);
    float3 correctLab = ToOkLab(correct);
    float incorrectChroma = length(incorrectLab.yz);
    float correctChroma = length(correctLab.yz);
    incorrectLab.yz = correctLab.yz * (correctChroma == 0.0 ? 1.0 : incorrectChroma / correctChroma);
    return ClampAp1(FromOkLab(incorrectLab));
}
// Divisor family: keep the frame's luminance above the proxy, take the model's colour and shading.
float3 UpgradeToneMap(float3 original, float3 proxy, float3 neural) {
    float originalY = Luminance(original), proxyY = Luminance(proxy), neuralY = Luminance(neural);
    float3 result = original;
    if (neuralY > 1e-5) {
        float ratio = originalY / max(proxyY, 1e-6);
        if (originalY >= proxyY) ratio = (neuralY + max(0.0, originalY - proxyY)) / neuralY;
        float3 scaled = HueOkLab(neural * ratio, neural);
        float trust = curve == 2u ? max(FloorTrust(neural), 1.0 - smoothstep(1.0, 3.0, log2(max(ratio, 1.0)))) : 1.0;
        if (trust < 1.0) scaled = lerp(original * (neuralY * ratio / max(originalY, 1e-6)), scaled, trust);
        result = lerp(original, scaled, transferStrength);
    }
    return result;
}

)" R"(
// ---------------------------------------------------------------------------------------- autoscale
groupshared uint Histogram[256];
groupshared uint ValidCount;
groupshared uint ShoulderCount;
groupshared float LogSum[64];

uint2 LatticePixel(uint index, uint2 size) {
    uint2 cell = uint2(index & 63u, index >> 6u);
    return min(uint2(((cell.x * 2u + 1u) * size.x) / 128u, ((cell.y * 2u + 1u) * size.y) / 72u), size - 1u);
}

[numthreads(64, 1, 1)]
void Measure(uint local : SV_GroupIndex) {
    [unroll] for (uint b0 = local; b0 < 256u; b0 += 64u) Histogram[b0] = 0u;
    if (local == 0u) { ValidCount = 0u; ShoulderCount = 0u; }
    GroupMemoryBarrierWithGroupSync();
    uint2 size = max(uint2(w, h), uint2(1, 1));
    [loop] for (uint index = local; index < 64u * 36u; index += 64u) {
        float value = maxc(LoadFrame(int2(LatticePixel(index, size))).rgb);
        if (finite1(value) && value > 0.0) {
            InterlockedAdd(Histogram[ScaleBin(value)], 1u);
            InterlockedAdd(ValidCount, 1u);
            if (value > kDisplayKnee) InterlockedAdd(ShoulderCount, 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (local != 0u) return;

    // One-sided: the divisor is 1 unless enough of the frame is proven over the shoulder.
    float gateFraction = ValidCount != 0u ? float(ShoulderCount) / float(ValidCount) : 0.0;
    float gateWeight = smoothstep(-6.0, -4.0, log2(max(gateFraction, 1e-6)));
    float candidate = 1.0;
    if (gateWeight > 0.0) {
        uint middle = (ValidCount - 1u) >> 1u, cumulative = 0u, medianBin = 0u, below = 0u;
        [loop] for (uint bin = 0u; bin < 256u; ++bin) {
            uint next = cumulative + Histogram[bin];
            if (next > middle) { medianBin = bin; below = cumulative; break; }
            cumulative = next;
        }
        float within = (float(middle - below) + 0.5) / float(max(Histogram[medianBin], 1u));
        float medianStops = -24.0 + (float(medianBin) + within) * (48.0 / 256.0);
        candidate = exp2(gateWeight * max(medianStops - log2(kMedianTarget), 0.0));
    }

    // Governor: hysteresis, asymmetric rate, hold on a frame the model itself sees as black.
    float4 carried = uCommit[uint2(0, 0)];
    float previous = carried.r;
    bool wasTracking = carried.g > 0.5;
    float committed = candidate, tracking = 0.0;
    if (previous > 0.0 && snap == 0.0) {
        uint lit = 0u;
        [loop] for (uint bin2 = ScaleBin(previous * kReadableFloor) + 1u; bin2 < 256u; ++bin2) lit += Histogram[bin2];
        if (lit < kReadableSamples) { committed = previous; tracking = wasTracking ? 1.0 : 0.0; }
        else {
            float error = log2(candidate / previous);
            tracking = abs(error) > (wasTracking ? kSettleStops : kEngageStops) ? 1.0 : 0.0;
            committed = tracking > 0.0 ? previous * exp2(clamp(error, -release * dt, attack * dt)) : previous;
        }
    }
    uCommit[uint2(0, 0)] = float4(committed, tracking, carried.b, carried.a);
    uScale[uint2(0, 0)] = float4(committed, 0.0, 0.0, 0.0);
}

// ------------------------------------------------------------------------ the game's exposure feed
// divisor = pre-exposure / (exposure x exposure scale). When guarded, the measured divisor takes over
// while the game's exposure would put the frame outside the range the model reads.
[numthreads(64, 1, 1)]
void ExposureScale(uint local : SV_GroupIndex) {
    static const float kMinDivisor = 5.96046448e-8, kMaxDivisor = 16777216.0;
    uint2 size = max(uint2(w, h), uint2(1, 1));
    float sum = 0.0; uint count = 0u;
    if (exposureGuard != 0u) {
        [loop] for (uint index = local; index < 64u * 36u; index += 64u) {
            float luma = dot(LoadFrame(int2(LatticePixel(index, size))).rgb, float3(0.2126, 0.7152, 0.0722));
            if (finite1(luma) && luma > 0.0) { sum += clamp(log2(luma), -24.0, 24.0); count++; }
        }
    }
    LogSum[local] = sum;
    if (local == 0u) ValidCount = 0u;
    GroupMemoryBarrierWithGroupSync();
    InterlockedAdd(ValidCount, count);
    GroupMemoryBarrierWithGroupSync();
    if (local != 0u) return;

    float4 carried = uCommit[uint2(0, 0)];
    float exposure = 0.0;
    float reading = exposureSource == 2u ? 1.0 : 0.0;
    if (exposureSource == 1u) {
        exposure = tExposure.Load(int3(0, 0, 0)).r;
        float d = preExposure / (exposure * exposureScale);
        if (finite1(d) && d >= kMinDivisor && d <= kMaxDivisor) reading = d;
    }
    float previous = carried.b;
    bool canHold = snap == 0.0 && finite1(previous) && previous >= kMinDivisor && previous <= kMaxDivisor;
    float committed, state;
    if (!(reading > 0.0)) { committed = canHold ? previous : 1.0; state = canHold ? 3.0 : 4.0; }
    else { committed = reading; state = exposureSource == 1u ? 1.0 : 2.0; }
    float held = committed;
    float tripped = carried.a;
    if (exposureGuard != 0u) {
        if (reading > 0.0 && ValidCount >= 144u) {
            float total = 0.0;
            for (uint i = 0u; i < 64u; ++i) total += LogSum[i];
            float exposed = total / float(ValidCount) - log2(reading);
            float margin = tripped > 0.5 ? 0.5 : 0.0;
            tripped = (exposed < -7.1876 + margin || exposed > -0.4150 - margin) ? 1.0 : 0.0;
        }
        if (state == 4.0) tripped = 1.0;
        if (tripped > 0.5) { committed = max(carried.r, 1e-8); state = 5.0; }
    }
    uCommit[uint2(0, 0)] = float4(carried.r, carried.g, held, tripped);
    uScale[uint2(0, 0)] = float4(committed, reading, finite1(exposure) ? exposure : -1.0, state);
}

)" R"(
// ------------------------------------------------------------------------------------------- encode
// The model's input for one pixel of the frame in linear light, before its sRGB encode.
float3 Proxy(float3 p) {
    p = max(p, 0.0) / Divisor();
    float m = maxc(p);
    if (curve == 1u) {                                   // Display codec: localized shadow toe
        float m2 = m + kDisplayToeA * m / (m + kDisplayToeB);
        p *= m2 / max(m, 1e-6);
        m = m2;
    }
    if (m > kDisplayKnee) p *= (kDisplayKnee + 0.25 * (1.0 - exp(-kDisplayShoulderRate * (m - kDisplayKnee)))) / max(m, 1e-6);
    return p;
}
// One thread per working pixel. At the frame's own resolution the pixel is read as is; below it an HDR
// frame is area-filtered in linear light and an SDR frame point-sampled, and the footprint's mean
// luminance is kept as the guide of the edge-aware transport.
[numthreads(8, 8, 1)]
void Encode(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nrW || id.y >= nrH) return;
    float3 p;
    if (nrW == w && nrH == h) p = Guarded(LoadFrame(int2(id.xy)).rgb);
    else {
        float2 scale = float2(w, h) / float2(nrW, nrH);
        float2 center = (float2(id.xy) + 0.5) * scale;
        float2 lo = max(center - 0.5 * scale, 0.0), hi = min(center + 0.5 * scale, float2(w, h));
        int2 first = int2(floor(lo)), last = int2(ceil(hi));
        float3 sum = 0.0, lit = 0.0; float weight = 0.0;
        for (int y = first.y; y < last.y; ++y)
            for (int x = first.x; x < last.x; ++x) {
                float2 overlap = min(float2(x + 1, y + 1), hi) - max(float2(x, y), lo);
                float k = max(overlap.x, 0.0) * max(overlap.y, 0.0);
                float3 texel = Guarded(LoadFrame(int2(x, y)).rgb);
                sum += texel * k; lit += FrameLinear(max(texel, 0.0)) * k; weight += k;
            }
        uGuide[id.xy] = log2(max(Luminance(lit / max(weight, 1e-12)), kGuideFloor));
        if (encoding == 0u) p = LoadFrame(int2(min(uint2(center), uint2(w, h) - 1u))).rgb;
        else p = weight > 0.0 ? sum / weight : 0.0;
    }
    if (encoding == 0u) { uOut[id.xy] = float4(max(p, 0.0), 1.0); return; }   // SDR: the code values themselves
    uOut[id.xy] = float4(SrgbEncode(Proxy(p)), 1.0);     // the model wants an opaque input
}

// ---------------------------------------------------------------------------------------- stabilize
// The model's edit at one working pixel: e = log2(neural / proxy), split in luma and opponent chroma.
struct Edit { float3 p; float3 n; float3 e; float a; float3 c; float I; float t; };
Edit ReadEdit(int2 pixel) {
    Edit x;
    x.p = SrgbDecode(tPin.Load(int3(pixel, 0)).rgb);
    x.n = (flags & kSkipModel) ? x.p : SrgbDecode(tPout.Load(int3(pixel, 0)).rgb);
    x.e = log2(max(x.n, kRatioFloor)) - log2(max(x.p, kRatioFloor));
    x.a = dot(kW, x.e);
    x.c = x.e - x.a;
    x.I = log2(max(dot(kW, x.p), kRatioFloor));
    x.t = FloorTrust(x.p);
    return x;
}
// Look (after RenoDX's look stage, without its band split): gains on the edit, in stops. `a` is the
// achromatic part of the edit and `c` its colour part; the result is the shaped edit per channel.
// Every gain at 1 and every limit at 0 returns a + c, and the flag is then not even set.
static const float kLog2Grey = -2.4739312;      // log2(0.18)
float ZoneGain(float I) {
    float L = I - kLog2Grey;
    float shadow = 1.0 - smoothstep(-3.5, -1.5, L);
    float highlight = smoothstep(0.5, 2.5, L);
    return shadow * lookShadows + (1.0 - shadow - highlight) * lookMidtones + highlight * lookHighlights;
}
float3 LookShape(Edit x, float a, float3 c) {
    if (!(flags & kLook)) return a + c;
    a = lookBrighten * max(a, 0.0) + lookDarken * min(a, 0.0);
    // the colour edit, split along and across the input's own colour direction (faded out near grey)
    float3 lp = log2(max(x.p, kRatioFloor));
    float3 o = lp - dot(kW, lp);
    float oo = dot(kW, o * o);
    float3 radial = (dot(kW, c * o) / max(oo, 1e-6)) * o;
    float split = smoothstep(0.05, 0.25, sqrt(oo));
    c = split * (lookColour * radial + lookHue * (c - radial)) + (1.0 - split) * lookColour * c;
    float gain = lookStrength * ZoneGain(x.I);
    a *= gain; c *= gain;
    // soft limits; the argument is bounded because tanh overflows to NaN past about 88
    if (a > 0.0 && lookMaxBrighten > 0.0) a = lookMaxBrighten * tanh(min(a / lookMaxBrighten, 10.0));
    if (a < 0.0 && lookMaxDarken > 0.0) a = lookMaxDarken * tanh(max(a / lookMaxDarken, -10.0));
    if (lookMaxColour > 0.0) {
        float norm = sqrt(dot(kW, c * c));
        if (norm > 1e-6) c *= lookMaxColour * tanh(min(norm / lookMaxColour, 10.0)) / norm;
    }
    return a + c;
}
// The model output carrying the shaped edit (the model output itself while the look is neutral).
float3 LookNeural(Edit x) { return (flags & kLook) ? saturate(x.n * exp2(x.t * (LookShape(x, x.a, x.c) - x.e))) : x.n; }

// Filters the edit over time: history reprojected with the game's motion vectors, clipped to the 3x3
// neighbourhood of the current edit, rejected where the input changed under it. Returns the model
// output carrying the filtered edit.
float3 Stabilize(int2 pixel, Edit x) {
    int2 size = int2(nrW, nrH);
    float3 current = float3(x.a, x.c.r, x.c.b);
    float3 filtered = current;
    if (flags & kHistoryValid) {
        float2 previous = float2(pixel);
        if (flags & kMotion) {
            uint2 texel = motionBase + min(uint2((float2(pixel) + 0.5) * float2(motionGrid) / float2(size)), motionGrid - 1u);
            previous += tMotion.Load(int3(texel, 0)).xy * motionScale;
        }
        float2 limit = float2(size) - 0.5;
        bool onscreen = all(previous >= -0.5) && all(previous <= limit);
        float2 pos = clamp(previous, 0.0, float2(size - 1));
        int2 h0 = int2(floor(pos));
        int2 h1 = min(h0 + 1, size - 1);
        float2 f = pos - float2(h0);
        float4 q00 = tHistory.Load(int3(h0, 0)), q10 = tHistory.Load(int3(h1.x, h0.y, 0));
        float4 q01 = tHistory.Load(int3(h0.x, h1.y, 0)), q11 = tHistory.Load(int3(h1, 0));
        float4 history = q00 * (1 - f.x) * (1 - f.y) + q10 * f.x * (1 - f.y) + q01 * (1 - f.x) * f.y + q11 * f.x * f.y;
        float3 lo = current, hi = current;
        float iLo = x.I, iHi = x.I;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                Edit around = ReadEdit(clamp(pixel + int2(dx, dy), 0, size - 1));
                float3 value = float3(around.a, around.c.r, around.c.b);
                lo = min(lo, value); hi = max(hi, value);
                iLo = min(iLo, around.I); iHi = max(iHi, around.I);
            }
        float inputChange = max(max(iLo - history.a, history.a - iHi), 0.0);
        float confidence = onscreen ? 1.0 - smoothstep(0.1, 0.3, inputChange) : 0.0;
        float beta = max(alpha, 1.0 - confidence);
        float3 clipped = clamp(history.rgb, lo, hi);
        filtered = clipped + beta * (current - clipped);
    }
    uHistory[uint2(pixel)] = float4(filtered, x.I);
    float3 c = float3(filtered.y, -(kW.x * filtered.y + kW.z * filtered.z) / kW.y, filtered.z);
    return saturate(x.n * exp2(x.t * (LookShape(x, filtered.x, c) - x.e)));
}

// Working resolution below the frame's: the stabilized edit is kept, in stops, for the transport. Kept
// as an edit rather than as an encoded image, an unchanged model output stays exactly zero.
[numthreads(8, 8, 1)]
void Shape(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nrW || id.y >= nrH) return;
    Edit x = ReadEdit(int2(id.xy));
    float3 neural = (flags & kTemporal) ? Stabilize(int2(id.xy), x) : LookNeural(x);
    uOut[id.xy] = float4(log2(max(neural, kRatioFloor)) - log2(max(x.p, kRatioFloor)), 1.0);
}

)" R"(
// ------------------------------------------------------------------------------------------ resolve
// One pixel of the frame from the proxy and the model output at that pixel; linear source units
// (SDR: code values).
float3 ResolveCore(float3 proxy, float3 neural, float3 original) {
    float3 result;
    if (!finite3(original)) return original;
    if (encoding == 0u) {
        // SDR: the model's colour at a bounded luminance ratio, on code values.
        float3 upgraded = SrgbEncode(neural);
        float originalY = max(Luminance(original), 0.0);
        float ratio = originalY == 0.0 ? 1.0 : clamp(Luminance(upgraded) / originalY, 0.0, 4.0);
        result = lerp(original * ratio, upgraded, colorStrength);
    } else if (curve == 1u) {
        // Display codec: the neural residual as a bounded gain on the untouched frame.
        float3 lg = log2(max(neural, kRatioFloor) / max(proxy, kRatioFloor));
        float luma = dot(lg, kW);
        float3 chroma = lg - luma;
        float correction = 0.0;
        if (transferMode == 1u) {
            float mProxy = DisplayCurveInverse(maxc(proxy));
            float mTarget = DisplayCurveInverse(maxc(neural));
            if (mProxy > 1e-7 && mTarget > 1e-7) {
                float s = min(DisplayCurveLogSlope(mProxy), DisplayCurveLogSlope(mTarget));
                float mNeural = mProxy * pow(mTarget / mProxy, s * s / (s * s + 1.28e-4));
                correction = log2((DisplayCurve(mProxy) / mProxy) / (DisplayCurve(mNeural) / mNeural));
            }
        }
        float l = clamp(luma + correction, -lumaClamp, lumaClamp);
        float3 cc = clamp(chroma, -chromaClamp, chromaClamp);
        result = lerp(original, original * exp2(FloorTrust(proxy) * (l + cc)), colorStrength);
    } else {
        // Divisor family: compare in proxy units, return to source units at once.
        float d = Divisor();
        float3 upgraded = UpgradeToneMap(max(original, 0.0) / d, proxy, neural) * d;
        float originalY = max(Luminance(original), 0.0);
        float ratio = originalY == 0.0 ? 1.0 : clamp(Luminance(upgraded) / originalY, 0.0, 4.0);
        result = lerp(original * ratio, upgraded, colorStrength);
    }
    if (encoding != 0u) { float peak = maxc(result); if (peak > 65504.0) result *= 65504.0 / peak; }
    return result;
}
// With a pedestal pass to follow the result stays linear in its own texture; otherwise it goes out in
// the game's encoding.
void WriteResolved(int2 pixel, float3 result, float a) {
    if (pedestalGate > 0.0) uOut[uint2(pixel)] = float4(result, a);
    else WriteFinal(pixel, float4(EncodeFrame(result), a));
}

// Working resolution equal to the frame's: every read is 1:1.
[numthreads(8, 8, 1)]
void Resolve(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    int2 pixel = int2(id.xy);
    Edit x = ReadEdit(pixel);
    float3 neural = (flags & kTemporal) ? Stabilize(pixel, x) : LookNeural(x);
    float4 frame = LoadFrame(pixel);
    WriteResolved(pixel, ResolveCore(x.p, neural, frame.rgb), frame.a);
}

// Working resolution below the frame's: the edit of the 4x4 working pixels around each frame pixel is
// averaged with joint-bilateral weights (spatial on the distance, range on the difference between this
// pixel's log luminance and each working pixel's footprint mean), so the edit follows the frame's
// edges instead of the working grid. The group's working pixels are decoded once into a shared tile.
static const float kSpatialSigma = 0.5;  // working pixels
static const float kRangeSigma = 0.5;    // stops
static const int kTile = 20;
groupshared float4 TileEdit[kTile * kTile];    // e per channel, .a = footprint guide
groupshared float3 TileProxy[kTile * kTile];
uint TileIndex(int2 local) { int2 at = clamp(local, 0, kTile - 1); return uint(at.y * kTile + at.x); }

[numthreads(16, 16, 1)]
void ResolveUp(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint local : SV_GroupIndex) {
    int2 nr = int2(nrW, nrH), full = int2(w, h);
    int2 last = nr - 1;
    int2 origin = int2(floor((float2(group.xy * 16u) + 0.5) * float2(nr) / float2(full) - 0.5)) - 1;
    for (uint i = local; i < uint(kTile * kTile); i += 256u) {
        int2 tap = clamp(origin + int2(i % uint(kTile), i / uint(kTile)), 0, last);
        float3 proxy = SrgbDecode(tPin.Load(int3(tap, 0)).rgb);
        float3 edit = 0.0;
        if (flags & kShaped) edit = tShaped.Load(int3(tap, 0)).rgb;
        else if (!(flags & kSkipModel)) edit = log2(max(SrgbDecode(tPout.Load(int3(tap, 0)).rgb), kRatioFloor)) - log2(max(proxy, kRatioFloor));
        TileProxy[i] = proxy;
        TileEdit[i] = float4(edit, tGuide.Load(int3(tap, 0)));
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= w || id.y >= h) return;
    int2 pixel = int2(id.xy);
    float2 position = (float2(pixel) + 0.5) * float2(nr) / float2(full) - 0.5;
    int2 base = int2(floor(position));
    float4 frame = LoadFrame(pixel);
    float3 linearFrame = FrameLinear(frame.rgb);
    float guideFull = log2(max(Luminance(max(linearFrame, 0.0)), kGuideFloor));

    float3 joint = 0.0, spatial = 0.0; float jointWeight = 0.0, spatialWeight = 0.0;
    for (int dy = -1; dy <= 2; ++dy)
        for (int dx = -1; dx <= 2; ++dx) {
            int2 tap = base + int2(dx, dy);
            float2 dist = float2(tap) - position;
            float ws = exp(-dot(dist, dist) / (2.0 * kSpatialSigma * kSpatialSigma));
            float4 edit = TileEdit[TileIndex(tap - origin)];
            float diff = guideFull - edit.a;
            float wr = exp(-(diff * diff) / (2.0 * kRangeSigma * kRangeSigma));
            joint += ws * wr * edit.rgb; jointWeight += ws * wr;
            spatial += ws * edit.rgb; spatialWeight += ws;
        }
    float3 editUp = jointWeight > 1e-6 ? joint / jointWeight : spatial / spatialWeight;

    float2 clamped = clamp(position, 0.0, float2(last));
    int2 b0 = int2(floor(clamped));
    int2 b1 = min(b0 + 1, last);
    float2 f = clamped - float2(b0);
    float3 proxyUp = TileProxy[TileIndex(b0 - origin)] * (1.0 - f.x) * (1.0 - f.y) + TileProxy[TileIndex(int2(b1.x, b0.y) - origin)] * f.x * (1.0 - f.y)
                   + TileProxy[TileIndex(int2(b0.x, b1.y) - origin)] * (1.0 - f.x) * f.y + TileProxy[TileIndex(b1 - origin)] * f.x * f.y;
    float3 gain = exp2(editUp);
    // Display codec: the gain on the floored bilinear proxy, so the resolve's own ratio is the transported
    // edit. Divisor family: that resolve takes the model's colour itself, so the gain goes on this pixel's
    // own proxy; the frame keeps its detail and an unchanged model output gives the frame back. SDR: the
    // gain on the frame's own pixel, for the same reason.
    float3 neuralUp;
    if (encoding == 0u) neuralUp = linearFrame >= kRatioFloor ? linearFrame * gain : linearFrame + kRatioFloor * (gain - 1.0);
    else if (curve == 1u) neuralUp = max(proxyUp, kRatioFloor) * gain;
    else {
        proxyUp = saturate(Proxy(Guarded(linearFrame)));
        neuralUp = proxyUp >= kRatioFloor ? proxyUp * gain : proxyUp + kRatioFloor * (gain - 1.0);
    }
    WriteResolved(pixel, ResolveCore(saturate(proxyUp), saturate(neuralUp), frame.rgb), frame.a);
}

)" R"(
// ----------------------------------------------------------------------------------------- pedestal
// Per 32x32 block: mean luminance of the resolved frame, mean and maximum luminance of the untouched one.
[numthreads(8, 8, 1)]
void Reduce(uint3 id : SV_DispatchThreadID) {
    uint2 size = uint2(w, h);
    uint2 base = id.xy * 32u;
    if (base.x >= size.x || base.y >= size.y) return;
    float finalSum = 0.0, originalSum = 0.0, originalMax = 0.0, n = 0.0;
    for (uint y = 0u; y < 32u; ++y)
        for (uint x = 0u; x < 32u; ++x) {
            uint2 p = base + uint2(x, y);
            if (p.x >= size.x || p.y >= size.y) continue;
            float o = Luminance(max(LoadFrame(int2(p)).rgb, 0.0));
            finalSum += Luminance(tResolved.Load(int3(p, 0)).rgb);
            originalSum += o; originalMax = max(originalMax, o); n += 1.0;
        }
    uOut[id.xy] = float4(finalSum / max(n, 1.0), originalSum / max(n, 1.0), originalMax, 1.0);
}

[numthreads(8, 8, 1)]
void Commit(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    float4 value = tResolved.Load(int3(id.xy, 0));
    float3 result = value.rgb;
    int2 blocks = int2((w + 31u) / 32u, (h + 31u) / 32u);
    float2 pos = (float2(id.xy) + 0.5) / 32.0 - 0.5;
    int2 b0 = clamp(int2(floor(pos)), 0, blocks - 1);
    int2 b1 = min(b0 + 1, blocks - 1);
    float2 f = saturate(pos - float2(b0));
    float4 s00 = tBlocks.Load(int3(b0, 0)), s10 = tBlocks.Load(int3(b1.x, b0.y, 0));
    float4 s01 = tBlocks.Load(int3(b0.x, b1.y, 0)), s11 = tBlocks.Load(int3(b1, 0));
    float4 stat = s00 * (1 - f.x) * (1 - f.y) + s10 * f.x * (1 - f.y) + s01 * (1 - f.x) * f.y + s11 * f.x * f.y;
    float unit = (flags & kGateInInputUnits) ? max(uScale[uint2(0, 0)].r, 1e-8) : 1.0;
    float weight = BlackRestoreWeight(stat.z, darkGate * unit, darkGateMode == 2u);
    if (weight > 0.0) {
        float pedestal = clamp(stat.x - stat.y, 0.0, pedestalCap * unit) * transferStrength * pedestalGate * weight;
        if (pedestal > 0.0) result = max(result - pedestal, 0.0);
    }
    WriteFinal(int2(id.xy), float4(EncodeFrame(result), value.a));
}

// ------------------------------------------------------------------------------------ settings panel
// Fallback for when the panel cannot be drawn at Present (present.cpp): it is drawn into the frame itself.
// The game's exposure, bloom and tone mapping then apply to it, so its brightness follows the scene.
// Geometric mean luminance of the frame, from a sparse grid that avoids the panel's own rectangle.
float SceneKey() {
    float sum = 0.0, n = 0.0;
    for (uint j = 0u; j < 8u; j++)
        for (uint i = 0u; i < 8u; i++) {
            uint2 at = uint2((i * 2u + 1u) * w / 16u, (j * 2u + 1u) * h / 16u);
            if (at.x >= panelPos.x && at.x < panelPos.x + panelSize.x && at.y >= panelPos.y && at.y < panelPos.y + panelSize.y) continue;
            float3 v = Guarded(RawFrame(int2(at)).rgb);
            sum += log2(clamp(Luminance(max(v, 0.0)), 1e-6, 65504.0)); n += 1.0;
        }
    return n > 0.0 ? exp2(sum / n) : 0.18;
}
[numthreads(8, 8, 1)]
void Overlay(uint3 id : SV_DispatchThreadID) {
    if (id.x >= panelSize.x || id.y >= panelSize.y) return;
    uint2 target = panelPos + id.xy;
    if (target.x >= w || target.y >= h) return;
    float4 c = tPanel.Load(int3(id.xy, 0));
    if (c.a < 0.5) return;
    // In a linear frame the panel's white follows the scene itself: the divisor never goes below 1, so
    // in a dark scene it would put the panel a hundred times above the picture and the game's own
    // exposure and bloom would wash it out. The factor was set by eye on a night scene: the game's exposure
    // is not known here, which is why the panel is normally drawn at Present instead.
    float level = encoding == 1u ? 21.1 * SceneKey() : (encoding == 3u ? 1.0 : Divisor());
    float3 value = encoding == 0u ? c.rgb : EncodeFrame(SrgbDecode(c.rgb) * panelWhite * level);
    WriteFinal(int2(target), float4(value, 1.0));
}
)";
