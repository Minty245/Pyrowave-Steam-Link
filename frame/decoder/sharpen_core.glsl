// Steam Link Pyrowave v7.21: the inset sharpening (contrast-adaptive sharpening, CAS) of sharpen.comp, shared with
// pack_rgba10_fx.comp (full colour: sharpening folded into the pack, no yf write and read-back). Needs uY
// (texture2D, samplerless) declared by the includer. Returns the sharpened value, not yet quantised.
float cas_sharpen(ivec2 p, ivec2 sz, int T, float sharpen, float r0, float r1)
{
    int ylo = (p.y / T) * T, yhi = min(ylo + T, sz.y) - 1;
    // clamp the neighbourhood to the current tile (never mix two views), like postfx.comp
    int xm = max(p.x - 1, 0), xp = min(p.x + 1, sz.x - 1);
    int ym = max(p.y - 1, ylo), yp = min(p.y + 1, yhi);
    float e = texelFetch(uY, p, 0).r;
    float o = e;
    vec2 lp = vec2(p.x, p.y - ylo) + 0.5 - 0.5 * float(T);
    float r = length(lp) / (0.5 * float(T));
    float k = 1.0 - smoothstep(r0, r1, r);
    if (k > 0.0) {
        float b = texelFetch(uY, ivec2(p.x, ym), 0).r, d = texelFetch(uY, ivec2(xm, p.y), 0).r;
        float f = texelFetch(uY, ivec2(xp, p.y), 0).r, h = texelFetch(uY, ivec2(p.x, yp), 0).r;
        float a = texelFetch(uY, ivec2(xm, ym), 0).r, cc = texelFetch(uY, ivec2(xp, ym), 0).r;
        float g = texelFetch(uY, ivec2(xm, yp), 0).r, ii = texelFetch(uY, ivec2(xp, yp), 0).r;
        float mn = min(min(min(d, e), min(f, b)), h);
        float mn2 = min(mn, min(min(a, cc), min(g, ii)));
        mn += mn2;
        float mx = max(max(max(d, e), max(f, b)), h);
        float mx2 = max(mx, max(max(a, cc), max(g, ii)));
        mx += mx2;
        float amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
        float peak = -1.0 / mix(8.0, 5.0, clamp(sharpen * k, 0.0, 1.0));
        float w = amp * peak;
        o = clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
    }
    return o;
}
