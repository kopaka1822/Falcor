// load fragments into registers
FragData f[MAX_FRAGMENT];

// insert from linked list
uint next = p.head;
uint count = 0;
/* [unroll] for (uint i = 0; i < MAX_FRAGMENT && next < maxElements; ++i)
{
    BufferData data = gBuffer[next];
    next = data.next;
    f[i].depth = data.depth;
    f[i].color = data.color;
    count++;
}*/
[unroll] for (uint i = 0; i < MAX_FRAGMENT; ++i)
{
    if (next < maxElements)
    {
        BufferData data = gBuffer[next];
        next = data.next;
        f[i].depth = data.depth;
        f[i].color = data.color;
        count++;
    }
    else
    {
        f[i].depth = 3.402823466e+38f;
        f[i].color = 0;
    }
}

#if MAX_FRAGMENT <= 0

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
#elif MAX_FRAGMENT >= 32
// sort registers (shell sort)
const uint gaps[] = { 111, 41, 13, 4, 1 };
const uint maxGaps = 5;

[unroll] for (uint gapIdx = 0; gapIdx < maxGaps; ++gapIdx)
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

#else
// bitonic sort

[unroll]
for (uint size = 2; size <= MAX_FRAGMENT; size <<= 1)
{
    [unroll]
    for (uint stride = size >> 1; stride > 0; stride >>= 1)
    {
        [unroll]
        for (uint i = 0; i < MAX_FRAGMENT; ++i)
        {
            uint j = i ^ stride;
            if (j > i)
            {
                bool ascending = ((i & size) == 0);
                if ((f[i].depth > f[j].depth) == ascending)
                {
                    let tmp = f[i];
                    f[i] = f[j];
                    f[j] = tmp;
                }
            }
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
