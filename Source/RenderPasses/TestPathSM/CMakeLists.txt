add_plugin(TestPathSM)

target_sources(TestPathSM PRIVATE
    TestPathSM.cpp
    TestPathSM.h
    TestPathSM.rt.slang
    GenerateShadowMap.rt.slang
)


target_copy_shaders(TestPathSM RenderPasses/TestPathSM)
target_source_group(TestPathSM "RenderPasses")
