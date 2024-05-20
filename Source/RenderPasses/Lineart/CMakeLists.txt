add_plugin(Lineart)

target_sources(Lineart PRIVATE
    Lineart.cpp
    Lineart.h
    Lineart.ps.slang
)

target_copy_shaders(Lineart RenderPasses/Lineart)

target_source_group(Lineart "RenderPasses")
