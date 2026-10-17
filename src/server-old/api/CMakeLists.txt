cmake_minimum_required(VERSION 3.21.1)
include("${CMAKE_CURRENT_LIST_DIR}/../Version.cmake")
project(ZTServerPluginApi VERSION "${ZTGUI_API_VERSION}" LANGUAGES CXX)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake")
ztgui_host_runtime_contract(server ZTGUI_HOST_CONTRACT_DIRECTORY)
set(ZTGUI_SERVER_CONTRACT_DIRECTORY "${ZTGUI_HOST_CONTRACT_DIRECTORY}" CACHE INTERNAL "" FORCE)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
string(REGEX REPLACE "^0\\." "" ZTGUI_API_CORE "${ZTGUI_API_VERSION}")
configure_file(ApiVersion.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/ztserver/ApiVersion.h" @ONLY)
add_library(ztserver_protocol INTERFACE)
add_library(ZTServer::Protocol ALIAS ztserver_protocol)
set_target_properties(ztserver_protocol PROPERTIES EXPORT_NAME Protocol)
target_link_libraries(ztserver_protocol INTERFACE Qt6::Core Qt6::Network)
target_compile_features(ztserver_protocol INTERFACE cxx_std_17)
target_include_directories(ztserver_protocol INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
add_library(ztserver_api INTERFACE)
add_library(ZTServer::Api ALIAS ztserver_api)
set_target_properties(ztserver_api PROPERTIES EXPORT_NAME Api)
target_link_libraries(ztserver_api INTERFACE ztserver_protocol Qt6::Network)
target_compile_features(ztserver_api INTERFACE cxx_std_17)
target_include_directories(ztserver_api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set(sdk_cmake "${CMAKE_INSTALL_LIBDIR}/cmake/ZTServerPluginApi")
configure_package_config_file(ZTServerPluginApiConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/ZTServerPluginApiConfig.cmake" INSTALL_DESTINATION "${sdk_cmake}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ZTServerPluginApiConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
install(TARGETS ztserver_api ztserver_protocol EXPORT ZTServerPluginApiTargets)
install(EXPORT ZTServerPluginApiTargets NAMESPACE ZTServer:: DESTINATION "${sdk_cmake}")
install(DIRECTORY include/ "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/ztserver/ApiVersion.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ztserver")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ZTServerPluginApiConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/ZTServerPluginApiConfigVersion.cmake" DESTINATION "${sdk_cmake}")

if(NOT TARGET ztgui_package)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared" "${CMAKE_CURRENT_BINARY_DIR}/shared-tools")
endif()
install(TARGETS ztplugin_pack RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/PackagePlugin.cmake" DESTINATION "${sdk_cmake}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/CollectPluginRuntime.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/RuntimeTools.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/GenerateHostContract.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/package/HostRuntimeContract.cmake" DESTINATION "${sdk_cmake}")
install(DIRECTORY "${ZTGUI_HOST_CONTRACT_DIRECTORY}/" DESTINATION "${sdk_cmake}/contracts")
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
ztgui_install_sdk_metadata(server)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ZTGUI_HOST_CONTRACT_DIRECTORY "${ZTGUI_HOST_CONTRACT_DIRECTORY}" PARENT_SCOPE)
endif()
