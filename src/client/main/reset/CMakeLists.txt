# Reset local ZeroTier state feature module. The parent CMakeLists.txt prepends
# its source directory to these relative entries, so host sources stay next to
# this feature and do not collide with concurrent edits in the parent list.
list(APPEND ZTO_MAIN_SOURCES
    reset/ResetPlan.cpp
    reset/ResetScript.cpp
    reset/ResetController.cpp)
list(APPEND ZTO_MAIN_HEADERS
    reset/ResetPlan.h
    reset/ResetScript.h
    reset/ResetController.h)
set(ZTO_MAIN_SOURCES "${ZTO_MAIN_SOURCES}" PARENT_SCOPE)
set(ZTO_MAIN_HEADERS "${ZTO_MAIN_HEADERS}" PARENT_SCOPE)
