cmake_minimum_required(VERSION 3.21.1)

# This SDK can be configured independently with cmake -S src/client/api.
set(ZTGUI_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../../VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ZTGUI_VERSION_FILE}")
file(READ "${ZTGUI_VERSION_FILE}" ZTGUI_APP_VERSION)
string(STRIP "${ZTGUI_APP_VERSION}" ZTGUI_APP_VERSION)
if(NOT ZTGUI_APP_VERSION MATCHES "^0\\.([0-9]+)\\.[0-9]+$")
    message(FATAL_ERROR "Pre-release VERSION must be 0.core.patch")
endif()
set(ZTGUI_API_VERSION "0.${CMAKE_MATCH_1}")
project(ZTGUIPluginApi VERSION "${ZTGUI_API_VERSION}" LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake")
find_package(Qt6 6.8 REQUIRED COMPONENTS ${ZTGUI_CLIENT_HOST_QT_MODULES})
ztgui_host_runtime_contract(client ZTGUI_HOST_CONTRACT_DIRECTORY)
set(ZTGUI_CLIENT_CONTRACT_DIRECTORY "${ZTGUI_HOST_CONTRACT_DIRECTORY}" CACHE INTERNAL "" FORCE)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

configure_file(ApiVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/ztgui/ApiVersion.h" @ONLY)
add_library(ztgui_plugin_api INTERFACE)
add_library(ZTGUI::Api ALIAS ztgui_plugin_api)
set_target_properties(ztgui_plugin_api PROPERTIES EXPORT_NAME Api)
target_compile_features(ztgui_plugin_api INTERFACE cxx_std_17)
target_link_libraries(ztgui_plugin_api INTERFACE Qt6::Quick Qt6::Qml)
target_include_directories(ztgui_plugin_api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

add_library(ztgui_protocol INTERFACE)
add_library(ZTGUI::Protocol ALIAS ztgui_protocol)
set_target_properties(ztgui_protocol PROPERTIES EXPORT_NAME Protocol)
target_link_libraries(ztgui_protocol INTERFACE Qt6::Core Qt6::Network)
target_compile_features(ztgui_protocol INTERFACE cxx_std_17)
target_include_directories(ztgui_protocol INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(ztgui_plugin_api INTERFACE ztgui_protocol)

set(ZTGUI_API_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ZTGUIPluginApi")
configure_package_config_file(ZTGUIPluginApiConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ZTGUIPluginApiConfig.cmake"
    INSTALL_DESTINATION "${ZTGUI_API_CMAKE_DIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ZTGUIPluginApiConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
install(TARGETS ztgui_plugin_api ztgui_protocol EXPORT ZTGUIPluginApiTargets)
install(EXPORT ZTGUIPluginApiTargets NAMESPACE ZTGUI:: DESTINATION "${ZTGUI_API_CMAKE_DIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Remove retired headers when updating an existing development SDK installation.
install(CODE "file(REMOVE
    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/ztgui/PluginPreHandle.h\"
    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/ztgui/PluginInitialization.h\")")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/ztgui/ApiVersion.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ztgui")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ZTGUIPluginApiConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ZTGUIPluginApiConfigVersion.cmake"
    DESTINATION "${ZTGUI_API_CMAKE_DIR}")

if(NOT TARGET ztgui_package)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared" "${CMAKE_CURRENT_BINARY_DIR}/shared-tools")
endif()
install(TARGETS ztplugin_pack RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/PackagePlugin.cmake" DESTINATION "${ZTGUI_API_CMAKE_DIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/RuntimeTools.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/GenerateHostContract.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake" DESTINATION "${ZTGUI_API_CMAKE_DIR}")
install(DIRECTORY "${ZTGUI_HOST_CONTRACT_DIRECTORY}/" DESTINATION "${ZTGUI_API_CMAKE_DIR}/contracts")
install(FILES "${ZTGUI_HOST_CONTRACT_DIRECTORY}/Release.json" DESTINATION . RENAME host-runtime-contract.json)
if(WIN32)
    install(FILES "$<TARGET_FILE:ztgui_sodium>" DESTINATION bin)
    install(FILES "$<TARGET_FILE:Qt6::Core>" DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
endif()

set(ZTGUI_PACKAGE_TOOL "$<TARGET_FILE:ztplugin_pack>")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/PackagePlugin.cmake")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/libsodium-LICENSE.txt" DESTINATION licenses)
ztgui_install_sdk_metadata(client)
