add_plugin(ShadowPathTracer)

target_sources(ShadowPathTracer PRIVATE
    ShadowPathTracer.cpp
    ShadowPathTracer.h
	PathTrace.rt.slang
)

target_copy_shaders(ShadowPathTracer RenderPasses/ShadowPathTracer)
target_source_group(ShadowPathTracer "RenderPasses")
