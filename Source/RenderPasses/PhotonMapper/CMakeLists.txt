add_plugin(PhotonMapper)

target_sources(PhotonMapper PRIVATE
	Hash.slang
	PhotonData.slang
    PhotonMapper.cpp
    PhotonMapper.h
	PhotonMapperCollect.rt.slang
	PhotonMapperGenerate.rt.slang
	TraceTransmissionDelta.rt.slang
)

target_copy_shaders(PhotonMapper RenderPasses/PhotonMapper)
target_source_group(PhotonMapper "RenderPasses")
