# ABI smoke test: loads a plugin with the raw OS loader (no Qt) and exercises
# the C interface. Runs in CI without hardware or Qt.
add_executable(abi_smoke abi_smoke.cpp)
target_link_libraries(abi_smoke PRIVATE dm::sdk ${CMAKE_DL_LIBS})

if(TARGET sim_smu)
    add_dependencies(abi_smoke sim_smu)
    add_test(NAME abi_smoke_sim_smu
             COMMAND abi_smoke $<TARGET_FILE:sim_smu>)
endif()

# Python SDK unit tests (plugin base class, helpers) — no host needed.
find_package(Python 3.9 COMPONENTS Interpreter)
if(Python_Interpreter_FOUND)
    add_test(NAME python_sdk
             COMMAND ${Python_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/sdk/python/tests -v)
    set_tests_properties(python_sdk PROPERTIES ENVIRONMENT "MPLBACKEND=Agg")
    # Python plugins with a tests/ folder
    file(GLOB _plugin_tests LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/devices/*/tests)
    foreach(_t ${_plugin_tests})
        get_filename_component(_plugin ${_t} DIRECTORY)
        get_filename_component(_id ${_plugin} NAME)
        file(GLOB _py ${_t}/test_*.py)
        if(NOT _py)
            continue()          # C++ plugins register their own tests
        endif()
        add_test(NAME plugin_${_id} COMMAND ${Python_EXECUTABLE} -m unittest discover -s ${_t} -v)
    endforeach()
endif()

# RTL simulation of the FPGA platform (Yosys lint + CXXRTL testbenches) when yowasp-yosys is available.
set(DM_YOWASP_PATH "D:/Qt/_deps/yowasp" CACHE PATH "folder holding the yowasp_yosys package (pip --target)")
if(Python_Interpreter_FOUND AND EXISTS "${DM_YOWASP_PATH}/yowasp_yosys")
    add_test(NAME fpga_rtl_sim
             COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/devices/fpga_k7/rtl/sim/run_sim.py
                     --cxx ${CMAKE_CXX_COMPILER} --out ${CMAKE_BINARY_DIR}/fpga_sim)
    set_tests_properties(fpga_rtl_sim PROPERTIES
        ENVIRONMENT "PYTHONPATH=${DM_YOWASP_PATH};YOWASP_CACHE_DIR=${DM_YOWASP_PATH}_cache"
        TIMEOUT 600)
endif()
