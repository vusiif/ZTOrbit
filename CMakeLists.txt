cmake_minimum_required(VERSION 3.21.1)
project(zerotier_gui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)

# Static Qt builds must use the same static MSVC runtime as their SDK.
get_target_property(QT_CORE_LIBRARY_TYPE Qt6::Core TYPE)
if(MSVC AND QT_CORE_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(SOURCES
    main.cpp
    AppStyle.cpp
    AppLog.cpp
    UiTransitions.cpp
    MainWindow.cpp
    DataTable.cpp
    ZeroTierClient.cpp
    NetworkBundle.cpp
    MacPlatform.cpp
    tabs/ServiceTab.cpp
    tabs/InfoTab.cpp
    tabs/PeersTab.cpp
    tabs/NetworkTab.cpp
    tabs/MoonTab.cpp
)

set(HEADERS
    MainWindow.h
    AppStyle.h
    AppLog.h
    UiTransitions.h
    DataTable.h
    ZeroTierClient.h
    NetworkBundle.h
    MacPlatform.h
    tabs/ServiceTab.h
    tabs/InfoTab.h
    tabs/PeersTab.h
    tabs/NetworkTab.h
    tabs/MoonTab.h
)

qt_add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ZTGUI")
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "ZTGUI"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.ztgui"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets)
add_compile_definitions(ZEROTIER_GUI_VERSION="${PROJECT_VERSION}")
qt_add_resources(${PROJECT_NAME} app_icon
    PREFIX "/"
    FILES assets/app-icon.ico assets/app-icon.png
)
if(QT_CORE_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
    # Keep the headless smoke test available in the standalone executable too.
    qt_import_plugins(${PROJECT_NAME} INCLUDE Qt6::QOffscreenIntegrationPlugin)
endif()
if(WIN32)
    configure_file(assets/app-icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app-icon.rc" @ONLY)
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app-icon.rc")
    target_link_libraries(${PROJECT_NAME} PRIVATE shell32 advapi32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        # ZeroTier's service token is readable by administrators. Apply this
        # only to the shipped app; regression tests remain unprivileged.
        target_link_options(${PROJECT_NAME} PRIVATE
            "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    endif()
endif()

option(ZEROTIER_BUILD_TESTS "Build GUI regression tests with a fake ZeroTier CLI" OFF)
if(ZEROTIER_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(fake_zerotier_cli tests/FakeCli.cpp)
    set_target_properties(fake_zerotier_cli PROPERTIES OUTPUT_NAME "zerotier-cli")
    target_link_libraries(fake_zerotier_cli PRIVATE Qt6::Core)
    set(TEST_SOURCES ${SOURCES})
    list(REMOVE_ITEM TEST_SOURCES main.cpp)
    qt_add_executable(zerotier_gui_tests tests/GuiTests.cpp ${TEST_SOURCES} ${HEADERS})
    target_link_libraries(zerotier_gui_tests PRIVATE Qt6::Widgets Qt6::Test)
    target_compile_definitions(zerotier_gui_tests PRIVATE ZEROTIER_TESTING)
    if(QT_CORE_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
        qt_import_plugins(zerotier_gui_tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_dependencies(zerotier_gui_tests fake_zerotier_cli)
    if(WIN32)
        target_link_libraries(zerotier_gui_tests PRIVATE shell32 advapi32)
        target_compile_definitions(zerotier_gui_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(fake_zerotier_cli PRIVATE /utf-8)
        target_compile_options(zerotier_gui_tests PRIVATE /utf-8)
    endif()
    add_test(NAME gui_regressions COMMAND zerotier_gui_tests -platform offscreen -o test-results.txt,txt)
endif()

# Size and memory optimizations for Release
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8
        $<$<CONFIG:Release>:/O1 /GL /Gy>)
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()
