# Automatic update feature module.
#
# The parent directory adds this file automatically when it exists and appends
# the sources below to ZTO_MAIN_SOURCES (relative paths, because the parent
# prepends its own source directory afterwards).
#
# Developer inputs (see docs/auto-update.md):
#   -DZTO_UPDATE_PUBLIC_KEY_BASE64=<base64 Ed25519 public key>
# The user must choose a manifest URL in settings. No URL is built in.

set(ZTO_UPDATE_SOURCES
    UpdateVersion.cpp
    UpdateManifest.cpp
    UpdateCrypto.cpp
    UpdateConfig.cpp
    UpdateDeflate.cpp
    UpdateArchive.cpp
    UpdateHttp.cpp
    UpdateStorage.cpp
    UpdateController.cpp
    UpdaterPlan.cpp
    UpdaterEngine.cpp)
set(ZTO_UPDATE_HEADERS
    UpdateVersion.h
    UpdateManifest.h
    UpdateCrypto.h
    UpdateConfig.h
    UpdateDefaults.h.in
    UpdateDeflate.h
    UpdateArchive.h
    UpdateHttp.h
    UpdateStorage.h
    UpdateController.h
    UpdaterPlan.h
    UpdaterEngine.h)

set(ZTO_UPDATE_PUBLIC_KEY_BASE64 "" CACHE STRING
    "Base64 Ed25519 public key trusted for update manifests (empty = not configured)")

set(ZTO_UPDATE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE INTERNAL
    "Directory containing the generated UpdateDefaults.h")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/UpdateDefaults.h.in"
    "${ZTO_UPDATE_GENERATED_DIR}/UpdateDefaults.h" @ONLY)

# The host target is created after this directory returns, so make the generated
# header visible to it through the parent directory's include path.
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." APPEND PROPERTY
    INCLUDE_DIRECTORIES "${ZTO_UPDATE_GENERATED_DIR}")

list(TRANSFORM ZTO_UPDATE_SOURCES PREPEND "update/")
list(TRANSFORM ZTO_UPDATE_HEADERS PREPEND "update/")
list(APPEND ZTO_MAIN_SOURCES ${ZTO_UPDATE_SOURCES})
list(APPEND ZTO_MAIN_HEADERS ${ZTO_UPDATE_HEADERS})
set(ZTO_MAIN_SOURCES "${ZTO_MAIN_SOURCES}" PARENT_SCOPE)
set(ZTO_MAIN_HEADERS "${ZTO_MAIN_HEADERS}" PARENT_SCOPE)

# z2_updater.exe: pure Win32 C++17, no Qt dependency. Deployed next to Z2.exe by
# the build script (see .agents/skills/zto-build/scripts/build-windows.ps1).
if(WIN32)
    add_executable(z2_updater WIN32
        "${CMAKE_CURRENT_SOURCE_DIR}/helper/UpdaterMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/helper/NativeDownload.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/UpdaterPlan.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/UpdaterEngine.cpp")
    set_target_properties(z2_updater PROPERTIES OUTPUT_NAME "z2_updater"
        RUNTIME_OUTPUT_DIRECTORY "${ZTO_BUILD_DIR}")
    target_include_directories(z2_updater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(z2_updater PRIVATE cxx_std_17)
    target_compile_definitions(z2_updater PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(z2_updater PRIVATE shell32 bcrypt winhttp)
    if(MSVC)
        target_compile_options(z2_updater PRIVATE /utf-8 /W3)
    endif()
endif()
