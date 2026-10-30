# Header-only SDK target. Plugins: target_link_libraries(my_plugin PRIVATE dm::sdk)
add_library(dm_sdk INTERFACE)
add_library(dm::sdk ALIAS dm_sdk)
target_include_directories(dm_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(dm_sdk INTERFACE cxx_std_17)

# dm_add_plugin(<id> SOURCES ...)
#   Builds devices/<id> as a shared library named <id> and stages it, together
#   with plugin.json / page.json / README.md, into ${DM_DEVICES_BUILD_DIR}/<id>/.
function(dm_add_plugin id)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_library(${id} SHARED ${ARG_SOURCES})
    target_link_libraries(${id} PRIVATE dm::sdk)
    set_target_properties(${id} PROPERTIES
        PREFIX ""                                   # sim_smu.dll, not libsim_smu.dll
        CXX_VISIBILITY_PRESET hidden
        RUNTIME_OUTPUT_DIRECTORY ${DM_DEVICES_BUILD_DIR}/${id}
        LIBRARY_OUTPUT_DIRECTORY ${DM_DEVICES_BUILD_DIR}/${id})
    foreach(f plugin.json page.json README.md)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
            configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${f} ${DM_DEVICES_BUILD_DIR}/${id}/${f} COPYONLY)
        endif()
    endforeach()
    install(TARGETS ${id} RUNTIME DESTINATION devices/${id} LIBRARY DESTINATION devices/${id})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json DESTINATION devices/${id})
    foreach(f page.json README.md)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
            install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/${f} DESTINATION devices/${id})
        endif()
    endforeach()
endfunction()
