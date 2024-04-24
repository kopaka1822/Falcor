add_plugin(LightTrace)

target_sources(LightTrace PRIVATE
    LightTrace.cpp
    LightTrace.h
    CollectBackProject.rt.slang
    TraceLight.rt.slang
)
target_copy_shaders(LightTrace RenderPasses/LightTrace)
target_source_group(LightTrace "RenderPasses")
