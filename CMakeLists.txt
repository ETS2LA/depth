cmake_minimum_required(VERSION 3.15)
project(ets2la_depth VERSION 1.0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

add_library(ets2la_depth SHARED
    src/dll/dllmain.cpp
    src/dll/depth_hook.cpp
)

target_compile_definitions(ets2la_depth PRIVATE "ETS2LA_DEPTH_VERSION=\"${PROJECT_VERSION}\"")
target_include_directories(ets2la_depth PRIVATE include)
target_link_libraries(ets2la_depth PRIVATE minhook d3d11 dxgi)
set_target_properties(ets2la_depth PROPERTIES OUTPUT_NAME "ets2la_depth")