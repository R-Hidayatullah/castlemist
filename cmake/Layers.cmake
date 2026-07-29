# =============================================================================
# Layers.cmake -- how castlemist declares one layer of the stack.
#
# castlemist is a stack of small libraries, one per src/<layer>/ directory.
# Dependencies point downward only, and the include path spells out which layer
# you took a dependency on: a header is always included as
# "castlemist/<layer>/<file>.h".
#
# Each layer is built either as a DLL or as a static library, chosen once by
# CASTLEMIST_SHARED_LAYERS:
#
#   ON  (default for Debug / RelWithDebInfo)
#       One DLL per layer. Touching src/ui/ relinks ui.dll and nothing else,
#       which is the difference between a two-second and a thirty-second
#       iteration. Symbols are exported wholesale via WINDOWS_EXPORT_ALL_SYMBOLS,
#       so no __declspec(dllexport) annotations are needed anywhere in the code.
#
#   OFF (default for Release / MinSizeRel)
#       Everything is archived into one self-contained castlemist.exe with a
#       statically linked libstdc++/libgcc -- nothing to ship alongside it.
# =============================================================================

if (CASTLEMIST_SHARED_LAYERS)
    set(CASTLEMIST_LAYER_KIND SHARED)
else()
    set(CASTLEMIST_LAYER_KIND STATIC)
endif()

# -----------------------------------------------------------------------------
# castlemist_add_layer(<name> [DEPS <targets...>])
#
# Declares src/<name>/*.c(pp) as the layer `castlemist::<name>`.
#   PUBLIC  include/        -- so dependents can include castlemist/<name>/*.h
#   PRIVATE src/<name>/     -- so the layer can include its own "internal.h" or
#                              "detail/state.h" without leaking them upward
# -----------------------------------------------------------------------------
function(castlemist_add_layer name)
    cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})

    file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/${name}/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/${name}/*.c")
    if (NOT _srcs)
        message(FATAL_ERROR "layer '${name}' has no sources under src/${name}")
    endif()

    add_library(castlemist_${name} ${CASTLEMIST_LAYER_KIND} ${_srcs})
    target_include_directories(castlemist_${name}
        PUBLIC  "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/src/${name}")
    target_link_libraries(castlemist_${name} PUBLIC ${ARG_DEPS})
    set_target_properties(castlemist_${name} PROPERTIES
        OUTPUT_NAME               "castlemist_${name}"
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        FOLDER                    "castlemist")
    add_library(castlemist::${name} ALIAS castlemist_${name})
endfunction()

# -----------------------------------------------------------------------------
# castlemist_link_runtime(<target>)
#
# MinGW's libstdc++/libgcc/libwinpthread are not on a stock Windows PATH, and
# picking up a mismatched copy from some other toolchain kills the process at
# startup with 0xC0000139 (STATUS_ENTRYPOINT_NOT_FOUND) before a window ever
# appears. Two ways out, and the layer kind decides which:
#
#   static layers  -- link the runtime into the exe (-static ...)
#   shared layers  -- copy this compiler's own runtime DLLs next to the exe
# -----------------------------------------------------------------------------
function(castlemist_link_runtime target)
    if (NOT MINGW)
        return()
    endif()

    if (NOT CASTLEMIST_SHARED_LAYERS)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
        return()
    endif()

    get_filename_component(_bindir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_dll libstdc++-6 libgcc_s_seh-1 libwinpthread-1)
        if (EXISTS "${_bindir}/${_dll}.dll")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_bindir}/${_dll}.dll" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "runtime: ${_dll}.dll")
        endif()
    endforeach()
endfunction()
