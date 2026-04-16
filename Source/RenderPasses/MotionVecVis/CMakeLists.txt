add_plugin(MotionVecVis)

target_sources(MotionVecVis PRIVATE
    MotionVecVis.cpp
    MotionVecVis.h
    MotionVecVis.ps.slang
    PosDiff.ps.slang
    AngularDiff.ps.slang
)

target_copy_shaders(MotionVecVis RenderPasses/MotionVecVis)

target_source_group(MotionVecVis "RenderPasses")
