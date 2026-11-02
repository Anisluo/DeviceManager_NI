# Pure Python task: install the plugin and run its protocol-level checks with CTest.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION devices/protocol_sim
    PATTERN __pycache__ EXCLUDE PATTERN _work EXCLUDE)
if(DM_BUILD_TESTS)
    find_package(Python COMPONENTS Interpreter QUIET)
    if(Python_Interpreter_FOUND)
        add_test(NAME protocol_sim COMMAND ${Python_EXECUTABLE} -m unittest discover
            -s ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        set_tests_properties(protocol_sim PROPERTIES TIMEOUT 30)
    endif()
endif()
