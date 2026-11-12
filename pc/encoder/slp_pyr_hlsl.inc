R"SLPHLSL(// Steam Link Pyrowave v46: one level of the Gaussian pyramid of the atlas (replaces GenerateMips' 2x2 box chain).
// Output pixel = 6-tap binomial low-pass (1 5 10 10 5 1)/32 of the level above, centred between the two source
// pixels it covers, so detail above the new Nyquist is removed before halving (no aliasing; the blur built from it
// moves smoothly with the picture). Rows stay inside their atlas tile (the four views never mix).
Texture2D<float4> gSrc : register(t0);    // level k - 1
RWTexture2D<float4> gDst : register(u0);  // level k

static const float kW[6] = {1.0, 5.0, 10.0, 10.0, 5.0, 1.0};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h, sw, sh;
    gDst.GetDimensions(w, h);
    gSrc.GetDimensions(sw, sh);
    if (id.x >= w || id.y >= h)
        return;
    uint tile = min(3u, id.y * 4u / h);
    int r0 = int(tile * sh / 4u), r1 = int((tile + 1u) * sh / 4u) - 1;
    int2 b = int2(id.xy) * 2 - 2;
    float4 acc = 0.0;
    [unroll] for (int j = 0; j < 6; ++j)
    {
        int y = clamp(b.y + j, r0, r1);
        float4 row = 0.0;
        [unroll] for (int i = 0; i < 6; ++i)
            row += kW[i] * gSrc.Load(int3(clamp(b.x + i, 0, int(sw) - 1), y, 0));
        acc += kW[j] * row;
    }
    gDst[id.xy] = acc * (1.0 / 1024.0);
}
)SLPHLSL"