include_guard(GLOBAL)
function(ztmedia_write_extension_bundle output target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "Unknown extension target: ${target}")
    endif()
    file(GENERATE OUTPUT "${output}" CONTENT
        "[{\"source\":\"$<TARGET_FILE:${target}>\",\"path\":\"extensions/${target}/$<TARGET_FILE_NAME:${target}>\"}]\n")
endfunction()
