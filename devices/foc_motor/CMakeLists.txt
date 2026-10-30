dm_add_plugin(foc_motor SOURCES
    src/foc_plugin.cpp
    src/foc_sim.cpp
    src/serial_port.cpp
    firmware/dmfoc_proto.c)          # the simulated board runs the real firmware protocol code

add_executable(foc_test tests/foc_test.cpp)
target_link_libraries(foc_test PRIVATE dm::sdk ${CMAKE_DL_LIBS})
add_dependencies(foc_test foc_motor)
add_test(NAME foc_motor_closed_loop COMMAND foc_test $<TARGET_FILE:foc_motor>)
set_tests_properties(foc_motor_closed_loop PROPERTIES TIMEOUT 120)

# firmware sources ship with the plugin: they go into the motor controller's project
install(DIRECTORY firmware/ DESTINATION devices/foc_motor/firmware)
