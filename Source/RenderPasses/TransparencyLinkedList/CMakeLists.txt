add_plugin(TransparencyLinkedList)

target_sources(TransparencyLinkedList PRIVATE
    TransparencyLinkedList.cpp
    TransparencyLinkedList.h
	TransparencyLinkedList.rt.slang
	GenStochasticSM.rt.slang
    GenLinkedList.rt.slang
    GenLinkedListNeighbors.cs.slang
    SharedTypes.slang
)

target_copy_shaders(TransparencyLinkedList RenderPasses/TransparencyLinkedList)
target_source_group(TransparencyLinkedList "RenderPasses")
