add_plugin(TestShadowMap)

target_sources(TestShadowMap PRIVATE
    TestShadowMap.cpp
    TestShadowMap.h
    TestShadowMap.rt.slang
    GenerateShadowMap.rt.slang
    SMStructs.slang
    DebugShadowMap.cs.slang
    GenMinMaxMips.cs.slang
)


target_copy_shaders(TestShadowMap RenderPasses/TestShadowMap)
target_source_group(TestShadowMap "RenderPasses")
