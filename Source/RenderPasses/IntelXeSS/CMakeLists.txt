add_plugin(IntelXeSS)

target_sources(IntelXeSS PRIVATE
    IntelXeSS.cpp
    IntelXeSS.h
)
target_link_libraries(IntelXeSS PRIVATE xessapi)
target_source_group(IntelXeSS "RenderPasses")
