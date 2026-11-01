if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21.1)
    project(zds_protocol LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core)

add_library(zds_protocol STATIC
    src/JsonSupport.cpp
    src/JsonSupport.h
    src/ControlProtocol.cpp
    src/MediaProtocol.cpp
    src/PacketReassembler.cpp
    src/StreamReceiver.cpp
)
target_include_directories(zds_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The ZTS1 outer layer comes from the installed public SDK. A standalone test
# build must call find_package(ZTOrbitPluginApi ...) first.
if(NOT TARGET ZTO::Protocol)
    message(FATAL_ERROR "zds_protocol requires the public ZTO::Protocol target (find_package(ZTOrbitPluginApi ...))")
endif()
target_link_libraries(zds_protocol PUBLIC Qt6::Core ZTO::Protocol)
set_target_properties(zds_protocol PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
    target_compile_options(zds_protocol PRIVATE /utf-8)
endif()
