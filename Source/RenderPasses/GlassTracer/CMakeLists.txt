add_plugin(GlassTracer)

target_sources(GlassTracer PRIVATE
    GlassTracer.cpp
    GlassTracer.h
    GlassTracer.rt.slang
    Shared.slang
    IterateMV.rt.slang
    OpticalFlow.cs.slang
)

target_copy_shaders(GlassTracer RenderPasses/GlassTracer)

target_source_group(GlassTracer "RenderPasses")
