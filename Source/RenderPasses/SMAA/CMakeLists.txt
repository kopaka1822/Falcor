add_plugin(SMAA)

target_sources(SMAA PRIVATE
    SMAA.cpp
    SMAA.h
    AreaTex.h
    SearchTex.h
    SMAA.hlsl
    Pass1.hlsl
    Pass2.hlsl
    Pass3.hlsl
)

target_copy_shaders(SMAA RenderPasses/SMAA)

target_source_group(SMAA "RenderPasses")
