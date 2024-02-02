add_plugin(ShadowPass)

target_sources(ShadowPass PRIVATE
    ShadowPass.cpp
    ShadowPass.h
	ShadowPass.rt.slang
	ShadowPassData.slang
	HybridMask.slang
	HybridMaskSamplePatterns.slang
)

target_copy_shaders(ShadowPass RenderPasses/ShadowPass)

target_source_group(ShadowPass "RenderPasses")
