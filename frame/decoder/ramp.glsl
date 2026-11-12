// Steam Link Pyrowave v7.14: the shared, gaze-centred "ramp to the edge" (same curve as the PC prefilter, v45).
// The PC sends, with every frame, where each eye's inset is and the eye's projection (SLPR block). Positions are in
// tile units (0..1 across the wide view); a pixel at (x, y) looks along tan = ((x - bx) / ax, (y - by) / ay).
// Needs in the push constants: vec4 rp (ramp, start deg, end deg, overshoot), vec4 pj0 / pj1 (ax, bx, ay, by per
// eye), vec4 cen (eye 0 cx, cy, eye 1 cx, cy).
vec2 ramp_tan(vec2 p, vec4 pr) { return vec2((p.x - pr.y) / pr.x, (p.y - pr.w) / pr.z); }

float ramp_angle(vec2 ta, vec2 tb)
{
    vec3 a = vec3(ta, 1.0), b = vec3(tb, 1.0);
    return degrees(atan(length(cross(a, b)), dot(a, b)));
}

// multiplier at tile position p of eye e: 1 inside `start`, 1 + 2 * ramp at `end` and beyond
float ramp_mult(vec2 p, int e, vec4 rp, vec4 pj0, vec4 pj1, vec4 cen)
{
    if (rp.x <= 0.0) return 1.0;
    vec4 pr = e == 0 ? pj0 : pj1;
    vec2 c = e == 0 ? cen.xy : cen.zw;
    float ang = ramp_angle(ramp_tan(p, pr), ramp_tan(c, pr));
    return 1.0 + 2.0 * rp.x * clamp((ang - rp.y) / max(rp.z - rp.y, 1.0), 0.0, 1.0);
}

// v7.17: angle (deg) from the inset centre at tile position p of eye e
float ramp_gaze_angle(vec2 p, int e, vec4 pj0, vec4 pj1, vec4 cen)
{
    vec4 pr = e == 0 ? pj0 : pj1;
    return ramp_angle(ramp_tan(p, pr), ramp_tan(e == 0 ? cen.xy : cen.zw, pr));
}

// local pixels per degree relative to the view's optical centre (a flat projection packs more pixels into each
// degree towards the edge)
float ramp_density(vec2 p, int e, vec4 pj0, vec4 pj1)
{
    vec2 t = ramp_tan(p, e == 0 ? pj0 : pj1);
    return pow(1.0 + dot(t, t), 0.75);
}
