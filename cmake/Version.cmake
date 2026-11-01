get_filename_component(ZTO_REPOSITORY "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ZTO_VERSION_FILE "${ZTO_REPOSITORY}/VERSION")
set(ZTOHUB_VERSION_FILE "${ZTO_REPOSITORY}/SERVER_VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ZTO_VERSION_FILE}" "${ZTOHUB_VERSION_FILE}")
file(READ "${ZTO_VERSION_FILE}" ZTO_VERSION)
file(READ "${ZTOHUB_VERSION_FILE}" ZTOHUB_VERSION_TEXT)
string(STRIP "${ZTO_VERSION}" ZTO_VERSION)
string(STRIP "${ZTOHUB_VERSION_TEXT}" ZTOHUB_VERSION_TEXT)

# Client version: 0.<core>.<patch>[-qualifier]. The client API uses the same
# core components and the same qualifier, so the two stay in lockstep.
if(NOT ZTO_VERSION MATCHES "^0\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION must be 0.<core>.<patch>[-qualifier]")
endif()
set(ZTO_VERSION_NUMERIC "0.${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
set(ZTO_VERSION_SUFFIX "${CMAKE_MATCH_3}")
set(ZTO_API_VERSION "0.${CMAKE_MATCH_1}${CMAKE_MATCH_3}")
set(ZTO_API_VERSION_NUMERIC "0.${CMAKE_MATCH_1}")

# Server version is specified on the client side in SERVER_VERSION. The server
# build reads the same file; it is not derived from the client VERSION.
if(NOT ZTOHUB_VERSION_TEXT MATCHES "^0\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "SERVER_VERSION must be 0.<core>.<patch>[-qualifier]")
endif()
set(ZTOHUB_VERSION "${ZTOHUB_VERSION_TEXT}")
set(ZTOHUB_VERSION_NUMERIC "0.${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
set(ZTOHUB_VERSION_SUFFIX "${CMAKE_MATCH_3}")
set(ZTOHUB_API_VERSION "0.${CMAKE_MATCH_1}${CMAKE_MATCH_3}")
set(ZTOHUB_API_VERSION_NUMERIC "0.${CMAKE_MATCH_1}")
