// Steam Link Pyrowave v7.21: the wide-view blur, shared by wideblur_apply.comp (in place, 8- or 16-bit) and
// pack_rgba10_fx.comp (full colour: the blur is folded into the pack, so the blurred luma is never written to and
// read back from the 16-bit plane). Same arithmetic as v7.17's wideblur_apply.comp, with one change that does not
// alter the result: a pyramid level read by both the blur and the contrast term is sampled once (cached), not twice.
// Needs, declared by the includer: uMip (sampler2DArray, the Gaussian pyramid) and ioHist (r16f image2DArray), and
// ramp.glsl included before this file.

// cubic B-spline read of one pyramid level (4 bilinear taps; smooth and shift-stable, unlike bilinear)
float wb_bspline(vec2 uv, float layer, int lev, float tile)
{
    // level size from the tile size (textureSize with a per-pixel level is not reliable on every driver)
    vec2 size = vec2(float((int(tile) / 2) >> lev));
    vec2 t = uv * size - 0.5, i = floor(t), f = t - i;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 p0 = (i - 0.5 + w1 / g0) / size, p1 = (i + 1.5 + w3 / g1) / size;
    float l = float(lev);
    return g0.y * (g0.x * textureLod(uMip, vec3(p0.x, p0.y, layer), l).r + g1.x * textureLod(uMip, vec3(p1.x, p0.y, layer), l).r) +
           g1.y * (g0.x * textureLod(uMip, vec3(p0.x, p1.y, layer), l).r + g1.x * textureLod(uMip, vec3(p1.x, p1.y, layer), l).r);
}

// v7.21: the last two levels read for this pixel
struct WbCache { int la, lb; float va, vb; };

float wb_level(vec2 uv, float layer, int lev, float tile, inout WbCache c)
{
    if (c.la == lev) return c.va;
    if (c.lb == lev) return c.vb;
    float v = wb_bspline(uv, layer, lev, tile);
    c.lb = c.la; c.vb = c.va;
    c.la = lev; c.va = v;
    return v;
}

// Blur level L: 0 = the decoded pixel, 1 = pyramid level 0 (half size), 2 = level 1, ...; in between blends the two
// neighbouring levels.
float wb_blurred(vec2 uv, float layer, float orig, float L, int nlev, float tile, inout WbCache c)
{
    if (L <= 0.0) return orig;
    int j = int(floor(L)) - 1;
    float f = L - floor(L);
    if (j + 1 >= nlev) return wb_level(uv, layer, nlev - 1, tile, c);
    float a = orig;
    if (j >= 0) a = wb_level(uv, layer, j, tile, c);
    if (f <= 0.0) return a;
    return mix(a, wb_level(uv, layer, j + 1, tile, c), f);
}

// The filtered value (not yet quantised) of wide-view pixel l (view coordinates) of view z (0 = tile 1, 1 = tile 3).
// lsb = one output step (1/255 or 1/1023). Reads and writes the history when temporal > 0.
float wb_apply(ivec2 l, int z, float orig, float tile, float px0, float contrast, float max_lod, float temporal,
               float tclamp, float hist_ok, float lsb, vec4 rp, vec4 pj0, vec4 pj1, vec4 cen)
{
    vec2 uv = (vec2(l) + 0.5) / tile;
    // v7.14: the same blur in degrees everywhere (more pixels per degree towards the edge), times the shared ramp;
    // contrast restoration ramps with it
    // v7.16: scale in px first, then turn px into a blur level: linear up to 1 px (0.5 px = halfway to the first
    // pyramid level), log2(2 px) above, so every slider step down to 0 does something
    float m = ramp_mult(uv, z, rp, pj0, pj1, cen);
    float px = px0 * m * ramp_density(uv, z, pj0, pj1);
    float L = min(px < 1.0 ? px : log2(2.0 * px), max_lod);
    float k = min(contrast * m, 3.0);
    int nlev = int(max_lod + 2.0);           // max_lod = levels - 1.5
    WbCache c = WbCache(-1, -1, 0.0, 0.0);
    float b = wb_blurred(uv, float(z), orig, L, nlev, tile, c);
    float b2 = wb_blurred(uv, float(z), orig, L + 1.5, nlev, tile, c);
    float o = b + k * (b - b2);
    // never past the decoded pixel, unless overshoot (rp.w) allows a fraction of the difference beyond it
    float d = abs(orig - b) * rp.w;
    o = clamp(o, min(b, orig) - d, max(b, orig) + d);
    // v7.17: headset stability. Every frame is coded on its own, so at low bitrates the periphery's coding errors
    // change from frame to frame (flicker). Blend in last frame's result away from the gaze, held within this
    // pixel's own range: between it, its blur and its result, widened by the local contrast |orig - b| (+ 2 steps
    // for the flicker itself). Neighbours are not read (the 8-bit pass writes in place), so the contrast term
    // stands in for the neighbourhood spread.
    if (temporal > 0.0)
    {
        ivec3 hp = ivec3(l, z);
        if (hist_ok > 0.5)
        {
            float ang = ramp_gaze_angle(uv, z, pj0, pj1, cen);
            float tw = 0.85 * temporal * smoothstep(rp.y, rp.y + 10.0, ang);
            if (tw > 0.0)
            {
                float s = tclamp * (abs(orig - b) + 2.0 * lsb);
                float h = clamp(imageLoad(ioHist, hp).r, min(min(o, b), orig) - s, max(max(o, b), orig) + s);
                o = mix(o, h, tw);
            }
        }
        imageStore(ioHist, hp, vec4(o));
    }
    return o;
}
