add_plugin(MotionVecVis)

target_sources(MotionVecVis PRIVATE
    MotionVecVis.cpp
    MotionVecVis.h
    MotionVecVis.ps.slang
    PosDiff.ps.slang
    AngularDiff.ps.slang
)

target_source_group(MotionVecVis "RenderPasses")
