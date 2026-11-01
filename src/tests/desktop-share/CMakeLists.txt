cmake_minimum_required(VERSION 3.21.1)
project(zds_protocol_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(ZTOrbitPluginApi 0.5 EXACT CONFIG REQUIRED)
find_package(Qt6 6.8 REQUIRED COMPONENTS Test)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/desktop-share/shared"
    "${CMAKE_CURRENT_BINARY_DIR}/shared")

add_executable(desktop_share_protocol_tests ProtocolGoldenTests.cpp)
target_link_libraries(desktop_share_protocol_tests PRIVATE zds_protocol Qt6::Test)
target_compile_definitions(desktop_share_protocol_tests PRIVATE
    ZDS_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/desktop-share/shared/samples")
if(MSVC)
    target_compile_options(desktop_share_protocol_tests PRIVATE /utf-8)
endif()

add_executable(desktop_share_stream_tests StreamLogicTests.cpp)
target_link_libraries(desktop_share_stream_tests PRIVATE zds_protocol Qt6::Test)
if(MSVC)
    target_compile_options(desktop_share_stream_tests PRIVATE /utf-8)
endif()

add_executable(desktop_share_stage5_congestion_tests Stage5CongestionTests.cpp)
target_link_libraries(desktop_share_stage5_congestion_tests PRIVATE zds_protocol Qt6::Test)
if(MSVC)
    target_compile_options(desktop_share_stage5_congestion_tests PRIVATE /utf-8)
endif()

# Stage-5 R1/R2 follow-up: the real NetworkClient publisher pipeline (whole-packet
# admission, send deadline, dependency invalidation) against a recording UDP host.
add_executable(desktop_share_stage5_followup_tests Stage5FollowupNetworkTests.cpp
    ../../plugins/desktop-share/client/NetworkClient.cpp
    ../../plugins/desktop-share/client/NetworkClient.h)
target_include_directories(desktop_share_stage5_followup_tests PRIVATE
    ../../plugins/desktop-share/client)
target_link_libraries(desktop_share_stage5_followup_tests PRIVATE
    zds_protocol Qt6::Test ZTO::Api)
if(MSVC)
    target_compile_options(desktop_share_stage5_followup_tests PRIVATE /utf-8)
endif()

add_executable(desktop_share_stage5_remediation_tests Stage5RemediationTests.cpp)
target_link_libraries(desktop_share_stage5_remediation_tests PRIVATE zds_protocol Qt6::Test)
if(MSVC)
    target_compile_options(desktop_share_stage5_remediation_tests PRIVATE /utf-8)
endif()

# Stage-5 remediation round R4/R1-residual/R6: the real publisher pipeline against
# a recording UDP host (reservation/refund byte conservation, deferred admission
# age and preemption, bounded GUI notification board).
add_executable(desktop_share_stage5_remediation_network_tests Stage5RemediationNetworkTests.cpp
    ../../plugins/desktop-share/client/NetworkClient.cpp
    ../../plugins/desktop-share/client/NetworkClient.h)
target_include_directories(desktop_share_stage5_remediation_network_tests PRIVATE
    ../../plugins/desktop-share/client)
target_link_libraries(desktop_share_stage5_remediation_network_tests PRIVATE
    zds_protocol Qt6::Test ZTO::Api)
if(MSVC)
    target_compile_options(desktop_share_stage5_remediation_network_tests PRIVATE /utf-8)
endif()

enable_testing()
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME desktop_share_rss_analysis
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/validation/test_rss.py")
    add_test(NAME desktop_share_validation_analyzers
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/validation/test_analyzers.py")
    add_test(NAME desktop_share_stop_proof
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/validation/test_stop_proof.py")
endif()
add_test(NAME desktop_share_protocol COMMAND desktop_share_protocol_tests)
add_test(NAME desktop_share_stream COMMAND desktop_share_stream_tests)
add_test(NAME desktop_share_stage5_congestion COMMAND desktop_share_stage5_congestion_tests)
add_test(NAME desktop_share_stage5_followup COMMAND desktop_share_stage5_followup_tests)
add_test(NAME desktop_share_stage5_remediation COMMAND desktop_share_stage5_remediation_tests)
add_test(NAME desktop_share_stage5_remediation_network
    COMMAND desktop_share_stage5_remediation_network_tests)

find_package(ZTOrbitMediaApi 2 CONFIG REQUIRED COMPONENTS Quick)
    add_executable(desktop_share_stage4_regression_tests Stage4RegressionTests.cpp
        ../../plugins/desktop-share/client/NetworkClient.cpp
        ../../plugins/desktop-share/client/NetworkClient.h
        ../../plugins/desktop-share/client/DeskShareController.cpp
        ../../plugins/desktop-share/client/DeskShareController.h
        ../../plugins/desktop-share/client/Preferences.cpp
        ../../plugins/desktop-share/client/Preferences.h)
    target_include_directories(desktop_share_stage4_regression_tests PRIVATE
        ../../plugins/desktop-share/client)
    target_link_libraries(desktop_share_stage4_regression_tests PRIVATE
        zds_protocol Qt6::Test ZTO::Api ZTO::MediaApi ZTO::MediaQuick)
    if(MSVC)
        target_compile_options(desktop_share_stage4_regression_tests PRIVATE /utf-8)
    endif()
    add_test(NAME desktop_share_stage4_regression COMMAND desktop_share_stage4_regression_tests)

# Stage-6 page/lifecycle controller contract: explicit operations, cancellation,
# stop ordering, stale results and field-merging preferences.
add_executable(desktop_share_stage6_lifecycle_tests Stage6LifecycleTests.cpp
    ../../plugins/desktop-share/client/NetworkClient.cpp
    ../../plugins/desktop-share/client/NetworkClient.h
    ../../plugins/desktop-share/client/DeskShareController.cpp
    ../../plugins/desktop-share/client/DeskShareController.h
    ../../plugins/desktop-share/client/Preferences.cpp
    ../../plugins/desktop-share/client/Preferences.h)
target_include_directories(desktop_share_stage6_lifecycle_tests PRIVATE
    ../../plugins/desktop-share/client)
target_link_libraries(desktop_share_stage6_lifecycle_tests PRIVATE
    zds_protocol Qt6::Test ZTO::Api ZTO::MediaApi ZTO::MediaQuick)
if(MSVC)
    target_compile_options(desktop_share_stage6_lifecycle_tests PRIVATE /utf-8)
endif()
add_test(NAME desktop_share_stage6_lifecycle COMMAND desktop_share_stage6_lifecycle_tests)

# Stage-6 remediation R1-R4: removal-result authority, stale stop completion,
# conflicting-operation blocking and dirty-field preference merge.
add_executable(desktop_share_stage6_remediation_tests Stage6RemediationTests.cpp
    ../../plugins/desktop-share/client/NetworkClient.cpp
    ../../plugins/desktop-share/client/NetworkClient.h
    ../../plugins/desktop-share/client/DeskShareController.cpp
    ../../plugins/desktop-share/client/DeskShareController.h
    ../../plugins/desktop-share/client/Preferences.cpp
    ../../plugins/desktop-share/client/Preferences.h)
target_include_directories(desktop_share_stage6_remediation_tests PRIVATE
    ../../plugins/desktop-share/client)
target_link_libraries(desktop_share_stage6_remediation_tests PRIVATE
    zds_protocol Qt6::Test ZTO::Api ZTO::MediaApi ZTO::MediaQuick)
if(MSVC)
    target_compile_options(desktop_share_stage6_remediation_tests PRIVATE /utf-8)
endif()
add_test(NAME desktop_share_stage6_remediation COMMAND desktop_share_stage6_remediation_tests)
