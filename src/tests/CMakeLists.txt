find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(plugin_package_tests PluginPackageTests.cpp)
add_library(zt_runtime_probe SHARED plugins/RuntimeFixture.cpp)
set_target_properties(zt_runtime_probe PROPERTIES PREFIX "")
target_link_libraries(plugin_package_tests PRIVATE zto_package Qt6::Test ZTO::Api)
zto_embed_host_contract(plugin_package_tests "${ZTO_CLIENT_CONTRACT_DIRECTORY}")
target_compile_definitions(plugin_package_tests PRIVATE
    FIXTURE_LIBRARY="$<TARGET_FILE:plugin_fixture_good>"
    RUNTIME_LIBRARY="$<TARGET_FILE:zt_runtime_probe>"
    FIXTURE_PACKAGE="$<TARGET_FILE_DIR:plugin_fixture_good>/plugin_fixture_good.z2plugin")
add_dependencies(plugin_package_tests plugin_fixture_good zt_runtime_probe)
if(MSVC)
    target_compile_options(plugin_package_tests PRIVATE /utf-8)
endif()
add_test(NAME plugin_package_container COMMAND plugin_package_tests)
add_executable(network_tests NetworkTests.cpp "${ZTO_ROOT_DIR}/src/client/main/NetworkMembers.cpp")
target_include_directories(network_tests PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
target_link_libraries(network_tests PRIVATE ZTO::Api Qt6::Test)
if(MSVC)
    target_compile_options(network_tests PRIVATE /utf-8)
endif()
add_test(NAME network_transport COMMAND network_tests)
add_executable(storage_tests StorageTests.cpp
    "${ZTO_ROOT_DIR}/src/client/main/storage/StorageContext.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/CoordinationLock.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/SettingsStore.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/PluginDataStore.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/StorageMaintenance.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/StorageMaintenanceScheduler.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/AppLog.cpp")
target_include_directories(storage_tests PRIVATE "${ZTO_ROOT_DIR}/src/client/main"
    "${ZTO_ROOT_DIR}/src/client/api/include")
target_compile_definitions(storage_tests PRIVATE ZTO_TESTING
    ZTO_VERSION="${ZTO_VERSION}")
target_link_libraries(storage_tests PRIVATE Qt6::Test Qt6::Core Qt6::Concurrent)
if(MSVC)
    target_compile_options(storage_tests PRIVATE /utf-8)
endif()
add_test(NAME storage_core COMMAND storage_tests)
add_executable(local_config_tests LocalConfigTests.cpp
    "${ZTO_ROOT_DIR}/src/client/main/LocalConfig.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/NetworkBundle.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/ZeroTierClient.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/AppLog.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/MacPlatform.cpp")
target_include_directories(local_config_tests PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
target_compile_definitions(local_config_tests PRIVATE ZTO_TESTING
    ZTO_VERSION="${ZTO_VERSION}")
target_link_libraries(local_config_tests PRIVATE Qt6::Test Qt6::Core Qt6::Network Qt6::Concurrent)
if(MSVC)
    target_compile_options(local_config_tests PRIVATE /utf-8)
endif()
add_test(NAME local_config COMMAND local_config_tests)
set_tests_properties(local_config PROPERTIES TIMEOUT 60)
add_executable(fake_ping FakePing.cpp)
target_link_libraries(fake_ping PRIVATE Qt6::Core)
set_target_properties(fake_ping PROPERTIES OUTPUT_NAME "ping" RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}/ping-fixture")
add_executable(member_ping_tests MemberPingTests.cpp "${ZTO_ROOT_DIR}/src/client/main/MemberPing.cpp")
target_include_directories(member_ping_tests PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
target_link_libraries(member_ping_tests PRIVATE Qt6::Network Qt6::Test)
target_compile_definitions(member_ping_tests PRIVATE FAKE_PING="$<TARGET_FILE:fake_ping>")
add_dependencies(member_ping_tests fake_ping)
if(MSVC)
    target_compile_options(member_ping_tests PRIVATE /utf-8)
endif()
add_test(NAME member_icmp_ping COMMAND member_ping_tests)
set_tests_properties(member_icmp_ping PROPERTIES TIMEOUT 30)
if(WIN32)
    add_test(NAME windows_port_preflight COMMAND powershell -NoProfile -NonInteractive -File "${CMAKE_CURRENT_SOURCE_DIR}/WindowsPortTests.ps1")
    # Native read-only install check: a deny ACE replaces the regular-file fault
    # injection and proves the fallback probe works without administrator help.
    add_executable(readonly_probe ReadOnlyProbe.cpp
        "${ZTO_ROOT_DIR}/src/client/main/storage/StorageContext.cpp")
    target_include_directories(readonly_probe PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
    target_compile_definitions(readonly_probe PRIVATE ZTO_TESTING)
    target_link_libraries(readonly_probe PRIVATE Qt6::Core)
    set_target_properties(readonly_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
    add_test(NAME windows_native_readonly COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/ReadOnlyProbe.ps1" -Probe "$<TARGET_FILE:readonly_probe>")
    set_tests_properties(windows_native_readonly PROPERTIES TIMEOUT 60)
endif()
add_executable(instance_probe InstanceProbe.cpp "${ZTO_ROOT_DIR}/src/client/main/SingleInstance.cpp")
target_include_directories(instance_probe PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
target_link_libraries(instance_probe PRIVATE Qt6::Gui Qt6::Network)
set_target_properties(instance_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
if(MSVC)
    target_compile_options(instance_probe PRIVATE /utf-8)
    target_compile_definitions(instance_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_executable(fake_zerotier_cli FakeCli.cpp)
set_target_properties(fake_zerotier_cli PROPERTIES OUTPUT_NAME "zerotier-cli"
    RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
target_link_libraries(fake_zerotier_cli PRIVATE Qt6::Core)

add_executable(app_log_probe AppLogProbe.cpp "${ZTO_ROOT_DIR}/src/client/main/AppLog.cpp")
target_include_directories(app_log_probe PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
target_compile_definitions(app_log_probe PRIVATE ZTO_VERSION="${ZTO_VERSION}")
target_link_libraries(app_log_probe PRIVATE Qt6::Core)
set_target_properties(app_log_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")

qt_add_executable(zto_tests QmlTests.cpp ${ZTO_MAIN_SOURCES} ${ZTO_MAIN_HEADERS})
zto_configure_host(zto_tests)
target_link_libraries(zto_tests PRIVATE Qt6::Test Qt6::Widgets)
zto_add_qml_ui(zto_tests)
target_compile_definitions(zto_tests PRIVATE ZTO_TESTING
    ZTO_FAKE_PING_DIRECTORY="$<TARGET_FILE_DIR:fake_ping>"
    ZTO_SOURCE_VERSION_FILE="${ZTO_ROOT_DIR}/VERSION")
# Keep the launchable application current when building its UI regressions.
add_dependencies(zto_tests zto fake_zerotier_cli app_log_probe)
add_dependencies(zto_tests instance_probe)
add_dependencies(zto_tests fake_ping)
if(MSVC)
    target_compile_options(fake_zerotier_cli PRIVATE /utf-8)
    target_compile_options(app_log_probe PRIVATE /utf-8)
endif()
add_test(NAME gui_regressions COMMAND zto_tests -platform offscreen -o test-results.txt,txt)
set_tests_properties(gui_regressions PROPERTIES WORKING_DIRECTORY "${ZTO_BUILD_DIR}")
set_tests_properties(gui_regressions PROPERTIES TIMEOUT 300)
set_tests_properties(gui_regressions PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")
add_test(NAME gpu_acceleration_override COMMAND zto_tests
    --disable-gpu-acceleration gpuAccelerationCommandLineOverrideIsTemporary
    -platform offscreen -o gpu-override-results.txt,txt)
set_tests_properties(gpu_acceleration_override PROPERTIES
    WORKING_DIRECTORY "${ZTO_BUILD_DIR}" TIMEOUT 30
    ENVIRONMENT "QT_QUICK_BACKEND=software")

add_executable(storage_update_probe StorageUpdateProbe.cpp
    "${ZTO_ROOT_DIR}/src/client/main/plugins/HostServicesImpl.cpp"
    "${ZTO_ROOT_DIR}/src/plugins/voice-chat/client/VoiceConfig.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/plugins/ServiceBroker.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/ClientNetworkService.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/ZeroTierClient.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/LocalConfig.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/NetworkMembers.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/AppLog.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/StorageContext.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/CoordinationLock.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/SettingsStore.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/PluginDataStore.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/MacPlatform.cpp")
zto_configure_host(storage_update_probe)
target_compile_definitions(storage_update_probe PRIVATE ZTO_TESTING)
set_target_properties(storage_update_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
if(MSVC)
    target_compile_options(storage_update_probe PRIVATE /utf-8)
endif()

qt_add_executable(core_tests CoreTests.cpp ${ZTO_MAIN_SOURCES} ${ZTO_MAIN_HEADERS})
zto_configure_host(core_tests)
target_link_libraries(core_tests PRIVATE Qt6::Test Qt6::Widgets)
target_compile_definitions(core_tests PRIVATE ZTO_TESTING
    STORAGE_UPDATE_PROBE="$<TARGET_FILE:storage_update_probe>")
add_dependencies(core_tests fake_zerotier_cli app_log_probe instance_probe storage_update_probe)
add_test(NAME core_regressions COMMAND core_tests -platform offscreen)
set_tests_properties(core_regressions PROPERTIES WORKING_DIRECTORY "${ZTO_BUILD_DIR}" TIMEOUT 120)

foreach(kind good bad obsolete legacy lifecycle lifecycle_failed cancellable cancellable_second cancellable_queued cancellable_blocked twin_a twin_b)
    set(FIXTURE_API_VERSION "${ZTO_API_VERSION}")
    if(kind STREQUAL "bad")
        set(FIXTURE_API_VERSION "0.999")
    endif()
    configure_file(plugins/fixture.json.in "${CMAKE_CURRENT_BINARY_DIR}/${kind}/fixture.json" @ONLY)
    qt_add_plugin(plugin_fixture_${kind} SHARED CLASS_NAME Fixture plugins/Fixture.cpp)
    target_include_directories(plugin_fixture_${kind} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${kind}")
    target_link_libraries(plugin_fixture_${kind} PRIVATE ZTO::Api)
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
        target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_ID="test-${kind}")
        if(kind STREQUAL "cancellable_blocked")
            target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_CANCELLABLE=2)
        else()
            target_compile_definitions(plugin_fixture_${kind} PRIVATE FIXTURE_CANCELLABLE=1)
        endif()
    endif()
    set(pack_id "test-dynamic")
    set(pack_name "Dynamic")
    if(kind MATCHES "^lifecycle" OR kind STREQUAL "legacy")
        set(pack_id "test-lifecycle")
    elseif(kind MATCHES "^cancellable")
        set(pack_id "test-${kind}")
        set(pack_name "test-${kind}")
    elseif(kind STREQUAL "twin_a")
        # Two distinct IDs deliberately declare one display name; the storage
        # layer must not let the first installed package claim legacy data.
        set(pack_id "test-twin-a")
        set(pack_name "Shared Name")
    elseif(kind STREQUAL "twin_b")
        set(pack_id "test-twin-b")
        set(pack_name "Shared Name")
    endif()
    set(pack_iid "vusiif.zto.api.PluginInterface/3.0")
    if(kind STREQUAL "obsolete" OR kind STREQUAL "legacy")
        set(pack_iid "vusiif.zto.api.PluginInterface/2.0")
    endif()
    add_custom_command(TARGET plugin_fixture_${kind} POST_BUILD
        COMMAND "$<TARGET_FILE:ztplugin_pack>" --library "$<TARGET_FILE:plugin_fixture_${kind}>"
            --output "$<TARGET_FILE_DIR:plugin_fixture_${kind}>/plugin_fixture_${kind}.z2plugin"
            --host client --id "${pack_id}" --name "${pack_name}" --version 1 --apiVersion "${FIXTURE_API_VERSION}" --iid "${pack_iid}" VERBATIM)
    add_dependencies(plugin_fixture_${kind} ztplugin_pack)
endforeach()
qt_add_executable(plugin_loader_tests plugins/LoaderTests.cpp
    "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginLoader.cpp" "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginRegistry.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/PluginImport.cpp")
target_link_libraries(plugin_loader_tests PRIVATE ZTO::Api Qt6::Test Qt6::Concurrent)
add_executable(package_lock_probe PackageLockProbe.cpp)
target_link_libraries(package_lock_probe PRIVATE Qt6::Core)
set_target_properties(package_lock_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
add_executable(coordination_lock_probe CoordinationLockProbe.cpp
    "${ZTO_ROOT_DIR}/src/client/main/storage/CoordinationLock.cpp")
target_include_directories(coordination_lock_probe PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
target_link_libraries(coordination_lock_probe PRIVATE Qt6::Core)
set_target_properties(coordination_lock_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
if(MSVC)
    target_compile_options(coordination_lock_probe PRIVATE /utf-8)
endif()

# Plugin tools are implemented by the host, never by the public SDK.
function(zto_add_plugin_tools_test target)
    target_sources(${target} PRIVATE
        "${ZTO_ROOT_DIR}/src/client/main/plugins/ServiceBroker.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/plugins/HostServicesImpl.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/ClientNetworkService.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/ZeroTierClient.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/LocalConfig.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/NetworkMembers.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/AppLog.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/storage/StorageContext.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/storage/CoordinationLock.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/storage/SettingsStore.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/storage/PluginDataStore.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/storage/StorageMaintenance.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/MacPlatform.cpp")
    zto_configure_host(${target})
    target_compile_definitions(${target} PRIVATE ZTO_TESTING)
endfunction()
zto_add_plugin_tools_test(plugin_loader_tests)
target_compile_definitions(plugin_loader_tests PRIVATE
    PACKAGE_LOCK_PROBE="$<TARGET_FILE:package_lock_probe>"
    COORDINATION_LOCK_PROBE="$<TARGET_FILE:coordination_lock_probe>"
    FIXTURE_LEGACY="$<TARGET_FILE_DIR:plugin_fixture_legacy>/plugin_fixture_legacy.z2plugin"
    FIXTURE_GOOD="$<TARGET_FILE_DIR:plugin_fixture_good>/plugin_fixture_good.z2plugin" FIXTURE_BAD="$<TARGET_FILE_DIR:plugin_fixture_bad>/plugin_fixture_bad.z2plugin"
    FIXTURE_OBSOLETE="$<TARGET_FILE_DIR:plugin_fixture_obsolete>/plugin_fixture_obsolete.z2plugin"
    FIXTURE_LIFECYCLE="$<TARGET_FILE_DIR:plugin_fixture_lifecycle>/plugin_fixture_lifecycle.z2plugin"
    FIXTURE_LIFECYCLE_FAILED="$<TARGET_FILE_DIR:plugin_fixture_lifecycle_failed>/plugin_fixture_lifecycle_failed.z2plugin"
    FIXTURE_CANCELLABLE="$<TARGET_FILE_DIR:plugin_fixture_cancellable>/plugin_fixture_cancellable.z2plugin"
    FIXTURE_CANCELLABLE_SECOND="$<TARGET_FILE_DIR:plugin_fixture_cancellable_second>/plugin_fixture_cancellable_second.z2plugin"
    FIXTURE_CANCELLABLE_QUEUED="$<TARGET_FILE_DIR:plugin_fixture_cancellable_queued>/plugin_fixture_cancellable_queued.z2plugin"
    FIXTURE_CANCELLABLE_BLOCKED="$<TARGET_FILE_DIR:plugin_fixture_cancellable_blocked>/plugin_fixture_cancellable_blocked.z2plugin"
    FIXTURE_TWIN_A="$<TARGET_FILE_DIR:plugin_fixture_twin_a>/plugin_fixture_twin_a.z2plugin"
    FIXTURE_TWIN_B="$<TARGET_FILE_DIR:plugin_fixture_twin_b>/plugin_fixture_twin_b.z2plugin")
if(MSVC)
    target_compile_options(plugin_loader_tests PRIVATE /utf-8)
endif()
add_dependencies(plugin_loader_tests package_lock_probe plugin_fixture_good plugin_fixture_bad plugin_fixture_obsolete
    plugin_fixture_legacy
    plugin_fixture_lifecycle plugin_fixture_lifecycle_failed plugin_fixture_cancellable
    plugin_fixture_cancellable_second plugin_fixture_cancellable_queued plugin_fixture_cancellable_blocked
    plugin_fixture_twin_a plugin_fixture_twin_b coordination_lock_probe)
add_test(NAME dynamic_plugins COMMAND plugin_loader_tests -platform offscreen)

target_compile_definitions(zto_tests PRIVATE
    FIXTURE_CANCELLABLE="$<TARGET_FILE_DIR:plugin_fixture_cancellable>/plugin_fixture_cancellable.z2plugin"
    FIXTURE_CANCELLABLE_BLOCKED="$<TARGET_FILE_DIR:plugin_fixture_cancellable_blocked>/plugin_fixture_cancellable_blocked.z2plugin"
    )
add_dependencies(zto_tests plugin_fixture_cancellable plugin_fixture_cancellable_blocked)

set(ZTO_VOICE_PLUGIN_PATH "" CACHE FILEPATH "Independently built voice plugin for smoke tests")
set(ZTO_MEDIA_PLUGIN_PATH "" CACHE FILEPATH "Independently built media worker package")
if(ZTO_MEDIA_PLUGIN_PATH)
    find_package(ZTOrbitMediaApi 2 CONFIG REQUIRED)
    add_executable(media_integration_tests media/MediaIntegrationTests.cpp "${ZTO_ROOT_DIR}/src/client/main/plugins/ServiceBroker.cpp")
    target_link_libraries(media_integration_tests PRIVATE ZTO::Api ZTO::MediaApi zto_package Qt6::Test Qt6::Widgets)
    target_compile_definitions(media_integration_tests PRIVATE MEDIA_PLUGIN="${ZTO_MEDIA_PLUGIN_PATH}")
    set_target_properties(media_integration_tests PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_options(media_integration_tests PRIVATE /utf-8)
        if(ZTO_MEDIA_TEST_ASAN)
            target_compile_options(media_integration_tests PRIVATE /fsanitize=address /Zi)
            target_compile_definitions(media_integration_tests PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
            target_link_options(media_integration_tests PRIVATE /INCREMENTAL:NO /DEBUG)
        endif()
    endif()
    add_test(NAME media_worker_integration COMMAND media_integration_tests -platform windows)
    set_tests_properties(media_worker_integration PROPERTIES TIMEOUT 900)
    get_target_property(media_test_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
    get_filename_component(media_test_qt_bin "${media_test_qt_core}" DIRECTORY)
    get_filename_component(media_test_qt_root "${media_test_qt_bin}" DIRECTORY)
    set_tests_properties(media_worker_integration PROPERTIES ENVIRONMENT
        "QT_PLUGIN_PATH=${media_test_qt_root}/plugins;QT_QPA_PLATFORM_PLUGIN_PATH=${media_test_qt_root}/plugins/platforms;QT_QUICK_BACKEND=software;QTEST_FUNCTION_TIMEOUT=850000")
endif()
if(ZTO_VOICE_PLUGIN_PATH)
    set(ZTO_MEDIA_PLUGIN_PATH "" CACHE FILEPATH "Independently built media worker package")
    find_package(ZTOrbitMediaApi 2 CONFIG REQUIRED)
    find_package(Qt6 REQUIRED COMPONENTS Network)
    qt_add_executable(voice_plugin_tests plugins/VoicePluginTests.cpp
        "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginLoader.cpp" "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginRegistry.cpp")
    target_link_libraries(voice_plugin_tests PRIVATE ZTO::MediaApi Qt6::Test Qt6::Network Qt6::Concurrent)
    zto_add_plugin_tools_test(voice_plugin_tests)
    target_compile_definitions(voice_plugin_tests PRIVATE VOICE_PLUGIN="${ZTO_VOICE_PLUGIN_PATH}" MEDIA_PLUGIN="${ZTO_MEDIA_PLUGIN_PATH}")
    target_compile_definitions(zto_tests PRIVATE ZTO_UI_VOICE_PACKAGE="${ZTO_VOICE_PLUGIN_PATH}" ZTO_UI_MEDIA_PACKAGE="${ZTO_MEDIA_PLUGIN_PATH}")
    if(MSVC)
        target_compile_options(voice_plugin_tests PRIVATE /utf-8)
    endif()
    add_test(NAME voice_plugin_panel COMMAND voice_plugin_tests -platform windows)
    set_tests_properties(voice_plugin_panel PROPERTIES TIMEOUT 300)
    set_tests_properties(voice_plugin_panel PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")
endif()

set(ZTO_DESKTOP_SHARE_PLUGIN_PATH "" CACHE FILEPATH "Independently built desktop-share plugin for panel smoke tests")
if(ZTO_DESKTOP_SHARE_PLUGIN_PATH)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME desktop_share_validation_analyzers
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop-share/validation/test_analyzers.py")
    endif()
    if(NOT ZTO_MEDIA_PLUGIN_PATH)
        message(FATAL_ERROR "ZTO_DESKTOP_SHARE_PLUGIN_PATH requires ZTO_MEDIA_PLUGIN_PATH")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)
    add_subdirectory("${ZTO_ROOT_DIR}/src/plugins/desktop-share/shared" "desktop-share-protocol")
    add_executable(desktop_share_protocol_tests desktop-share/ProtocolGoldenTests.cpp)
    target_link_libraries(desktop_share_protocol_tests PRIVATE zds_protocol Qt6::Test)
    target_compile_definitions(desktop_share_protocol_tests PRIVATE ZDS_SAMPLES_DIR="${ZTO_ROOT_DIR}/src/plugins/desktop-share/shared/samples")
    add_executable(desktop_share_udp_tests desktop-share/TileUdpTests.cpp "${ZTO_ROOT_DIR}/src/client/main/plugins/ServiceBroker.cpp")
    target_link_libraries(desktop_share_udp_tests PRIVATE zds_protocol ZTO::Api ZTO::MediaApi zto_package Qt6::Test)
    target_compile_definitions(desktop_share_udp_tests PRIVATE MEDIA_PLUGIN="${ZTO_MEDIA_PLUGIN_PATH}")
    if(MSVC)
        target_compile_options(desktop_share_protocol_tests PRIVATE /utf-8)
        target_compile_options(desktop_share_udp_tests PRIVATE /utf-8)
    endif()
    add_executable(desktop_share_stream_tests desktop-share/StreamLogicTests.cpp)
    target_link_libraries(desktop_share_stream_tests PRIVATE zds_protocol Qt6::Test)
    if(MSVC)
        target_compile_options(desktop_share_stream_tests PRIVATE /utf-8)
    endif()
    add_test(NAME desktop_share_tiles_udp COMMAND desktop_share_udp_tests)
    add_test(NAME desktop_share_protocol COMMAND desktop_share_protocol_tests)
    add_test(NAME desktop_share_stream COMMAND desktop_share_stream_tests)
    qt_add_executable(desktop_share_plugin_tests desktop-share/DesktopSharePluginTests.cpp
        "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginLoader.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginRegistry.cpp")
    target_link_libraries(desktop_share_plugin_tests PRIVATE
        ZTO::Api Qt6::Test Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Concurrent)
    zto_add_plugin_tools_test(desktop_share_plugin_tests)
    target_compile_definitions(desktop_share_plugin_tests PRIVATE
        DESKTOP_SHARE_PLUGIN="${ZTO_DESKTOP_SHARE_PLUGIN_PATH}"
        MEDIA_PLUGIN="${ZTO_MEDIA_PLUGIN_PATH}"
        VOICE_PLUGIN="${ZTO_VOICE_PLUGIN_PATH}"
        ZDS_PREVIEW_SCREENSHOT="${CMAKE_CURRENT_BINARY_DIR}/desktop-share-window-preview.png")
    if(MSVC)
        target_compile_options(desktop_share_plugin_tests PRIVATE /utf-8)
    endif()
    qt_add_executable(desktop_share_network_e2e desktop-share/DesktopShareNetworkE2E.cpp
        "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginLoader.cpp"
        "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginRegistry.cpp")
    target_link_libraries(desktop_share_network_e2e PRIVATE
        ZTO::Api Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Concurrent Qt6::Network)
    zto_add_plugin_tools_test(desktop_share_network_e2e)
    target_compile_definitions(desktop_share_network_e2e PRIVATE
        DESKTOP_SHARE_PLUGIN="${ZTO_DESKTOP_SHARE_PLUGIN_PATH}"
        MEDIA_PLUGIN="${ZTO_MEDIA_PLUGIN_PATH}")
    if(MSVC)
        target_compile_options(desktop_share_network_e2e PRIVATE /utf-8)
    endif()
    add_test(NAME desktop_share_plugin_panel COMMAND desktop_share_plugin_tests -platform windows)
    get_filename_component(zds_qt_qml_dir "${Qt6_DIR}/../../../qml" ABSOLUTE)
    set_tests_properties(desktop_share_plugin_panel PROPERTIES TIMEOUT 180)
    set_tests_properties(desktop_share_plugin_panel PROPERTIES
        ENVIRONMENT "QT_QUICK_BACKEND=software;QML2_IMPORT_PATH=${zds_qt_qml_dir}")
endif()

add_executable(relay_tests RelayTests.cpp)
target_link_libraries(relay_tests PRIVATE zto_network Qt6::Test)
add_test(NAME relay_end_to_end COMMAND relay_tests)
set_tests_properties(relay_end_to_end PROPERTIES TIMEOUT 60)

# Service fixtures exercise the generic loader; neither the host nor these
# consumers know media or voice implementation IDs.
set(ZTO_HOST_CONTRACT_DIRECTORY "${ZTO_CLIENT_CONTRACT_DIRECTORY}")
add_executable(service_fixture_worker plugins/ServiceWorkerFixture.cpp)
target_link_libraries(service_fixture_worker PRIVATE ZTO::Protocol)
foreach(letter a b)
    set(service_name "fixture.audio")
    if(letter STREQUAL "b")
        set(service_name "fixture.video")
    endif()
    configure_file(plugins/service-fixture.json.in "${CMAKE_CURRENT_BINARY_DIR}/service-${letter}/service-fixture.json" @ONLY)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/service-${letter}/manifest.json"
        "{\"execution\":{\"kind\":\"in-process\"},\"dependencies\":[{\"pluginId\":\"fixture-provider\",\"versionRange\":\">=1.0.0 <2.0.0\",\"services\":[{\"service\":\"${service_name}\",\"api\":\"1\"}]}],\"provides\":[]}")
    qt_add_plugin(service_fixture_${letter} SHARED CLASS_NAME ServiceFixture plugins/ServiceFixture.cpp)
    target_include_directories(service_fixture_${letter} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/service-${letter}")
    target_link_libraries(service_fixture_${letter} PRIVATE ZTO::Api)
    target_compile_definitions(service_fixture_${letter} PRIVATE FIXTURE_SERVICE_ID="fixture-${letter}" FIXTURE_SERVICE_NAME="${service_name}")
    zto_package_plugin(service_fixture_${letter} HOST client ID "fixture-${letter}" NAME "fixture-${letter}" VERSION "1.0.0"
        API "${FIXTURE_API_VERSION}" IID "vusiif.zto.api.PluginInterface/3.0" SERVICE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/service-${letter}/manifest.json")
endforeach()
qt_add_executable(plugin_services_tests PluginServicesTests.cpp
    "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginLoader.cpp" "${ZTO_ROOT_DIR}/src/client/main/plugins/PluginRegistry.cpp")
target_link_libraries(plugin_services_tests PRIVATE ZTO::Api Qt6::Test Qt6::Concurrent)
zto_add_plugin_tools_test(plugin_services_tests)
target_compile_definitions(plugin_services_tests PRIVATE
    SERVICE_WORKER="$<TARGET_FILE:service_fixture_worker>"
    SERVICE_CONSUMER_A="$<TARGET_FILE_DIR:service_fixture_a>/service_fixture_a.z2plugin"
    SERVICE_CONSUMER_B="$<TARGET_FILE_DIR:service_fixture_b>/service_fixture_b.z2plugin"
    QT_CORE_LIBRARY="$<TARGET_FILE:Qt6::Core>" QT_NETWORK_LIBRARY="$<TARGET_FILE:Qt6::Network>")
add_dependencies(plugin_services_tests service_fixture_worker service_fixture_a service_fixture_b)
add_test(NAME plugin_services COMMAND plugin_services_tests -platform windows)
set_tests_properties(plugin_services PROPERTIES TIMEOUT 120)

# Feature regression targets keep their own sources so independent changes do
# not collide in this file.
foreach(zto_feature update reset)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${zto_feature}/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/${zto_feature}" "${CMAKE_CURRENT_BINARY_DIR}/${zto_feature}")
    endif()
endforeach()
