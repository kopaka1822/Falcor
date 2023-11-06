add_plugin(ShadowPass)

target_sources(ShadowPass PRIVATE
    ShadowPass.cpp
    ShadowPass.h
	ShadowPass.rt.slang
	ShadowPassData.slang
)

target_source_group(ShadowPass "RenderPasses")
