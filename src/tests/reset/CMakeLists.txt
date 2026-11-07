# Isolated tests for the reset feature. They compile the pure host sources
# directly, like local_config_tests, so they can run without building the GUI.
set(ZTO_RESET_SHARED_SOURCES
    "${ZTO_ROOT_DIR}/src/client/main/reset/ResetPlan.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/reset/ResetScript.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/LocalConfig.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/MacPlatform.cpp")

add_executable(reset_plan_tests ResetPlanTests.cpp ${ZTO_RESET_SHARED_SOURCES})
target_include_directories(reset_plan_tests PRIVATE
    "${ZTO_ROOT_DIR}/src/client/main"
    "${ZTO_ROOT_DIR}/src/client/main/reset")
target_compile_definitions(reset_plan_tests PRIVATE
    ZTO_TESTING
    ZTO_VERSION="${ZTO_VERSION}")
target_link_libraries(reset_plan_tests PRIVATE Qt6::Test Qt6::Core Qt6::Network)
if(MSVC)
    target_compile_options(reset_plan_tests PRIVATE /utf-8)
endif()
add_test(NAME reset_plan COMMAND reset_plan_tests)
set_tests_properties(reset_plan PROPERTIES TIMEOUT 120)

add_executable(reset_controller_tests ResetControllerTests.cpp
    "${ZTO_ROOT_DIR}/src/client/main/reset/ResetController.cpp"
    ${ZTO_RESET_SHARED_SOURCES}
    "${ZTO_ROOT_DIR}/src/client/main/SystemOperations.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/ZeroTierClient.cpp"
    "${ZTO_ROOT_DIR}/src/client/main/AppLog.cpp")
target_include_directories(reset_controller_tests PRIVATE
    "${ZTO_ROOT_DIR}/src/client/main"
    "${ZTO_ROOT_DIR}/src/client/main/reset")
target_compile_definitions(reset_controller_tests PRIVATE
    ZTO_TESTING
    ZTO_VERSION="${ZTO_VERSION}")
target_link_libraries(reset_controller_tests PRIVATE
    Qt6::Test Qt6::Core Qt6::Network Qt6::Concurrent)
if(WIN32)
    target_link_libraries(reset_controller_tests PRIVATE shell32 advapi32)
endif()
if(MSVC)
    target_compile_options(reset_controller_tests PRIVATE /utf-8)
    target_compile_definitions(reset_controller_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_test(NAME reset_controller COMMAND reset_controller_tests)
set_tests_properties(reset_controller PROPERTIES TIMEOUT 120)
