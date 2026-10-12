set(ZTGUI_MAIN_SOURCES
    AppStyle.cpp AppLog.cpp UiTransitions.cpp MainWindow.cpp DataTable.cpp SingleInstance.cpp PluginImport.cpp GuideOverlay.cpp
    ZeroTierClient.cpp NetworkMembers.cpp NetworkMemberRow.cpp NetworkBundle.cpp MacPlatform.cpp plugins/PluginRegistry.cpp plugins/PluginLoader.cpp plugins/HostServicesImpl.cpp
    tabs/ServiceTab.cpp tabs/InfoTab.cpp tabs/PeersTab.cpp tabs/NetworkTab.cpp tabs/MoonTab.cpp)
set(ZTGUI_MAIN_HEADERS
    MainWindow.h AppStyle.h AppLog.h UiTransitions.h DataTable.h ZeroTierClient.h
    NetworkBundle.h NetworkMemberRow.h MacPlatform.h plugins/PluginRegistry.h plugins/PluginLoader.h plugins/HostServicesImpl.h
    tabs/ServiceTab.h tabs/InfoTab.h tabs/PeersTab.h tabs/NetworkTab.h tabs/MoonTab.h)
list(TRANSFORM ZTGUI_MAIN_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM ZTGUI_MAIN_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(ZTGUI_MAIN_SOURCES "${ZTGUI_MAIN_SOURCES}" PARENT_SCOPE)
set(ZTGUI_MAIN_HEADERS "${ZTGUI_MAIN_HEADERS}" PARENT_SCOPE)

# Shared build settings, with separate compilations for production and tests.
function(ztgui_configure_host target)
    target_link_libraries(${target} PRIVATE ZTGUI::Api Qt6::Concurrent Qt6::Network)
    target_include_directories(${target} PRIVATE "${ZTGUI_ROOT_DIR}/src/client/main")
    target_compile_definitions(${target} PRIVATE ZEROTIER_GUI_VERSION="${ZEROTIER_VERSION}")
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTGUI_BUILD_DIR}")
    if(WIN32)
        target_link_libraries(${target} PRIVATE shell32 advapi32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
endfunction()

qt_add_executable(zerotier_gui WIN32 main.cpp ${ZTGUI_MAIN_SOURCES} ${ZTGUI_MAIN_HEADERS})
ztgui_configure_host(zerotier_gui)
set_target_properties(zerotier_gui PROPERTIES OUTPUT_NAME "ZTGUI")
if(APPLE)
    set_target_properties(zerotier_gui PROPERTIES
        MACOSX_BUNDLE TRUE MACOSX_BUNDLE_BUNDLE_NAME "ZTGUI"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.ztgui"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ZEROTIER_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${ZEROTIER_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${ZTGUI_ROOT_DIR}/src/client/assets/Info.plist.in")
endif()
qt_add_resources(zerotier_gui app_icon PREFIX "/" BASE "${ZTGUI_ROOT_DIR}/src/client"
    FILES "${ZTGUI_ROOT_DIR}/src/client/assets/app-icon.ico" "${ZTGUI_ROOT_DIR}/src/client/assets/app-icon.png")
if(WIN32)
    configure_file("${ZTGUI_ROOT_DIR}/src/client/assets/app-icon.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/app-icon.rc" @ONLY)
    target_sources(zerotier_gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app-icon.rc")
    if(MSVC)
        target_link_options(zerotier_gui PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    endif()
endif()
if(MSVC)
    target_compile_options(zerotier_gui PRIVATE $<$<CONFIG:Release>:/O1 /GL /Gy>)
    target_link_options(zerotier_gui PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()
