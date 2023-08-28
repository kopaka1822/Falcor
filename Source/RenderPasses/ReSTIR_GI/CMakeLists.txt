add_plugin(ReSTIR_GI)

target_sources(ReSTIR_GI PRIVATE
	ReSTIR_GI.cpp
    ReSTIR_GI.h
	Shader/FinalShading.cs.slang
	Shader/GeneratePathSamples.rt.slang
	Shader/PathData.slang
	Shader/ResamplingPass.cs.slang
	Shader/Reservoir.slang
	Shader/SurfaceDataGI.slang
	Shader/TraceTransmissionDelta.rt.slang	
)

target_copy_shaders(ReSTIR_GI RenderPasses/ReSTIR_GI)

target_source_group(ReSTIR_GI "RenderPasses")
