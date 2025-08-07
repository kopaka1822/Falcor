// load fragments into registers
FragData f[MAX_FRAGMENT];

// insert from linked list
uint next = p.head;
uint count = 0;
[unroll] for (uint i = 0; i < MAX_FRAGMENT && next < maxElements; ++i)
{
    BufferData data = gBuffer[next];
    next = data.next;
    f[i].depth = data.depth;
    f[i].color = data.color;
    count++;
}

#if MAX_FRAGMENT < 32

// sort registers (insertion sort)
[unroll] for (uint i = 1; i < MAX_FRAGMENT && i < count; ++i)
{
    // i - 1 elements are sorted
    [unroll] for (uint j = i; j > 0 && f[j - 1].depth > f[j].depth; --j)
    {
        let tmp = f[j];
        f[j] = f[j - 1];
        f[j - 1] = tmp;
    }
}
#else
// sort registers (shell sort)
const uint gaps[] = { 111, 41, 13, 4, 1 };
const uint maxGaps = 5;
#if MAX_FRAGMENT <= 111
const uint startGap = 1;
#else
const uint startGap = 0;
#endif

[unroll] for (uint gapIdx = startGap; gapIdx < maxGaps; ++gapIdx)
{
    const int gap = gaps[gapIdx];
    for (uint i = gap; i < MAX_FRAGMENT && i < count; ++i)
    {
        for (uint j = i; j >= gap && f[j - gap].depth > f[j].depth; j -= gap)
        {
            let tmp = f[j];
            f[j] = f[j - gap];
            f[j - gap] = tmp;
        }
    }
}

#endif

float4 color = float4(0.0, 0.0, 0.0, 1.0);

// now blend together (front to back)
[unroll] for (uint i = 0; i < MAX_FRAGMENT && i < count; ++i)
{
    float4 c = unpackHalf4(f[i].color);
    color.rgb += color.a * c.a * c.rgb;
    color.a *= (1.0 - c.a);
}

gColor[DispatchRaysIndex().xy] = color;
