cmake_minimum_required(VERSION 3.10)
project(Castlemist VERSION 0.1.0 LANGUAGES CXX)

# ----------------------
# C++ standard
# ----------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------
# Source files
# ----------------------
file(GLOB_RECURSE SOURCES "${PROJECT_SOURCE_DIR}/src/*.cpp")

# ----------------------
# Include directories
# ----------------------
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/external/glad33extcore/include
    ${PROJECT_SOURCE_DIR}/external/glfw-3.4.bin.WIN64/include
    ${PROJECT_SOURCE_DIR}/external/glm-1.0.3
    ${PROJECT_SOURCE_DIR}/external/imgui-docking
    ${PROJECT_SOURCE_DIR}/external/ImGuizmo-master
    ${PROJECT_SOURCE_DIR}/external/stb-master
    ${PROJECT_SOURCE_DIR}/external/half-rocm-7.2.1/include
    ${PROJECT_SOURCE_DIR}/external/glad33extcore/include
)

# ----------------------
# Add executable
# ----------------------
add_executable(castlemist 
    ${SOURCES}
    ${PROJECT_SOURCE_DIR}/external/glad33extcore/src/glad.c
    )

# ----------------------
# GLFW (MinGW static linking)
# ----------------------
target_link_libraries(castlemist PRIVATE
    "${PROJECT_SOURCE_DIR}/external/glfw-3.4.bin.WIN64/lib-mingw-w64/libglfw3.a"
    -lgdi32
    -lopengl32
    -luser32
    -lkernel32
)


# ----------------------
# ImGui
# ----------------------
file(GLOB IMGUI_SOURCES
    "${PROJECT_SOURCE_DIR}/external/imgui-docking/*.cpp"
    "${PROJECT_SOURCE_DIR}/external/imgui-docking/backends/imgui_impl_glfw.cpp"
    "${PROJECT_SOURCE_DIR}/external/imgui-docking/backends/imgui_impl_opengl3.cpp"
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    ${PROJECT_SOURCE_DIR}/external/imgui-docking
    ${PROJECT_SOURCE_DIR}/external/imgui-docking/backends
)
target_link_libraries(castlemist PRIVATE imgui)

# ----------------------
# ImGuizmo
# ----------------------
file(GLOB IMGUZMO_SOURCES
    "${PROJECT_SOURCE_DIR}/external/ImGuizmo-master/*.cpp"
)
add_library(ImGuizmo STATIC ${IMGUZMO_SOURCES})
target_include_directories(ImGuizmo PUBLIC
    ${PROJECT_SOURCE_DIR}/external/ImGuizmo-master
)
target_link_libraries(castlemist PRIVATE ImGuizmo)

# ----------------------
# Header-only libraries
# ----------------------
target_include_directories(castlemist PRIVATE
    ${PROJECT_SOURCE_DIR}/external/stb-master
    ${PROJECT_SOURCE_DIR}/external/glm-1.0.3
    ${PROJECT_SOURCE_DIR}/external/half-rocm-7.2.1/include
)

# ----------------------
# Disable warnings on Windows
# ----------------------
target_compile_definitions(castlemist PRIVATE _CRT_SECURE_NO_WARNINGS)