cmake_minimum_required(VERSION 3.21.1)

# This SDK can be configured independently with cmake -S src/client/api.
set(ZTO_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../../VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ZTO_VERSION_FILE}")
file(READ "${ZTO_VERSION_FILE}" ZTO_APP_VERSION)
string(STRIP "${ZTO_APP_VERSION}" ZTO_APP_VERSION)
if(NOT ZTO_APP_VERSION MATCHES "^0\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION must be 0.<core>.<patch>[-qualifier]")
endif()
set(ZTO_API_VERSION "0.${CMAKE_MATCH_1}${CMAKE_MATCH_3}")
set(ZTO_API_VERSION_NUMERIC "0.${CMAKE_MATCH_1}")
project(ZTOrbitPluginApi VERSION "${ZTO_API_VERSION_NUMERIC}" LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake")
find_package(Qt6 6.8 REQUIRED COMPONENTS ${ZTO_CLIENT_HOST_QT_MODULES})
zto_host_runtime_contract(client ZTO_HOST_CONTRACT_DIRECTORY)
set(ZTO_CLIENT_CONTRACT_DIRECTORY "${ZTO_HOST_CONTRACT_DIRECTORY}" CACHE INTERNAL "" FORCE)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

configure_file(ApiVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/zto/ApiVersion.h" @ONLY)
add_library(zto_plugin_api INTERFACE)
add_library(ZTO::Api ALIAS zto_plugin_api)
set_target_properties(zto_plugin_api PROPERTIES EXPORT_NAME Api)
target_compile_features(zto_plugin_api INTERFACE cxx_std_17)
target_link_libraries(zto_plugin_api INTERFACE Qt6::Quick Qt6::Qml)
target_include_directories(zto_plugin_api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

add_library(zto_protocol INTERFACE)
add_library(ZTO::Protocol ALIAS zto_protocol)
set_target_properties(zto_protocol PROPERTIES EXPORT_NAME Protocol)
target_link_libraries(zto_protocol INTERFACE Qt6::Core Qt6::Network)
target_compile_features(zto_protocol INTERFACE cxx_std_17)
target_include_directories(zto_protocol INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(zto_plugin_api INTERFACE zto_protocol)
add_library(zto_services_api INTERFACE)
add_library(ZTO::Services ALIAS zto_services_api)
set_target_properties(zto_services_api PROPERTIES EXPORT_NAME Services)
target_link_libraries(zto_services_api INTERFACE zto_protocol)
target_include_directories(zto_services_api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set(ZTO_API_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ZTOrbitPluginApi")
configure_package_config_file(ZTOrbitPluginApiConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitPluginApiConfig.cmake"
    INSTALL_DESTINATION "${ZTO_API_CMAKE_DIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitPluginApiConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
install(TARGETS zto_plugin_api zto_protocol zto_services_api EXPORT ZTOrbitPluginApiTargets)
install(EXPORT ZTOrbitPluginApiTargets NAMESPACE ZTO:: DESTINATION "${ZTO_API_CMAKE_DIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Remove retired headers when updating an existing development SDK installation.
install(CODE "file(REMOVE
    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/zto/PluginPreHandle.h\"
    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/zto/PluginInitialization.h\")")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/zto/ApiVersion.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/zto")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitPluginApiConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitPluginApiConfigVersion.cmake"
    DESTINATION "${ZTO_API_CMAKE_DIR}")

if(NOT TARGET zto_package)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared" "${CMAKE_CURRENT_BINARY_DIR}/shared-tools")
endif()
install(TARGETS ztplugin_pack RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/PackagePlugin.cmake" DESTINATION "${ZTO_API_CMAKE_DIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/RuntimeTools.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/GenerateHostContract.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake" DESTINATION "${ZTO_API_CMAKE_DIR}")
install(DIRECTORY "${ZTO_HOST_CONTRACT_DIRECTORY}/" DESTINATION "${ZTO_API_CMAKE_DIR}/contracts")
install(FILES "${ZTO_HOST_CONTRACT_DIRECTORY}/Release.json" DESTINATION . RENAME host-runtime-contract.json)
if(WIN32)
    install(FILES "$<TARGET_FILE:zto_sodium>" DESTINATION bin)
    install(FILES "$<TARGET_FILE:Qt6::Core>" DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
endif()

set(ZTO_PACKAGE_TOOL "$<TARGET_FILE:ztplugin_pack>")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/PackagePlugin.cmake")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/libsodium-LICENSE.txt" DESTINATION licenses)
zto_install_sdk_metadata(client)
