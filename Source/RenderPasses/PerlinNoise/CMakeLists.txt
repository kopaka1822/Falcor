add_plugin(PerlinNoise)

target_sources(PerlinNoise PRIVATE
    PerlinNoise.cpp
    PerlinNoise.h
	PerlinNoise.cs.slang
)

target_copy_shaders(PerlinNoise RenderPasses/PerlinNoise)
target_source_group(PerlinNoise "RenderPasses")
