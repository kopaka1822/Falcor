add_plugin(UVMaps)

target_sources(UVMaps PRIVATE
    UVMaps.cpp
    UVMaps.h
    uvmaps.ps.slang
)

target_copy_shaders(UVMaps RenderPasses/UVMaps)

target_source_group(UVMaps "RenderPasses")
