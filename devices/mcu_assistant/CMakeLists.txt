install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION devices/mcu_assistant
    PATTERN __pycache__ EXCLUDE PATTERN _work EXCLUDE)
if(DM_BUILD_TESTS)
    find_package(Python COMPONENTS Interpreter QUIET)
    if(Python_Interpreter_FOUND)
        add_test(NAME mcu_assistant COMMAND ${Python_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        set_tests_properties(mcu_assistant PROPERTIES TIMEOUT 60)
    endif()
endif()
