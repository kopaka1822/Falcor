add_plugin(MinimalPathTracerShadowMap)

target_sources(MinimalPathTracerShadowMap PRIVATE
    MinimalPathTracerShadowMap.cpp
    MinimalPathTracerShadowMap.h
    MinimalPathTracerShadowMap.rt.slang
)

target_copy_shaders(MinimalPathTracerShadowMap RenderPasses/MinimalPathTracerShadowMap)

target_source_group(MinimalPathTracerShadowMap "RenderPasses")
