add_plugin(SDHBAO)

target_sources(SDHBAO PRIVATE
    SDHBAO.cpp
    SDHBAO.h
    HBAO1.slang
    HBAO2.slang
    Common.slang
    HBAOData.slang
)

target_copy_shaders(SDHBAO RenderPasses/SDHBAO)

target_source_group(SDHBAO "RenderPasses")
