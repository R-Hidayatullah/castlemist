# =============================================================================
# castlemist unit tests
#
# One executable per layer, so a failure names the layer immediately and a test
# binary only links what it is actually testing -- a `core` failure can never be
# caused by a Direct3D device that would not create.
#
#   ctest --preset debug
#   build/debug/bin/cm_test_native.exe method0   # run one group directly
#
# Tests that need a real Gw2.dat, a sound device or a GPU skip themselves rather
# than fail; see docs/testing.md.
# =============================================================================

add_library(castlemist_test_framework STATIC framework/test_main.cpp)
target_include_directories(castlemist_test_framework
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/framework")

# -----------------------------------------------------------------------------
# castlemist_add_test(<layer> SOURCES <files...> DEPS <targets...>
#                             [PRIVATE_HEADERS <dirs...>])
#
# PRIVATE_HEADERS grants access to a layer's internal headers (internal.h,
# detail/state.h, detail/app_state.h) for the tests that exercise the seams
# between a layer's own files.
# -----------------------------------------------------------------------------
function(castlemist_add_test layer)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_HEADERS" ${ARGN})
    set(_target cm_test_${layer})

    add_executable(${_target} ${ARG_SOURCES})
    target_link_libraries(${_target} PRIVATE castlemist_test_framework ${ARG_DEPS})
    castlemist_link_runtime(${_target})
    if (ARG_PRIVATE_HEADERS)
        target_include_directories(${_target} PRIVATE ${ARG_PRIVATE_HEADERS})
    endif()
    set_target_properties(${_target} PROPERTIES FOLDER "castlemist/tests")

    add_test(NAME ${layer} COMMAND ${_target})
    set_tests_properties(${layer} PROPERTIES TIMEOUT 120)
endfunction()

castlemist_add_test(native
    SOURCES test_native_method0.cpp
    DEPS    castlemist::native)

castlemist_add_test(core
    SOURCES test_core_bytes.cpp
            test_core_packfile.cpp
            test_core_text.cpp
            test_core_env.cpp
    DEPS    castlemist::core)

castlemist_add_test(format
    SOURCES test_format_dds.cpp
            test_format_chat_link.cpp
            test_format_strs.cpp
    DEPS    castlemist::format)

castlemist_add_test(db
    SOURCES test_db_index.cpp
    DEPS    castlemist::db ext::sqlite)

castlemist_add_test(media
    SOURCES test_media_audio.cpp
    DEPS    castlemist::media)

castlemist_add_test(extract
    SOURCES test_extract_classify.cpp
            test_extract_summary.cpp
    DEPS    castlemist::extract
    PRIVATE_HEADERS "${PROJECT_SOURCE_DIR}/src/extract")

# End-to-end extraction against a real Gw2.dat. Skips itself when the archive is
# absent, so it is safe to leave enabled; GW2_TEST_DAT overrides the location.
castlemist_add_test(dat
    SOURCES test_extract_dat.cpp
    DEPS    castlemist::extract)
set_tests_properties(dat PROPERTIES TIMEOUT 600)

castlemist_add_test(sim
    SOURCES test_sim_cloth.cpp
    DEPS    castlemist::sim)
