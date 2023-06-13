add_plugin(ReSTIR_FG)

target_sources(ReSTIR_FG PRIVATE
	CollectPhotons.rt.slang
    CustomAccelerationStructure.cpp
    CustomAccelerationStructure.h
	FinalGatherData.slang
	FinalShading.cs.slang
	GenerateFinalGatherSamples.rt.slang
    GeneratePhotons.rt.slang
	Hash.slang
	ResamplingPass.cs.slang
	Reservoir.slang
    ReSTIR_FG.cpp
    ReSTIR_FG.h
	SurfaceDataFG.slang
	
)

target_copy_shaders(ReSTIR_FG RenderPasses/ReSTIR_FG)

target_source_group(ReSTIR_FG "RenderPasses")
