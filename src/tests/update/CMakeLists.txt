# Isolated tests for the automatic update subsystem.
#
# Every test uses temporary directories and a loopback HTTP server. No real
# deployment, installation or user setting is touched.

set(ZTO_UPDATE_TEST_PUBLIC_KEY "")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/keys/update-signing.pub" ZTO_UPDATE_TEST_PUBLIC_KEY)
string(STRIP "${ZTO_UPDATE_TEST_PUBLIC_KEY}" ZTO_UPDATE_TEST_PUBLIC_KEY)

set(ZTO_UPDATE_DIR "${ZTO_ROOT_DIR}/src/client/main/update")
set(ZTO_UPDATE_CORE_SOURCES
    "${ZTO_UPDATE_DIR}/UpdateVersion.cpp"
    "${ZTO_UPDATE_DIR}/UpdateManifest.cpp"
    "${ZTO_UPDATE_DIR}/UpdateCrypto.cpp"
    "${ZTO_UPDATE_DIR}/UpdateConfig.cpp"
    "${ZTO_UPDATE_DIR}/UpdateDeflate.cpp"
    "${ZTO_UPDATE_DIR}/UpdateArchive.cpp"
    "${ZTO_UPDATE_DIR}/UpdaterPlan.cpp")
set(ZTO_UPDATE_TRANSPORT_SOURCES
    "${ZTO_UPDATE_DIR}/UpdateHttp.cpp"
    "${ZTO_UPDATE_DIR}/UpdateStorage.cpp"
    "${ZTO_UPDATE_DIR}/UpdateController.cpp")
set(ZTO_UPDATE_ENGINE_SOURCES
    "${ZTO_UPDATE_DIR}/UpdaterPlan.cpp"
    "${ZTO_UPDATE_DIR}/UpdaterEngine.cpp")

function(zto_update_test_common target)
    target_include_directories(${target} PRIVATE
        "${ZTO_ROOT_DIR}/src/client/main"
        "${ZTO_UPDATE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ZTO_UPDATE_GENERATED_DIR}")
    target_compile_definitions(${target} PRIVATE
        ZTO_VERSION="${ZTO_VERSION}"
        ZTO_TESTING
        ZTO_UPDATE_PUBLIC_KEY_BASE64="${ZTO_UPDATE_TEST_PUBLIC_KEY}"
        UPDATE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_link_libraries(${target} PRIVATE Qt6::Network)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:zto_sodium>" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()

add_executable(update_version_tests VersionTests.cpp "${ZTO_UPDATE_DIR}/UpdateVersion.cpp")
target_link_libraries(update_version_tests PRIVATE Qt6::Test Qt6::Core)
zto_update_test_common(update_version_tests)
add_test(NAME update_version COMMAND update_version_tests)

add_executable(update_protocol_tests ProtocolTests.cpp TestSupport.cpp ${ZTO_UPDATE_CORE_SOURCES})
target_link_libraries(update_protocol_tests PRIVATE Qt6::Test Qt6::Core)
zto_update_test_common(update_protocol_tests)
add_test(NAME update_protocol COMMAND update_protocol_tests)

add_executable(update_archive_tests ArchiveTests.cpp TestSupport.cpp
    "${ZTO_UPDATE_DIR}/UpdateDeflate.cpp"
    "${ZTO_UPDATE_DIR}/UpdateArchive.cpp"
    "${ZTO_UPDATE_DIR}/UpdaterPlan.cpp")
target_link_libraries(update_archive_tests PRIVATE Qt6::Test Qt6::Core)
zto_update_test_common(update_archive_tests)
add_test(NAME update_archive COMMAND update_archive_tests)

add_executable(update_http_tests HttpTests.cpp TestSupport.cpp ${ZTO_UPDATE_CORE_SOURCES}
    "${ZTO_UPDATE_DIR}/UpdateHttp.cpp")
target_link_libraries(update_http_tests PRIVATE Qt6::Test Qt6::Core Qt6::Network)
zto_update_test_common(update_http_tests)
add_test(NAME update_http COMMAND update_http_tests)
set_tests_properties(update_http PROPERTIES TIMEOUT 120)

add_executable(update_controller_tests ControllerTests.cpp TestSupport.cpp
    ${ZTO_UPDATE_CORE_SOURCES} ${ZTO_UPDATE_TRANSPORT_SOURCES}
    "${ZTO_UPDATE_DIR}/UpdaterEngine.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/AppLog.cpp")
target_link_libraries(update_controller_tests PRIVATE Qt6::Test Qt6::Core Qt6::Network Qt6::Concurrent)
zto_update_test_common(update_controller_tests)
# A stale downstream definition must never re-enable a built-in server URL.
target_compile_definitions(update_controller_tests PRIVATE
    ZTO_UPDATE_DEFAULT_MANIFEST_URL="http://127.0.0.1:9/manifest.json")
add_test(NAME update_controller COMMAND update_controller_tests)
set_tests_properties(update_controller PROPERTIES TIMEOUT 120)

if(WIN32)
    add_executable(update_fake_helper FakeHelper.cpp)
    set_target_properties(update_fake_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
    if(MSVC)
        target_compile_options(update_fake_helper PRIVATE /utf-8)
    endif()

    add_executable(update_restart_probe RestartProbe.cpp)
    set_target_properties(update_restart_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
    if(MSVC)
        target_compile_options(update_restart_probe PRIVATE /utf-8)
    endif()

    add_executable(update_engine_tests EngineTests.cpp TestSupport.cpp ${ZTO_UPDATE_ENGINE_SOURCES})
    target_link_libraries(update_engine_tests PRIVATE Qt6::Test Qt6::Core)
    zto_update_test_common(update_engine_tests)
    target_compile_definitions(update_engine_tests PRIVATE
        Z2_UPDATER_EXE="$<TARGET_FILE:z2_updater>"
        RESTART_PROBE_EXE="$<TARGET_FILE:update_restart_probe>")
    add_dependencies(update_engine_tests z2_updater update_restart_probe)
    add_test(NAME update_engine COMMAND update_engine_tests)
    set_tests_properties(update_engine PROPERTIES TIMEOUT 180)

    target_compile_definitions(update_controller_tests PRIVATE
        UPDATE_NATIVE_HELPER="$<TARGET_FILE:z2_updater>"
        UPDATE_FAKE_HELPER="$<TARGET_FILE:update_fake_helper>")
    add_dependencies(update_controller_tests update_fake_helper z2_updater)
endif()
