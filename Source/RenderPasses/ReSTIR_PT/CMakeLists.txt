add_plugin(ReSTIR_PT)

target_sources(ReSTIR_PT PRIVATE
    ColorType.slang
    GeneratePaths.cs.slang
    GuideData.slang
    LoadShadingData.slang
    NRDHelpers.slang
    Params.slang
	PathBuilder.slang
	PathReservoir.slang
    PathState.slang
    PathTracer.slang
    ReSTIR_PT.cpp
    ReSTIR_PT.h
	SpatialPathRetrace.cs.slang
	SpatialReuse.cs.slang
    PathTracerNRD.slang
    ReflectTypes.cs.slang
    ResolvePass.cs.slang
    StaticParams.slang
	TemporalPathRetrace.cs.slang
	TemporalReuse.cs.slang
    TracePass.rt.slang
)

target_copy_shaders(ReSTIR_PT RenderPasses/ReSTIR_PT)

target_source_group(ReSTIR_PT "RenderPasses")
