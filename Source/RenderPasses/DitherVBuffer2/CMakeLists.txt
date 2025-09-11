add_plugin(DitherVBuffer2)

target_sources(DitherVBuffer2 PRIVATE
    DitherVBuffer2.cpp
    DitherVBuffer2.h
    DitherVBuffer2.rt.slang
    Dither2.slangh

)

target_copy_shaders(DitherVBuffer2 RenderPasses/DitherVBuffer2)

target_source_group(DitherVBuffer2 "RenderPasses")
