add_plugin(DepthGuide)

target_sources(DepthGuide PRIVATE
    DepthGuide.cpp
    DepthGuide.h
    DepthGuide.ps.slang
    MinMaxRange.ps.slang
)

target_copy_shaders(DepthGuide RenderPasses/DepthGuide)

target_source_group(DepthGuide "RenderPasses")
