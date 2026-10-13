find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(network_tests NetworkTests.cpp "${ZTGUI_ROOT_DIR}/src/client/main/NetworkMembers.cpp")
target_include_directories(network_tests PRIVATE "${ZTGUI_ROOT_DIR}/src/client/main")
target_link_libraries(network_tests PRIVATE ZTGUI::Api Qt6::Test)
if(MSVC)
    target_compile_options(network_tests PRIVATE /utf-8)
endif()
add_test(NAME network_transport COMMAND network_tests)
if(WIN32)
    add_test(NAME windows_port_preflight COMMAND powershell -NoProfile -NonInteractive -File "${CMAKE_CURRENT_SOURCE_DIR}/WindowsPortTests.ps1")
endif()
add_executable(instance_probe InstanceProbe.cpp "${ZTGUI_ROOT_DIR}/src/client/main/SingleInstance.cpp")
target_include_directories(instance_probe PRIVATE "${ZTGUI_ROOT_DIR}/src/client/main")
target_link_libraries(instance_probe PRIVATE Qt6::Gui Qt6::Network)
set_target_properties(instance_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTGUI_BUILD_DIR}")
if(MSVC)
    target_compile_options(instance_probe PRIVATE /utf-8)
    target_compile_definitions(instance_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_executable(fake_zerotier_cli FakeCli.cpp)
set_target_properties(fake_zerotier_cli PROPERTIES OUTPUT_NAME "zerotier-cli"
    RUNTIME_OUTPUT_DIRECTORY "${ZTGUI_BUILD_DIR}")
target_link_libraries(fake_zerotier_cli PRIVATE Qt6::Core)

add_executable(app_log_probe AppLogProbe.cpp "${ZTGUI_ROOT_DIR}/src/client/main/AppLog.cpp")
target_include_directories(app_log_probe PRIVATE "${ZTGUI_ROOT_DIR}/src/client/main")
target_compile_definitions(app_log_probe PRIVATE ZEROTIER_GUI_VERSION="${ZEROTIER_VERSION}")
target_link_libraries(app_log_probe PRIVATE Qt6::Core)
set_target_properties(app_log_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTGUI_BUILD_DIR}")

qt_add_executable(zerotier_gui_tests QmlTests.cpp ${ZTGUI_MAIN_SOURCES} ${ZTGUI_MAIN_HEADERS})
ztgui_configure_host(zerotier_gui_tests)
target_link_libraries(zerotier_gui_tests PRIVATE Qt6::Test Qt6::Widgets)
ztgui_add_qml_ui(zerotier_gui_tests)
qt_add_resources(zerotier_gui_tests test_app_icon PREFIX "/" BASE "${ZTGUI_ROOT_DIR}/src/client"
    FILES "${ZTGUI_ROOT_DIR}/src/client/assets/app-icon.png")
target_compile_definitions(zerotier_gui_tests PRIVATE ZEROTIER_TESTING)
add_dependencies(zerotier_gui_tests fake_zerotier_cli app_log_probe)
add_dependencies(zerotier_gui_tests instance_probe)
if(MSVC)
    target_compile_options(fake_zerotier_cli PRIVATE /utf-8)
    target_compile_options(app_log_probe PRIVATE /utf-8)
endif()
add_test(NAME gui_regressions COMMAND zerotier_gui_tests -platform offscreen -o test-results.txt,txt)
set_tests_properties(gui_regressions PROPERTIES WORKING_DIRECTORY "${ZTGUI_BUILD_DIR}")
set_tests_properties(gui_regressions PROPERTIES TIMEOUT 120)
set_tests_properties(gui_regressions PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")
add_test(NAME gpu_acceleration_override COMMAND zerotier_gui_tests
    --disable-gpu-acceleration gpuAccelerationCommandLineOverrideIsTemporary
    -platform offscreen -o gpu-override-results.txt,txt)
set_tests_properties(gpu_acceleration_override PROPERTIES
    WORKING_DIRECTORY "${ZTGUI_BUILD_DIR}" TIMEOUT 30
    ENVIRONMENT "QT_QUICK_BACKEND=software")

qt_add_executable(core_tests CoreTests.cpp ${ZTGUI_MAIN_SOURCES} ${ZTGUI_MAIN_HEADERS})
ztgui_configure_host(core_tests)
target_link_libraries(core_tests PRIVATE Qt6::Test Qt6::Widgets)
target_compile_definitions(core_tests PRIVATE ZEROTIER_TESTING)
add_dependencies(core_tests fake_zerotier_cli app_log_probe instance_probe)
add_test(NAME core_regressions COMMAND core_tests -platform offscreen)
set_tests_properties(core_regressions PROPERTIES WORKING_DIRECTORY "${ZTGUI_BUILD_DIR}" TIMEOUT 120)

foreach(kind good bad obsolete legacy lifecycle lifecycle_failed cancellable cancellable_second cancellable_queued cancellable_blocked)
    set(FIXTURE_API_VERSION "${ZTGUI_API_VERSION}")
    # The API subdirectory's variables have directory scope; derive generation from VERSION.
    string(REGEX REPLACE "\\.[0-9]+$" "" FIXTURE_API_VERSION "${ZEROTIER_VERSION}")
    if(kind STREQUAL "bad")
        set(FIXTURE_API_VERSION "0.999")
    endif()
    configure_file(plugins/fixture.json.in "${CMAKE_CURRENT_BINARY_DIR}/${kind}/fixture.json" @ONLY)
    qt_add_plugin(plugin_fixture_${kind} SHARED CLASS_NAME Fixture plugins/Fixture.cpp)
    target_include_directories(plugin_fixture_${kind} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${kind}")
    target_link_libraries(plugin_fixture_${kind} PRIVATE ZTGUI::Api)
    if(kind STREQUAL "obsolete")
        target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_OBSOLETE_IID=1)
    elseif(kind STREQUAL "legacy")
        target_include_directories(plugin_fixture_${kind} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugins/legacy-api")
        target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_LIFECYCLE=1 FIXTURE_LEGACY_API=1)
    elseif(kind STREQUAL "lifecycle")
        target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_LIFECYCLE=1)
    elseif(kind STREQUAL "lifecycle_failed")
        target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_LIFECYCLE=2)
    elseif(kind MATCHES "^cancellable")
        target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_ID="test.${kind}")
        if(kind STREQUAL "cancellable_blocked")
            target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_CANCELLABLE=2)
        else()
            target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_CANCELLABLE=1)
        endif()
    endif()
endforeach()
qt_add_executable(plugin_loader_tests plugins/LoaderTests.cpp
    "${ZTGUI_ROOT_DIR}/src/client/main/plugins/PluginLoader.cpp" "${ZTGUI_ROOT_DIR}/src/client/main/plugins/PluginRegistry.cpp")
target_link_libraries(plugin_loader_tests PRIVATE ZTGUI::Api Qt6::Test Qt6::Concurrent)

# Plugin tools are implemented by the host, never by the public SDK.
function(ztgui_add_plugin_tools_test target)
    target_sources(${target} PRIVATE
        "${ZTGUI_ROOT_DIR}/src/client/main/plugins/HostServicesImpl.cpp"
        "${ZTGUI_ROOT_DIR}/src/client/main/ZeroTierClient.cpp"
        "${ZTGUI_ROOT_DIR}/src/client/main/NetworkMembers.cpp"
        "${ZTGUI_ROOT_DIR}/src/client/main/AppLog.cpp"
        "${ZTGUI_ROOT_DIR}/src/client/main/MacPlatform.cpp")
    ztgui_configure_host(${target})
    target_compile_definitions(${target} PRIVATE ZEROTIER_TESTING)
endfunction()
ztgui_add_plugin_tools_test(plugin_loader_tests)
target_compile_definitions(plugin_loader_tests PRIVATE
    FIXTURE_LEGACY="$<TARGET_FILE:plugin_fixture_legacy>"
    FIXTURE_GOOD="$<TARGET_FILE:plugin_fixture_good>" FIXTURE_BAD="$<TARGET_FILE:plugin_fixture_bad>"
    FIXTURE_OBSOLETE="$<TARGET_FILE:plugin_fixture_obsolete>"
    FIXTURE_LIFECYCLE="$<TARGET_FILE:plugin_fixture_lifecycle>"
    FIXTURE_LIFECYCLE_FAILED="$<TARGET_FILE:plugin_fixture_lifecycle_failed>"
    FIXTURE_CANCELLABLE="$<TARGET_FILE:plugin_fixture_cancellable>"
    FIXTURE_CANCELLABLE_SECOND="$<TARGET_FILE:plugin_fixture_cancellable_second>"
    FIXTURE_CANCELLABLE_QUEUED="$<TARGET_FILE:plugin_fixture_cancellable_queued>"
    FIXTURE_CANCELLABLE_BLOCKED="$<TARGET_FILE:plugin_fixture_cancellable_blocked>")
if(MSVC)
    target_compile_options(plugin_loader_tests PRIVATE /utf-8)
endif()
add_dependencies(plugin_loader_tests plugin_fixture_good plugin_fixture_bad plugin_fixture_obsolete
    plugin_fixture_legacy
    plugin_fixture_lifecycle plugin_fixture_lifecycle_failed plugin_fixture_cancellable
    plugin_fixture_cancellable_second plugin_fixture_cancellable_queued plugin_fixture_cancellable_blocked)
add_test(NAME dynamic_plugins COMMAND plugin_loader_tests -platform offscreen)

target_compile_definitions(zerotier_gui_tests PRIVATE
    FIXTURE_CANCELLABLE="$<TARGET_FILE:plugin_fixture_cancellable>"
    FIXTURE_CANCELLABLE_BLOCKED="$<TARGET_FILE:plugin_fixture_cancellable_blocked>"
    )
add_dependencies(zerotier_gui_tests plugin_fixture_cancellable plugin_fixture_cancellable_blocked)

set(ZTGUI_VOICE_PLUGIN_PATH "" CACHE FILEPATH "Independently built voice plugin for smoke tests")
if(ZTGUI_VOICE_PLUGIN_PATH)
    find_package(Qt6 REQUIRED COMPONENTS Network)
    qt_add_executable(voice_plugin_tests plugins/VoicePluginTests.cpp
        "${ZTGUI_ROOT_DIR}/src/client/main/plugins/PluginLoader.cpp" "${ZTGUI_ROOT_DIR}/src/client/main/plugins/PluginRegistry.cpp")
    target_link_libraries(voice_plugin_tests PRIVATE ZTGUI::Api Qt6::Test Qt6::Network Qt6::Concurrent)
    ztgui_add_plugin_tools_test(voice_plugin_tests)
    target_compile_definitions(voice_plugin_tests PRIVATE VOICE_PLUGIN="${ZTGUI_VOICE_PLUGIN_PATH}")
    if(MSVC)
        target_compile_options(voice_plugin_tests PRIVATE /utf-8)
    endif()
    add_test(NAME voice_plugin_panel COMMAND voice_plugin_tests -platform offscreen)
    set_tests_properties(voice_plugin_panel PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")
endif()
