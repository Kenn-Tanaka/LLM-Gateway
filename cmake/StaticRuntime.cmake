# StaticRuntime.cmake
# Enforces MSVC static multithreaded runtime (/MT or /MTd) and security compiler flags.

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

    # Function to apply strict gateway compiler/linker flags to target
    function(apply_gateway_target_flags target)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /EHsc
            /utf-8
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/guard:cf>
        )

        target_link_options(${target} PRIVATE
            /DYNAMICBASE
            /NXCOMPAT
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/guard:cf>
        )
    endfunction()
endif()
