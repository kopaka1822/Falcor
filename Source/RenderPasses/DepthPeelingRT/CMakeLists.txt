add_plugin(DepthPeelingRT)

target_sources(DepthPeelingRT PRIVATE
    DepthPeelingRT.cpp
    DepthPeelingRT.h
    DepthPeelingRT.rt.slang
)

target_copy_shaders(DepthPeelingRT RenderPasses/DepthPeelingRT)

target_source_group(DepthPeelingRT "RenderPasses")
