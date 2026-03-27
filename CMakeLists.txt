cmake_minimum_required(VERSION 3.10)
project(Castlemist VERSION 0.1.0 LANGUAGES CXX)

# ----------------------
# C++ standard
# ----------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------
# Include directories
# ----------------------
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/external
)

# ----------------------
# Source files
# ----------------------
file(GLOB_RECURSE SOURCES "${PROJECT_SOURCE_DIR}/src/*.cpp")

# ----------------------
# Create executable
# ----------------------
add_executable(castlemist ${SOURCES})

# ----------------------
# Link external libraries (example: GLFW)
# ----------------------
# If using FetchContent for GLFW
# include(FetchContent)
# FetchContent_Declare(
#     glfw
#     GIT_REPOSITORY https://github.com/glfw/glfw.git
#     GIT_TAG 3.4.4
# )
# FetchContent_MakeAvailable(glfw)
# target_link_libraries(castlemist PRIVATE glfw)