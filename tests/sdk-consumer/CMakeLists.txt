cmake_minimum_required(VERSION 3.21.1)
project(ztgui_sdk_consumer LANGUAGES CXX)
# Configure this outside the host build to verify the installed SDK boundary.
set(ZTGUI_REQUIRED_API "0.3" CACHE STRING "Required plugin API generation")
find_package(ZTGUIPluginApi ${ZTGUI_REQUIRED_API} EXACT CONFIG REQUIRED)
add_executable(sdk_consumer ApiConsumer.cpp)
target_link_libraries(sdk_consumer PRIVATE ZTGUI::Api)
