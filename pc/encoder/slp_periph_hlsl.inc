R"SLPHLSL(// Steam Link Pyrowave - peripheral prefilter (contrast-aware foveated blur) for the vrlink atlas.
// SPDX-License-Identifier: MIT
//
// The atlas is 4 stacked square views: tiles 0/1 = eye 0 (inset, wide), tiles 2/3 = eye 1. Even tiles are the
// gaze-centred insets, odd tiles the wide views.
// Each pixel is blurred according to its eccentricity from the gaze (the inset centre), matched to how fast
// human acuity falls off, and then gets some of the lost contrast back (Patney et al. 2016). The part of each
// wide view that the high-res inset covers in the headset is blurred heavily (it is never seen).
// Blur = trilinear sampling of a box-filtered mip chain of the atlas (GenerateMips).

cbuffer Params : register(b0)
{
    float4 c0; // x tile size px, y wide-view FOV deg, z inset scale (inset FOV / wide FOV), w locator valid (0/1)
    float4 c1; // inset centre in the wide view, px: eye 0 (x, y), eye 1 (x, y)
    float4 c2; // x safe radius deg (full detail), y ramp deg, z acuity cpd at the fovea, w acuity e2 deg
    float4 c3; // x strength (lod multiplier), y max lod, z contrast restore, w blur inside the inset (0/1)
    float4 c4; // x hide (0/1), y hide margin (fraction), z hide lod, w safe radius deg when the locator is invalid
    float4 c5; // x minimum blur (lod) on the wide views, y current gaze known (0/1), z v54: colour blur keeps linear luminance (0/1)
    float4 c6; // current gaze in the wide view, px: eye 0 (x, y), eye 1 (x, y) (v31)
    float4 c7; // v33: lens centre (straight ahead) in the wide view, px: eye 0 (x, y), eye 1 (x, y)
    float4 c8; // v33: x lens radius px (0 = off), y lens feather px, z extra chroma lod, w contrast clamp (0/1)
    float4 c9;  // v35: hide box eye 0: centre (x, y), half size (x, y), px (where the inset is in every plausible position)
    float4 c10; // v35: hide box eye 1
    float4 c11; // v45: eye 0 projection: px = (ax * tan_x + bx, ay * tan_y + by): (ax, bx, ay, by)
    float4 c12; // v45: eye 1 projection
    float4 c13; // v45: x edge ramp (0..1), y ramp start deg, z ramp end deg, w true angles in the wide views (0/1)
    float4 c14; // v45: x contrast overshoot (0..1, with the clamp on); v47: y periphery stability (0..1),
                //      z history valid (0/1), w history clamp width (in local standard deviations)
};

Texture2D<float4> gMip : register(t0);
Texture2D<float4> gHist : register(t1);   // v47: last frame's output (periphery stability)
SamplerState gLin : register(s0);
RWTexture2D<float4> gOut : register(u0);

// v45: the wide views are flat (tangent-space) projections. A pixel near the edge spans far fewer degrees than one
// in the middle, and the angle from the gaze is not proportional to the pixel distance.
float2 tan_of(float2 p, float4 pr) { return float2((p.x - pr.y) / pr.x, (p.y - pr.w) / pr.z); }
float angle_deg(float2 ta, float2 tb)
{
    float3 a = float3(ta, 1.0), b = float3(tb, 1.0);
    return degrees(atan2(length(cross(a, b)), dot(a, b)));
}
// degrees per pixel at tangent position t (geometric mean of the radial and tangential sizes)
float degpp_at(float2 t, float4 pr)
{
    float g = 1.0 + dot(t, t);
    return degrees(1.0 / (0.5 * (abs(pr.x) + abs(pr.z)))) * pow(g, -0.75);
}
// v45: shared "ramp to the edge": 1 at the ramp start (about the inset edge), 1 + 2 * ramp at the ramp end
float edge_mult(float e_deg)
{
    return 1.0 + 2.0 * c13.x * saturate((e_deg - c13.y) / max(c13.z - c13.y, 1.0));
}

// sRGB transfer, decode (v54)
float3 srgb_lin(float3 c)
{
    return c <= 0.04045 ? c * (1.0 / 12.92) : pow((c + 0.055) * (1.0 / 1.055), 2.4);
}

float lod_for(float e_deg, float nyq_cpd, float safe_deg)
{
    if (e_deg <= safe_deg)
        return 0.0;
    float cpd_eye = c2.z / (1.0 + e_deg / c2.w);           // resolvable cycles/degree at this eccentricity
    float l = log2(max(nyq_cpd / cpd_eye, 1.0)) * c3.x;
    l *= saturate((e_deg - safe_deg) / max(c2.y, 0.01)); // ease in after the safe radius
    return min(l, c3.y);
}

// v46: cubic B-spline read of pyramid level k (4 bilinear taps: smooth and shift-stable, unlike plain bilinear)
float4 bspline_at(float2 uv, uint k, uint W, uint H)
{
    float2 size = float2(W >> k, H >> k);
    float2 t = uv * size - 0.5, i = floor(t), f = t - i;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    float2 g0 = w0 + w1, g1 = w2 + w3;
    float2 p0 = (i - 0.5 + w1 / g0) / size, p1 = (i + 1.5 + w3 / g1) / size;
    float l = float(k);
    return g0.y * (g0.x * gMip.SampleLevel(gLin, float2(p0.x, p0.y), l) + g1.x * gMip.SampleLevel(gLin, float2(p1.x, p0.y), l)) +
           g1.y * (g0.x * gMip.SampleLevel(gLin, float2(p0.x, p1.y), l) + g1.x * gMip.SampleLevel(gLin, float2(p1.x, p1.y), l));
}

// v46: Gaussian blur of level L (0 = the pixel itself, 1 = half size, ...): neighbouring pyramid levels blended
static const uint kTopLevel = 6;   // pyramid levels 0..6 (kLocMip + 3 in slp_periph.cpp)
float4 gauss(float2 uv, float L, float4 src, uint W, uint H)
{
    L = clamp(L, 0.0, float(kTopLevel));
    uint k = uint(floor(L));
    float f = L - float(k);
    float4 a = k == 0 ? src : bspline_at(uv, k, W, H);
    if (f <= 0.0 || k >= kTopLevel)
        return a;
    return lerp(a, bspline_at(uv, k + 1, W, H), f);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint W, H;
    gOut.GetDimensions(W, H);
    if (id.x >= W || id.y >= H)
        return;
    float T = c0.x;
    uint tile = id.y / (uint)T;
    float2 lp = float2(id.x, (float)id.y - tile * T) + 0.5;
    bool fovea = (tile & 1) == 0;   // even tiles = gaze insets, odd tiles = wide views
    bool valid = c0.w > 0.5;
    float lod = 0.0;
    float tw = 0.0;   // v47: weight of last frame's output (wide views, away from the gaze)
    if (fovea)
    {
        if (c3.w > 0.5)
        {
            float degpp = c0.y * c0.z / T;
            float e = length(lp - T * 0.5) * degpp;
            lod = lod_for(e, 0.5 / degpp, c2.x);
        }
    }
    else
    {
        float degpp = c0.y / T;
        float2 cen = (tile < 2) ? c1.xy : c1.zw;
        if (!valid)
            cen = float2(T * 0.5, T * 0.5);
        float e = length(lp - cen) * degpp;
        // v31: the inset follows the gaze a few frames late. Keep full detail around where the eye is now as well,
        // so after a saccade the wide view under the new gaze point is not blurred while the inset catches up.
        if (c5.y > 0.5)
            e = min(e, length(lp - ((tile < 2) ? c6.xy : c6.zw)) * degpp);
        if (c13.w > 0.5)
        {
            // v45: true angles and the local pixel size from the eye's projection
            float4 pr = (tile < 2) ? c11 : c12;
            float2 tp = tan_of(lp, pr);
            e = angle_deg(tp, tan_of(cen, pr));
            if (c5.y > 0.5)
                e = min(e, angle_deg(tp, tan_of((tile < 2) ? c6.xy : c6.zw, pr)));
            degpp = degpp_at(tp, pr);
        }
        lod = lod_for(e, 0.5 / degpp, valid ? c2.x : c4.w);
        // v47: periphery stability fades in over the 10 degrees after the ramp start (about the visible inset edge)
        tw = 0.85 * c14.y * smoothstep(c13.y, c13.y + 10.0, e);
        // v45: the shared ramp scales the blur radius (also where the view is resolution-limited), past max_lod
        lod = min(lod + log2(edge_mult(e)), 6.0);
        // The part of the wide view under the high-res inset is never shown: blur it hard. Soft edge (v28): the
        // locator's estimate lags the inset by a few frames and jitters by a few px, so a hard boundary would
        // shimmer where the compositor blends inset and wide view.
        // v35: the hidden area is the part of the wide view the inset covers at every gaze time it may have been
        // placed with (a box from the CPU: intersection of the inset squares over a window of gaze samples)
        float4 hb = (tile < 2) ? c9 : c10;
        float2 q = abs(lp - hb.xy) / max(hb.zw, 1.0);
        float d = (hb.z > 0.5 && hb.w > 0.5) ? max(q.x, q.y) : 9.0;   // 1 = box edge
        float inner = 1.0 - c4.y;                              // hard blur inside this
        float t = (d < inner) ? 1.0 : saturate(1.0 - (d - inner) / 0.2);
        if (valid && c4.x > 0.5)
            lod = lerp(lod, max(lod, c4.z), t * t);
        lod = max(lod, c5.x);
        // v33: outside the lens the view is never seen: blur to (almost) flat, which costs (almost) no bits.
        if (c8.x > 0.5)
        {
            float r = length(lp - ((tile < 2) ? c7.xy : c7.zw));
            float u = saturate((r - c8.x) / max(c8.y, 1.0));
            lod = lerp(lod, max(lod, 6.0), u);
        }
    }

    float4 src = gMip.Load(int3(id.xy, 0));
    float4 o = src;
    if (lod >= 0.01)
    {
    float2 uv = (float2(id.xy) + 0.5) / float2(W, H);
    // Keep the filter footprint inside this tile (the mips of the stacked atlas would mix neighbouring views).
    // v46: the pyramid levels stay inside their tile; only the B-spline / bilinear taps (~2.5 texels) need room
    float margin = 2.5 * exp2(ceil(min(lod + 1.5, float(kTopLevel))));
    uv.y = clamp(uv.y, (tile * T + margin) / H, ((tile + 1) * T - margin) / H);
    float4 b0 = gauss(uv, lod, src, W, H);
    float4 b1 = gauss(uv, lod + 1.5, src, W, H);
    // Contrast restoration: put back part of the band just below the new cut-off.
    o = b0 + c3.z * saturate(lod) * (b0 - b1);
    // v33: never restore past the original pixel (no halos/overshoot on strong edges such as bright text)
    // v45: overshoot lets it go a fraction of the restored difference past the original (Patney-style unsharp mask)
    if (c8.w > 0.5)
    {
        float3 d = abs(src.rgb - b0.rgb) * c14.x;
        o.rgb = clamp(o.rgb, min(src.rgb, b0.rgb) - d, max(src.rgb, b0.rgb) + d);
    }
    // v33: colour detail falls off faster than brightness detail away from the gaze: blur chroma further
    if (c8.z > 0.01)
    {
        float4 bc = gauss(uv, min(lod + c8.z * saturate(lod), 6.0), src, W, H);
        const float3 w = float3(0.2126, 0.7152, 0.0722);
        float d = dot(o.rgb, w) - dot(bc.rgb, w);
        // v54: keeping the gamma-encoded luma (above) darkens saturated edges and thin coloured detail once their
        // colour is spread out, like plain 4:2:0 does. Instead solve the offset so the pixel keeps its linear
        // luminance (Steam Link's luma adjustment, applied to the blurred colour). Skipped where the colour barely
        // changed.
        float3 cd = o.rgb - (bc.rgb + d);
        if (c5.z > 0.5 && max(abs(cd.r), max(abs(cd.g), abs(cd.b))) > 1.0 / 1024.0)
        {
            float target = dot(srgb_lin(saturate(o.rgb)), w);
            float lo = d - 1.0, hi = d + 1.0;
            [loop] for (int it = 0; it < 12; ++it)
            {
                float m = 0.5 * (lo + hi);
                if (dot(srgb_lin(saturate(bc.rgb + m)), w) < target) lo = m; else hi = m;
            }
            d = 0.5 * (lo + hi);
        }
        o.rgb = bc.rgb + d;
    }
    o.rgb = max(o.rgb, 0.0);
    o.a = src.a;
    }
    // v47: periphery stability. Blend in last frame's output, clamped to the spread of this frame's 3x3
    // neighbourhood (widened to include this pixel's own result) so moving things do not leave trails. Calms
    // aliasing and codec-independent crawl that a per-frame blur cannot; no motion vectors (head motion beyond the
    // clamp simply falls back to this frame).
    if (tw > 0.0 && c14.z > 0.5)
    {
        int y0 = int(tile * T), y1 = int((tile + 1) * T) - 1;
        float3 m1 = 0.0, m2 = 0.0;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                float3 v = gMip.Load(int3(clamp(int(id.x) + dx, 0, int(W) - 1), clamp(int(id.y) + dy, y0, y1), 0)).rgb;
                m1 += v;
                m2 += v * v;
            }
        m1 *= 1.0 / 9.0;
        float3 sd = sqrt(max(m2 * (1.0 / 9.0) - m1 * m1, 0.0)) * c14.w;
        float3 h = gHist.Load(int3(id.xy, 0)).rgb;
        h = clamp(h, min(o.rgb, m1) - sd, max(o.rgb, m1) + sd);
        o.rgb = lerp(o.rgb, h, tw);
    }
    gOut[id.xy] = o;
}
)SLPHLSL"
