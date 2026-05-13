cmake_minimum_required(VERSION 3.10)

# ── Project ────────────────────────────────────────────────────
project(mp_pool
    VERSION 1.0.0
    DESCRIPTION "A lightweight static memory pool library for embedded systems"
    LANGUAGES C
)

# ── C Standard ─────────────────────────────────────────────────
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Library ────────────────────────────────────────────────────
add_library(mp_pool
    mp_pool.c
)

target_include_directories(mp_pool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# ── Test executable ────────────────────────────────────────────
add_executable(test_mp_pool
    test_mp_pool.c
)

target_link_libraries(test_mp_pool
    PRIVATE
        mp_pool
)

# ── Test target ────────────────────────────────────────────────
add_custom_target(check
    COMMAND test_mp_pool
    DEPENDS test_mp_pool
    COMMENT "Running mp_pool test suite..."
)

# ── Compiler warnings (optional) ───────────────────────────────
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mp_pool PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
    target_compile_options(test_mp_pool PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

# ── Install rules ─────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS mp_pool
    EXPORT mp_poolTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES mp_pool.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT mp_poolTargets
    FILE mp_poolTargets.cmake
    NAMESPACE mp_pool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mp_pool
)
