add_plugin(ShadowPathTracer)

target_sources(ShadowPathTracer PRIVATE
	ShadowMap.cpp
	ShadowMap.h
    ShadowPathTracer.cpp
    ShadowPathTracer.h
	Shaders/ShadowMap.3d.slang
	Shaders/PathTrace.rt.slang
	Shaders/ShadowMapFunctions.slang
	Shaders/ReflectTypes.cs.slang
)

target_copy_shaders(ShadowPathTracer RenderPasses/ShadowPathTracer)
target_source_group(ShadowPathTracer "RenderPasses")
