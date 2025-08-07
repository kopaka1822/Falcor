add_plugin(RasterOITLinkedList)

target_sources(RasterOITLinkedList PRIVATE
    RasterOITLinkedList.cpp
    RasterOITLinkedList.h
    BuildList.3D.slang
    SortList.slang
    CallableSort.slang
    CallableInternal.h
)

target_copy_shaders(RasterOITLinkedList RenderPasses/RasterOITLinkedList)

target_source_group(RasterOITLinkedList "RenderPasses")
