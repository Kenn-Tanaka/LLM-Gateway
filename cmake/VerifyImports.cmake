# VerifyImports.cmake
# Usage: cmake -DTARGET_FILE=<path-to-binary> -P cmake/VerifyImports.cmake

if(NOT DEFINED TARGET_FILE)
    message(FATAL_ERROR "TARGET_FILE is not defined.")
endif()

if(NOT EXISTS "${TARGET_FILE}")
    message(FATAL_ERROR "Target file does not exist: ${TARGET_FILE}")
endif()

find_program(DUMPBIN_EXECUTABLE dumpbin)
if(NOT DUMPBIN_EXECUTABLE)
    # Search common VS installation path if dumpbin not in PATH
    file(GLOB_RECURSE DUMPBIN_CANDIDATES "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe")
    if(DUMPBIN_CANDIDATES)
        list(GET DUMPBIN_CANDIDATES 0 DUMPBIN_EXECUTABLE)
    else()
        message(FATAL_ERROR "dumpbin.exe not found.")
    endif()
endif()

message(STATUS "Using dumpbin: ${DUMPBIN_EXECUTABLE}")
message(STATUS "Verifying imports and dependencies for: ${TARGET_FILE}")

# Run dumpbin /DEPENDENTS
execute_process(
    COMMAND "${DUMPBIN_EXECUTABLE}" /DEPENDENTS "${TARGET_FILE}"
    OUTPUT_VARIABLE DUMPBIN_DEP_OUT
    RESULT_VARIABLE DUMPBIN_DEP_RES
    ERROR_QUIET
)

if(NOT DUMPBIN_DEP_RES EQUAL 0)
    message(FATAL_ERROR "Failed to run dumpbin /DEPENDENTS on ${TARGET_FILE}")
endif()

# Run dumpbin /IMPORTS
execute_process(
    COMMAND "${DUMPBIN_EXECUTABLE}" /IMPORTS "${TARGET_FILE}"
    OUTPUT_VARIABLE DUMPBIN_IMP_OUT
    RESULT_VARIABLE DUMPBIN_IMP_RES
    ERROR_QUIET
)

if(NOT DUMPBIN_IMP_RES EQUAL 0)
    message(FATAL_ERROR "Failed to run dumpbin /IMPORTS on ${TARGET_FILE}")
endif()

# Forbidden pattern: api-ms-win-crt-.*|MSVCP.*|VCRUNTIME.*|VCOMP.*|wx.*|libcrypto.*|libssl.*
set(FORBIDDEN_REGEX "(api-ms-win-crt-.*|msvcp.*|vcruntime.*|vcomp.*|wx.*|libcrypto.*|libssl.*)")

string(TOLOWER "${DUMPBIN_DEP_OUT}" DEP_LOWER)
string(TOLOWER "${DUMPBIN_IMP_OUT}" IMP_LOWER)

string(REGEX MATCHALL "${FORBIDDEN_REGEX}" MATCHED_DEP "${DEP_LOWER}")
string(REGEX MATCHALL "${FORBIDDEN_REGEX}" MATCHED_IMP "${IMP_LOWER}")

if(MATCHED_DEP OR MATCHED_IMP)
    message(FATAL_ERROR "Security violation: Forbidden dependencies/imports detected in ${TARGET_FILE}:\nDependents matches: ${MATCHED_DEP}\nImports matches: ${MATCHED_IMP}")
endif()

# Allowed system DLLs (case-insensitive)
set(ALLOWED_DLLS
    "kernel32.dll"
    "user32.dll"
    "advapi32.dll"
    "ws2_32.dll"
    "bcrypt.dll"
    "crypt32.dll"
    "secur32.dll"
    "shell32.dll"
    "comctl32.dll"
    "gdi32.dll"
    "ole32.dll"
    "oleaut32.dll"
    "comdlg32.dll"
    "uxtheme.dll"
    "shlwapi.dll"
    "winmm.dll"
    "version.dll"
    "rpcrt4.dll"
    "oleacc.dll"
    "msimg32.dll"
)

# Parse DEPENDENTS output for all DLL names
string(REGEX MATCHALL "([a-zA-Z0-9._-]+\\.[dD][lL][lL])" FOUND_DLLS "${DUMPBIN_DEP_OUT}")
list(REMOVE_DUPLICATES FOUND_DLLS)

foreach(DLL ${FOUND_DLLS})
    string(TOLOWER "${DLL}" DLL_LOWER)
    list(FIND ALLOWED_DLLS "${DLL_LOWER}" DLL_INDEX)
    if(DLL_INDEX EQUAL -1)
        message(FATAL_ERROR "Disallowed dependency found in ${TARGET_FILE}: ${DLL}")
    else()
        message(STATUS "Verified allowed dependency: ${DLL}")
    endif()
endforeach()

message(STATUS "Verification PASSED for ${TARGET_FILE}")
