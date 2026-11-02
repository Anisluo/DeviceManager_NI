install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION devices/signal_analysis
    PATTERN __pycache__ EXCLUDE PATTERN _work EXCLUDE)
if(DM_BUILD_TESTS)
    find_package(Python COMPONENTS Interpreter QUIET)
    if(Python_Interpreter_FOUND)
        add_test(NAME signal_analysis COMMAND ${Python_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        set_tests_properties(signal_analysis PROPERTIES TIMEOUT 60)
        add_test(NAME signal_analysis_performance COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/accept_performance.py
            --report ${CMAKE_BINARY_DIR}/performance/signal_analysis_fft.json)
        set_tests_properties(signal_analysis_performance PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "performance;acceptance")
    endif()
endif()
