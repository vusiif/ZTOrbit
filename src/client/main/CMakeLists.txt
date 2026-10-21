set(ZTGUI_MAIN_SOURCES
    AppController.cpp RecordModel.cpp SystemOperations.cpp AppLog.cpp SingleInstance.cpp PluginImport.cpp
    ClientNetworkService.cpp ZeroTierClient.cpp NetworkMembers.cpp MemberPing.cpp NetworkBundle.cpp MacPlatform.cpp
    storage/StorageContext.cpp storage/CoordinationLock.cpp storage/SettingsStore.cpp storage/PluginDataStore.cpp storage/StorageMaintenance.cpp storage/StorageMaintenanceScheduler.cpp
    plugins/PluginRegistry.cpp plugins/PluginLoader.cpp plugins/HostServicesImpl.cpp plugins/ServiceBroker.cpp)
set(ZTGUI_MAIN_HEADERS
    AppController.h RecordModel.h SystemOperations.h AppLog.h SingleInstance.h PluginImport.h ZeroTierClient.h
    NetworkBundle.h MemberPing.h MacPlatform.h storage/StorageContext.h storage/CoordinationLock.h storage/SettingsStore.h storage/PluginDataStore.h storage/StorageMaintenance.h storage/StorageMaintenanceScheduler.h
    plugins/PluginRegistry.h plugins/PluginLoader.h plugins/HostServicesImpl.h plugins/ServiceBroker.h)
list(TRANSFORM ZTGUI_MAIN_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM ZTGUI_MAIN_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(ZTGUI_MAIN_SOURCES "${ZTGUI_MAIN_SOURCES}" PARENT_SCOPE)
set(ZTGUI_MAIN_HEADERS "${ZTGUI_MAIN_HEADERS}" PARENT_SCOPE)

# Shared build settings, with separate compilations for production and tests.
function(ztgui_configure_host target)
    ztgui_embed_host_contract(${target} "${ZTGUI_CLIENT_CONTRACT_DIRECTORY}")
    foreach(module IN LISTS ZTGUI_CLIENT_HOST_QT_MODULES)
        target_link_libraries(${target} PRIVATE Qt6::${module})
    endforeach()
    target_link_libraries(${target} PRIVATE ztgui_network ztgui_package ZTGUI::Api Qt6::Concurrent Qt6::Network Qt6::QuickControls2)
    target_include_directories(${target} PRIVATE "${ZTGUI_ROOT_DIR}/src/client/main")
    target_compile_definitions(${target} PRIVATE
        ZEROTIER_GUI_VERSION="${ZEROTIER_VERSION}"
        ZTGUI_API_VERSION="${ZTGUI_API_VERSION}"
        ZTGUI_SERVER_VERSION="${ZTGUI_SERVER_VERSION}"
        ZTGUI_SERVER_API_VERSION="${ZTGUI_SERVER_API_VERSION}")
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTGUI_BUILD_DIR}")
    if(WIN32)
        target_link_libraries(${target} PRIVATE shell32 advapi32 comctl32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
endfunction()

qt_add_executable(zerotier_gui WIN32 main.cpp ${ZTGUI_MAIN_SOURCES} ${ZTGUI_MAIN_HEADERS})
ztgui_configure_host(zerotier_gui)
# Widgets supplies Qt.labs.platform's native tray/menu fallback only; application
# windows, pages, dialogs and plugin pages are Qt Quick items.
target_link_libraries(zerotier_gui PRIVATE Qt6::Widgets)
qt_policy(SET QTP0004 NEW)
function(ztgui_add_qml_ui target)
    qt_policy(SET QTP0004 NEW)
    set(qml_options)
    if(NOT target STREQUAL "zerotier_gui")
        # Qt 6.8/MSBuild cannot create qmlcache directories containing _.. when
        # the test target imports QML sources outside its own source directory.
        list(APPEND qml_options NO_CACHEGEN)
    endif()
    set(qml_sources)
    foreach(name SettingsPage AppToolTip Main Theme ActionButton RecordList Guide StartupError LazyPopup AppPopup AppDialog AppMenu AppMenuItem BoundedScrollView GearButton GearIcon ThemeToggle)
        set(source "${ZTGUI_ROOT_DIR}/src/client/main/qml/${name}.qml")
        set_source_files_properties("${source}" PROPERTIES QT_RESOURCE_ALIAS "qml/${name}.qml")
        list(APPEND qml_sources "${source}")
    endforeach()
    set_source_files_properties("${ZTGUI_ROOT_DIR}/src/client/main/qml/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    qt_add_qml_module(${target} URI ZTGUI VERSION 1.0 RESOURCE_PREFIX /qt/qml ${qml_options} QML_FILES ${qml_sources})
endfunction()
ztgui_add_qml_ui(zerotier_gui)
set_target_properties(zerotier_gui PROPERTIES OUTPUT_NAME "ZTGUI")
if(APPLE)
    set_target_properties(zerotier_gui PROPERTIES
        MACOSX_BUNDLE TRUE MACOSX_BUNDLE_BUNDLE_NAME "ZTGUI"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.ztgui"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ZEROTIER_VERSION_NUMERIC}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${ZEROTIER_VERSION_NUMERIC}"
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

if(WIN32)
    foreach(app zerotier_gui)
        add_custom_command(TARGET ${app} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ztgui_sodium>" "$<TARGET_FILE_DIR:${app}>")
    endforeach()
endif()
