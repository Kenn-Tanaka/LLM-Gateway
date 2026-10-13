cmake_minimum_required(VERSION 3.25)
project(llm_gateway LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/Zc:char8_t-)
endif()

# Enforce static runtime and MSVC compiler/linker flags
include(cmake/StaticRuntime.cmake)

# Prevent external tools/Strawberry Perl from polluting dependencies
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Libidn2 ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Libssh2 ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Libpsl ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_NGHTTP2 ON CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB ON CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CMAKE_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)

# Dependencies
include(cmake/Dependencies.cmake)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 1. Gateway Core Library
add_library(gateway_core STATIC
    src/gateway/config.cpp
    src/gateway/credential_store.cpp
    src/gateway/logger.cpp
    src/gateway/request_guard.cpp
    src/gateway/auth.cpp
    src/gateway/model_router.cpp
    src/gateway/model_catalog.cpp
    src/gateway/external_proxy.cpp
    src/gateway/embedding_engine.cpp
    src/gateway/rerank_engine.cpp
    src/gateway/embedded_engine.cpp
    src/gateway/http_server.cpp
    src/gateway/app.cpp
)

target_include_directories(gateway_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(gateway_core PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
    libcurl
    llama
    ws2_32
    crypt32
    advapi32
    bcrypt
)

if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(gateway_core)
endif()

# 2. Main Gateway Executable: llm-gateway.exe
add_executable(llm-gateway
    src/gateway/main.cpp
)

set_target_properties(llm-gateway PROPERTIES
    OUTPUT_NAME "llm-gateway"
)

target_link_libraries(llm-gateway PRIVATE
    gateway_core
)

if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(llm-gateway)
endif()

# 3. Maintenance Core Library
add_library(maintenance_core STATIC
    src/maintenance/config_store.cpp
    src/maintenance/credential_editor.cpp
    src/maintenance/health_client.cpp
    src/maintenance/process_controller.cpp
    src/maintenance/log_tail_reader.cpp
    src/maintenance/main_frame.cpp
)

target_include_directories(maintenance_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(maintenance_core PUBLIC
    gateway_core
    wx::core
    wx::base
    shell32
    comctl32
    uxtheme
)

if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(maintenance_core)
endif()

# 4. Maintenance Tool Executable: MaintenanceTool.exe
add_executable(MaintenanceTool WIN32
    src/maintenance/main.cpp
)

set_target_properties(MaintenanceTool PROPERTIES
    OUTPUT_NAME "MaintenanceTool"
)

target_link_libraries(MaintenanceTool PRIVATE
    maintenance_core
)

if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(MaintenanceTool)
endif()

# 5. Tests
enable_testing()

add_executable(config_test tests/unit/config_test.cpp)
target_link_libraries(config_test PRIVATE gateway_core)
if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(config_test)
endif()
add_test(NAME config_test COMMAND config_test)

add_executable(router_test tests/unit/router_test.cpp)
target_link_libraries(router_test PRIVATE gateway_core)
if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(router_test)
endif()
add_test(NAME router_test COMMAND router_test)

add_executable(auth_test tests/unit/auth_test.cpp)
target_link_libraries(auth_test PRIVATE gateway_core)
if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(auth_test)
endif()
add_test(NAME auth_test COMMAND auth_test)

add_executable(request_guard_test tests/unit/request_guard_test.cpp)
target_link_libraries(request_guard_test PRIVATE gateway_core)
if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(request_guard_test)
endif()
add_test(NAME request_guard_test COMMAND request_guard_test)

add_executable(health_test tests/integration/health_test.cpp)
target_link_libraries(health_test PRIVATE gateway_core maintenance_core)
if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(health_test)
endif()
add_test(NAME health_test COMMAND health_test)

add_executable(security_test tests/security/security_test.cpp)
target_link_libraries(security_test PRIVATE gateway_core)
if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(security_test)
endif()
add_test(NAME security_test COMMAND security_test)

add_executable(legacy_compat_test tests/integration/legacy_compat_test.cpp)
target_link_libraries(legacy_compat_test PRIVATE gateway_core)
if(COMMAND apply_gateway_target_flags)
    apply_gateway_target_flags(legacy_compat_test)
endif()
add_test(NAME legacy_compat_test COMMAND legacy_compat_test)
set_tests_properties(legacy_compat_test PROPERTIES TIMEOUT 30)

# These tests use assert: keep their checks active in Release as well.
foreach(test_target config_test router_test auth_test request_guard_test health_test security_test)
    if(MSVC)
        target_compile_options(${test_target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${test_target} PRIVATE -UNDEBUG)
    endif()
endforeach()

# 6. Verify Imports Custom Target
add_custom_target(verify_imports
    COMMAND ${CMAKE_COMMAND} -DTARGET_FILE=$<TARGET_FILE:llm-gateway> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyImports.cmake
    COMMAND ${CMAKE_COMMAND} -DTARGET_FILE=$<TARGET_FILE:MaintenanceTool> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyImports.cmake
    DEPENDS llm-gateway MaintenanceTool
)
