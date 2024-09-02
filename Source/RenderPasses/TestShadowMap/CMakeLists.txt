add_plugin(TestShadowMap)

target_sources(TestShadowMap PRIVATE
    TestShadowMap.cpp
    TestShadowMap.h
    TestShadowMap.rt.slang
    GenerateShadowMap.rt.slang
    ReverseSMGen.rt.slang
    SparseShadowMap.rt.slang
    SMStructs.slang
    DebugShadowMap.cs.slang
    GenMinMaxMips.cs.slang
    CreateRayNeededMask.cs.slang
    LayeredVariance.cs.slang
    VLVSM_CreateLayers.cs.slang
	VLVSM_EvaluateLayers.cs.slang
	VLVSM_Blur.cs.slang
)


target_copy_shaders(TestShadowMap RenderPasses/TestShadowMap)
target_source_group(TestShadowMap "RenderPasses")
