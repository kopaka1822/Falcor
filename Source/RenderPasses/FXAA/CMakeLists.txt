add_plugin(FXAA)

target_sources(FXAA PRIVATE
    FXAA.cpp
    FXAA.h
    FXAA.ps.slang
)

target_copy_shaders(FXAA RenderPasses/FXAA)

target_source_group(FXAA "RenderPasses")
