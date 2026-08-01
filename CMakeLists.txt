cmake_minimum_required(VERSION 3.20)

project(color_detection_engineering_poc VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(detector_core detector_core.cpp)
target_include_directories(detector_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(color_detector main.cpp screen_capture.cpp)
# d3d11/dxgi back the Desktop Duplication capture path; dxguid supplies the
# interface GUIDs that MinGW does not emit inline.
target_link_libraries(color_detector PRIVATE detector_core user32 gdi32 d3d11 dxgi dxguid)
target_compile_definitions(color_detector PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_executable(color_target_demo WIN32 target_demo.cpp)
target_link_libraries(color_target_demo PRIVATE user32 gdi32)
target_compile_definitions(color_target_demo PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_executable(detector_core_tests tests/detector_core_tests.cpp)
target_link_libraries(detector_core_tests PRIVATE detector_core)

if(MSVC)
    target_compile_options(detector_core PRIVATE /W4 /permissive-)
    target_compile_options(color_detector PRIVATE /W4 /permissive-)
    target_compile_options(color_target_demo PRIVATE /W4 /permissive-)
    target_compile_options(detector_core_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(detector_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(color_detector PRIVATE -Wall -Wextra)
    target_compile_options(color_target_demo PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(detector_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME detector_core_tests COMMAND detector_core_tests)
endif()
