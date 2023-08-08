add_plugin(ShadowPathTracer)

target_sources(ShadowPathTracer PRIVATE
	ShadowMap.cpp
	ShadowMap.h
    ShadowPathTracer.cpp
    ShadowPathTracer.h
	Shaders/GenerateShadowMap.3d.slang
	Shaders/PathTrace.rt.slang
	Shaders/ShadowMap.slang
	Shaders/ReflectTypes.cs.slang
)

target_copy_shaders(ShadowPathTracer RenderPasses/ShadowPathTracer)
target_source_group(ShadowPathTracer "RenderPasses")
