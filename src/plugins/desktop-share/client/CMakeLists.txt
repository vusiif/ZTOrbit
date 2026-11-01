cmake_minimum_required(VERSION 3.21.1)
project(zto_desktop_share_client LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(ZTOrbitPluginApi 0.5 EXACT CONFIG REQUIRED)
find_package(ZTOrbitMediaApi 2 CONFIG REQUIRED COMPONENTS Quick)
find_package(Qt6 6.8 REQUIRED COMPONENTS Network)

configure_file(plugin.json.in "${CMAKE_CURRENT_BINARY_DIR}/plugin.json" @ONLY)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared" "${CMAKE_CURRENT_BINARY_DIR}/shared")

qt_add_plugin(zto_desktop_share SHARED
    CLASS_NAME vusiif::zto::desktop_share::DesktopSharePlugin
    Plugin.cpp DeskShareController.cpp DeskShareController.h
    Preferences.cpp Preferences.h
    NetworkClient.cpp NetworkClient.h)
target_include_directories(zto_desktop_share PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(zto_desktop_share PRIVATE
    ZTO::Api ZTO::MediaApi ZTO::MediaQuick Qt6::Network zds_protocol)

qt_add_resources(zto_desktop_share desktop_share_page
    PREFIX /zto/desktop-share FILES DeskSharePage.qml)

set_target_properties(zto_desktop_share PROPERTIES PREFIX "")
if(MSVC)
    target_compile_options(zto_desktop_share PRIVATE /utf-8)
endif()

zto_package_plugin(zto_desktop_share HOST client ID desktop-share NAME "桌面共享"
    VERSION "0.1.0" API "${ZTO_API_VERSION}" IID "vusiif.zto.api.PluginInterface/3.0"
    SERVICE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/service-manifest.json")

install(FILES "$<TARGET_FILE_DIR:zto_desktop_share>/$<TARGET_FILE_BASE_NAME:zto_desktop_share>.z2plugin"
    DESTINATION plugins)
