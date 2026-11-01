cmake_minimum_required(VERSION 3.21.1)
include("${CMAKE_CURRENT_LIST_DIR}/../Version.cmake")
project(ZTOrbitHubPluginApi VERSION "${ZTO_API_VERSION}" LANGUAGES CXX)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake")
zto_host_runtime_contract(server ZTO_HOST_CONTRACT_DIRECTORY)
set(ZTOHUB_CONTRACT_DIRECTORY "${ZTO_HOST_CONTRACT_DIRECTORY}" CACHE INTERNAL "" FORCE)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
string(REGEX REPLACE "^0\\." "" ZTO_API_CORE "${ZTO_API_VERSION}")
configure_file(ApiVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/ztohub/ApiVersion.h" @ONLY)
add_library(ztohub_protocol INTERFACE)
add_library(ZTOrbitHub::Protocol ALIAS ztohub_protocol)
set_target_properties(ztohub_protocol PROPERTIES EXPORT_NAME Protocol)
target_link_libraries(ztohub_protocol INTERFACE Qt6::Core Qt6::Network)
target_compile_features(ztohub_protocol INTERFACE cxx_std_17)
target_include_directories(ztohub_protocol INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
add_library(ztohub_api INTERFACE)
add_library(ZTOrbitHub::Api ALIAS ztohub_api)
set_target_properties(ztohub_api PROPERTIES EXPORT_NAME Api)
target_link_libraries(ztohub_api INTERFACE ztohub_protocol Qt6::Network)
target_compile_features(ztohub_api INTERFACE cxx_std_17)
target_include_directories(ztohub_api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set(sdk_cmake "${CMAKE_INSTALL_LIBDIR}/cmake/ZTOrbitHubPluginApi")
configure_package_config_file(ZTOrbitHubPluginApiConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitHubPluginApiConfig.cmake" INSTALL_DESTINATION "${sdk_cmake}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitHubPluginApiConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
install(TARGETS ztohub_api ztohub_protocol EXPORT ZTOrbitHubPluginApiTargets)
install(EXPORT ZTOrbitHubPluginApiTargets NAMESPACE ZTOrbitHub:: DESTINATION "${sdk_cmake}")
install(DIRECTORY include/ "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/ztohub/ApiVersion.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ztohub")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitHubPluginApiConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/ZTOrbitHubPluginApiConfigVersion.cmake" DESTINATION "${sdk_cmake}")

if(NOT TARGET zto_package)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared" "${CMAKE_CURRENT_BINARY_DIR}/shared-tools")
endif()
install(TARGETS ztplugin_pack RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/PackagePlugin.cmake" DESTINATION "${sdk_cmake}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/RuntimeTools.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/GenerateHostContract.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake" DESTINATION "${sdk_cmake}")
install(DIRECTORY "${ZTO_HOST_CONTRACT_DIRECTORY}/" DESTINATION "${sdk_cmake}/contracts")
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
zto_install_sdk_metadata(server)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ZTO_HOST_CONTRACT_DIRECTORY "${ZTO_HOST_CONTRACT_DIRECTORY}" PARENT_SCOPE)
endif()
