add_plugin(TransparencyPathTracer)

target_sources(TransparencyPathTracer PRIVATE
    TransparencyPathTracer.cpp
    TransparencyPathTracer.h
    TransparencyPathTracer.rt.slang
    SharedTypes.slang
)

target_copy_shaders(TransparencyPathTracer RenderPasses/TransparencyPathTracer)
target_source_group(TransparencyPathTracer "RenderPasses")
