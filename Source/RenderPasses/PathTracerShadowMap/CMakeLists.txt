add_plugin(PathTracerShadowMap)

target_sources(PathTracerShadowMap PRIVATE
    ColorType.slang
    GeneratePaths.cs.slang
    GuideData.slang
    LoadShadingData.slang
    NRDHelpers.slang
    Params.slang
    PathState.slang
    PathTracer.slang
    PathTracerShadowMap.cpp
    PathTracerShadowMap.h
    PathTracerNRD.slang
    ReflectTypes.cs.slang
    ResolvePass.cs.slang
    StaticParams.slang
    TracePass.rt.slang
)

target_copy_shaders(PathTracerShadowMap RenderPasses/PathTracerShadowMap)

target_source_group(PathTracerShadowMap "RenderPasses")
