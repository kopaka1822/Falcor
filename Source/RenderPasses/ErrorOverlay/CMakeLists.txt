add_plugin(ErrorOverlay)

target_sources(ErrorOverlay PRIVATE
    ErrorOverlay.cpp
    ErrorOverlay.h
    ErrorOverlay.cs.slang
	ErrorOverlayEnums.slang
)

target_copy_shaders(ErrorOverlay RenderPasses/ErrorOverlay)
target_source_group(ErrorOverlay "RenderPasses")
