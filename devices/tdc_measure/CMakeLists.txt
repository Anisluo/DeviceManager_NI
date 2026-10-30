dm_add_plugin(tdc_measure SOURCES
    src/tdc_task.cpp
    src/project.cpp
    src/sheets.cpp
    src/tdcgen.cpp
    src/tdms.cpp
    src/util.cpp
    src/workspace.cpp)
if(NOT WIN32)
    target_link_libraries(tdc_measure PRIVATE ${CMAKE_DL_LIBS})
endif()

# the Digital Pattern Editor project ships with the task
install(DIRECTORY project/ DESTINATION devices/tdc_measure/project)

add_executable(tdc_task_test tests/tdc_task_test.cpp)
target_link_libraries(tdc_task_test PRIVATE dm::sdk ${CMAKE_DL_LIBS})
add_dependencies(tdc_task_test tdc_measure)
add_test(NAME tdc_measure_task COMMAND tdc_task_test $<TARGET_FILE:tdc_measure> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(tdc_measure_task PROPERTIES TIMEOUT 120)
