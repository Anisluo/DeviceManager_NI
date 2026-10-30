# Every subdirectory with a CMakeLists.txt is a C++ plugin.
# Python plugins (type: python) need no build step; they are installed as-is.
file(GLOB _plugin_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(_dir ${_plugin_dirs})
    if(IS_DIRECTORY ${_dir} AND EXISTS ${_dir}/CMakeLists.txt)
        add_subdirectory(${_dir})
    elseif(IS_DIRECTORY ${_dir} AND EXISTS ${_dir}/plugin.json)
        get_filename_component(_id ${_dir} NAME)
        install(DIRECTORY ${_dir}/ DESTINATION devices/${_id} PATTERN __pycache__ EXCLUDE PATTERN _work EXCLUDE)
    endif()
endforeach()
