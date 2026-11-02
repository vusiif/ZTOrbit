set(ZTO_MAIN_SOURCES
    AppController.cpp RecordModel.cpp SystemOperations.cpp AppLog.cpp SingleInstance.cpp PluginImport.cpp
    ClientNetworkService.cpp ZeroTierClient.cpp NetworkMembers.cpp MemberPing.cpp NetworkBundle.cpp LocalConfig.cpp MacPlatform.cpp
    storage/StorageContext.cpp storage/CoordinationLock.cpp storage/SettingsStore.cpp storage/PluginDataStore.cpp storage/StorageMaintenance.cpp storage/StorageMaintenanceScheduler.cpp
    plugins/PluginRegistry.cpp plugins/PluginLoader.cpp plugins/HostServicesImpl.cpp plugins/ServiceBroker.cpp)
set(ZTO_MAIN_HEADERS
    AppController.h RecordModel.h SystemOperations.h AppLog.h SingleInstance.h PluginImport.h ZeroTierClient.h
    NetworkBundle.h LocalConfig.h MemberPing.h MacPlatform.h storage/StorageContext.h storage/CoordinationLock.h storage/SettingsStore.h storage/PluginDataStore.h storage/StorageMaintenance.h storage/StorageMaintenanceScheduler.h
    plugins/PluginRegistry.h plugins/PluginLoader.h plugins/HostServicesImpl.h plugins/ServiceBroker.h)
list(TRANSFORM ZTO_MAIN_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM ZTO_MAIN_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(ZTO_MAIN_SOURCES "${ZTO_MAIN_SOURCES}" PARENT_SCOPE)
set(ZTO_MAIN_HEADERS "${ZTO_MAIN_HEADERS}" PARENT_SCOPE)

# Shared build settings, with separate compilations for production and tests.
function(zto_configure_host target)
    zto_embed_host_contract(${target} "${ZTO_CLIENT_CONTRACT_DIRECTORY}")
    foreach(module IN LISTS ZTO_CLIENT_HOST_QT_MODULES)
        target_link_libraries(${target} PRIVATE Qt6::${module})
    endforeach()
    target_link_libraries(${target} PRIVATE zto_network zto_package ZTO::Api Qt6::Concurrent Qt6::Network Qt6::QuickControls2)
    target_include_directories(${target} PRIVATE "${ZTO_ROOT_DIR}/src/client/main")
    target_compile_definitions(${target} PRIVATE
        ZTO_VERSION="${ZTO_VERSION}"
        ZTO_API_VERSION="${ZTO_API_VERSION}"
        ZTOHUB_VERSION="${ZTOHUB_VERSION}"
        ZTOHUB_API_VERSION="${ZTOHUB_API_VERSION}")
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
    if(WIN32)
        target_link_libraries(${target} PRIVATE shell32 advapi32 comctl32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
endfunction()

qt_add_executable(zto WIN32 main.cpp ${ZTO_MAIN_SOURCES} ${ZTO_MAIN_HEADERS})
zto_configure_host(zto)
# Widgets supplies Qt.labs.platform's native tray/menu fallback only; application
# windows, pages, dialogs and plugin pages are Qt Quick items.
target_link_libraries(zto PRIVATE Qt6::Widgets)
qt_policy(SET QTP0004 NEW)
function(zto_add_qml_ui target)
    qt_policy(SET QTP0004 NEW)
    set(qml_options)
    if(NOT target STREQUAL "zto")
        # Qt 6.8/MSBuild cannot create qmlcache directories containing _.. when
        # the test target imports QML sources outside its own source directory.
        list(APPEND qml_options NO_CACHEGEN)
    endif()
    set(qml_sources)
    foreach(name SettingsPage NotificationBanner AppToolTip Main Theme ActionButton RecordList Guide StartupError LazyPopup AppPopup AppDialog AppMenu AppMenuItem BoundedScrollView GearButton GearIcon ThemeToggle)
        set(source "${ZTO_ROOT_DIR}/src/client/main/qml/${name}.qml")
        set_source_files_properties("${source}" PROPERTIES QT_RESOURCE_ALIAS "qml/${name}.qml")
        list(APPEND qml_sources "${source}")
    endforeach()
    set_source_files_properties("${ZTO_ROOT_DIR}/src/client/main/qml/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    qt_add_qml_module(${target} URI ZTOrbit VERSION 1.0 RESOURCE_PREFIX /qt/qml ${qml_options} QML_FILES ${qml_sources})
endfunction()
zto_add_qml_ui(zto)
set_target_properties(zto PROPERTIES OUTPUT_NAME "Z2")
if(APPLE)
    set_target_properties(zto PROPERTIES
        MACOSX_BUNDLE TRUE MACOSX_BUNDLE_BUNDLE_NAME "Z2"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.zto"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ZTO_VERSION_NUMERIC}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${ZTO_VERSION_NUMERIC}"
        MACOSX_BUNDLE_INFO_PLIST "${ZTO_ROOT_DIR}/src/client/assets/Info.plist.in")
endif()
qt_add_resources(zto app_icon PREFIX "/" BASE "${ZTO_ROOT_DIR}/src/client"
    FILES "${ZTO_ROOT_DIR}/src/client/assets/app-icon.ico" "${ZTO_ROOT_DIR}/src/client/assets/app-icon.png")
if(WIN32)
    configure_file("${ZTO_ROOT_DIR}/src/client/assets/app-icon.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/app-icon.rc" @ONLY)
    target_sources(zto PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app-icon.rc")
    if(MSVC)
        target_link_options(zto PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    endif()
endif()
if(MSVC)
    target_compile_options(zto PRIVATE $<$<CONFIG:Release>:/O1 /GL /Gy>)
    target_link_options(zto PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()

if(WIN32)
    foreach(app zto)
        add_custom_command(TARGET ${app} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:zto_sodium>" "$<TARGET_FILE_DIR:${app}>")
    endforeach()
endif()
