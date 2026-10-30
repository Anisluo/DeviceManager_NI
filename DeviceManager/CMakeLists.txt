find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql)

# Embedded Python (pybind11). Hints: -DPython_ROOT_DIR=<python> -Dpybind11_DIR=<.../share/cmake/pybind11>
find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Embed)
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 2.10 CONFIG REQUIRED)
qt_standard_project_setup(REQUIRES 6.5)
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)   # extra qmldir per QML subdirectory
endif()

# Application icon / version info (Windows resource), regenerated with resources/make_icon.py
set(DM_APP_RC "")
if(WIN32)
    enable_language(RC)
    set(DM_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
    configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    set(DM_APP_RC ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()

qt_add_executable(DeviceManager WIN32 MACOSX_BUNDLE
    ${DM_APP_RC}
    src/main.cpp
    src/app/AppController.cpp   src/app/AppController.h
    src/app/SyntaxHighlighter.cpp src/app/SyntaxHighlighter.h
    src/app/PlotView.cpp        src/app/PlotView.h
    src/app/WindowChrome.cpp    src/app/WindowChrome.h
    src/core/PluginModule.h
    src/python/PythonRuntime.cpp      src/python/PythonRuntime.h
    src/python/PythonPluginModule.cpp src/python/PythonPluginModule.h
    src/python/ScriptEngine.cpp       src/python/ScriptEngine.h
    src/core/DataStore.cpp      src/core/DataStore.h
    src/core/Device.cpp         src/core/Device.h
    src/core/LogModel.cpp       src/core/LogModel.h
    src/core/PluginLibrary.cpp  src/core/PluginLibrary.h
    src/core/PluginRegistry.cpp src/core/PluginRegistry.h
    src/core/Station.cpp        src/core/Station.h
)

set(DM_QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/Sidebar.qml
    qml/ConsolePanel.qml
    qml/StationPage.qml
    qml/PluginInfoPage.qml
    qml/DevicePage.qml
    qml/ScriptConsole.qml
    qml/DataPage.qml
    qml/controls/ShmooMap.qml
    qml/page/XyWidget.qml
    qml/page/ShmooWidget.qml
    qml/page/SweepWidget.qml
    qml/page/TextWidget.qml
    qml/page/ScopeWidget.qml
    qml/page/ParamsWidget.qml
    qml/controls/CodeEditor.qml
    qml/controls/XyPlot.qml
    qml/page/ScriptWidget.qml
    qml/page/PageSection.qml
    qml/page/WidgetHost.qml
    qml/page/NumberWidget.qml
    qml/page/ComboWidget.qml
    qml/page/SwitchWidget.qml
    qml/page/ButtonWidget.qml
    qml/page/ReadoutWidget.qml
    qml/page/TrendWidget.qml
    qml/page/TableWidget.qml
    qml/page/LogWidget.qml
    qml/page/PlaceholderWidget.qml
    qml/controls/Card.qml
    qml/controls/DmButton.qml
    qml/controls/DmTextField.qml
    qml/controls/DmComboBox.qml
    qml/controls/DmSwitch.qml
    qml/controls/StatusDot.qml
    qml/controls/SidebarItem.qml
    qml/controls/SectionLabel.qml
    qml/js/Format.js
)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(DeviceManager
    URI DeviceManager
    VERSION 1.0
    QML_FILES ${DM_QML_FILES}
)

qt_add_resources(DeviceManager "app_icons"
    PREFIX "/icons"
    BASE resources
    FILES resources/app_16.png resources/app_24.png resources/app_32.png resources/app_48.png
          resources/app_64.png resources/app_128.png resources/app_256.png)

target_include_directories(DeviceManager PRIVATE src)
target_link_libraries(DeviceManager PRIVATE dm::sdk pybind11::embed
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Sql)

if(WIN32)
    target_link_libraries(DeviceManager PRIVATE dwmapi)      # WindowChrome: DwmExtendFrameIntoClientArea
endif()

# Python's home (stdlib + site-packages) and its runtime DLLs next to the exe.
cmake_path(GET Python_EXECUTABLE PARENT_PATH DM_PYTHON_HOME)
if(WIN32)
    file(GLOB _py_dlls "${DM_PYTHON_HOME}/python3*.dll" "${DM_PYTHON_HOME}/vcruntime140*.dll")
    foreach(_dll ${_py_dlls})
        add_custom_command(TARGET DeviceManager POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dll} $<TARGET_FILE_DIR:DeviceManager>)
    endforeach()
    install(FILES ${_py_dlls} DESTINATION bin)
endif()
target_compile_definitions(DeviceManager PRIVATE
    DM_VERSION="${PROJECT_VERSION}"
    DM_DEVICES_SOURCE_DIR="${DM_DEVICES_SOURCE_DIR}"
    DM_DEVICES_BUILD_DIR="${DM_DEVICES_BUILD_DIR}"
    DM_STATIONS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stations"
    DM_SEQUENCES_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sequences"
    DM_SDK_PYTHON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/sdk/python"
    DM_PYTHON_HOME="${DM_PYTHON_HOME}"
    DM_PYTHON_VERSION="${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}")

install(TARGETS DeviceManager RUNTIME DESTINATION bin BUNDLE DESTINATION .)
install(DIRECTORY stations/ DESTINATION stations)
install(DIRECTORY sequences/ DESTINATION sequences)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sdk/python/ DESTINATION sdk/python PATTERN __pycache__ EXCLUDE)

# PlotView: 1M-point paint time and the zoom gestures through real mouse / wheel events
if(DM_BUILD_TESTS)
    qt_add_executable(plot_view_test tests/plot_view_test.cpp src/app/PlotView.cpp src/app/PlotView.h)
    target_link_libraries(plot_view_test PRIVATE Qt6::Core Qt6::Gui Qt6::Quick)
    set_target_properties(plot_view_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME plot_view COMMAND plot_view_test ${CMAKE_BINARY_DIR}/plot_view)
    set_tests_properties(plot_view PROPERTIES TIMEOUT 120)
endif()
