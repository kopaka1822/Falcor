add_plugin(RayTracedSoftShadows)

target_sources(RayTracedSoftShadows PRIVATE
	RayTracedSoftShadows.cpp
    RayTracedSoftShadows.h
	RayTracedSoftShadows.rt.slang
)

target_copy_shaders(RayTracedSoftShadows RenderPasses/RayTracedSoftShadows)
target_source_group(RayTracedSoftShadows "RenderPasses")
