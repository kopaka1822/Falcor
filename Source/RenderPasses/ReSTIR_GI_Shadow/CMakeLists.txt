add_plugin(ReSTIR_GI_Shadow)

target_sources(ReSTIR_GI_Shadow PRIVATE
	ReSTIR_GI_Shadow.cpp
    ReSTIR_GI_Shadow.h
	Shader/FinalShading.cs.slang
	Shader/GeneratePathSamples.rt.slang
	Shader/PathData.slang
	Shader/ResamplingPass.cs.slang
	Shader/Reservoir.slang
	Shader/SurfaceDataGI.slang
	Shader/TraceTransmissionDelta.rt.slang
	Shader/EvalAnalyticDirect.rt.slang		
)

target_copy_shaders(ReSTIR_GI_Shadow RenderPasses/ReSTIR_GI_Shadow)

target_source_group(ReSTIR_GI_Shadow "RenderPasses")
