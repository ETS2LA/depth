cmake_minimum_required(VERSION 3.20)
project(depth_reader_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(depth_reader_example main.cpp)
target_include_directories(depth_reader_example PRIVATE ${OpenCV_INCLUDE_DIRS} ../../include)
target_link_libraries(depth_reader_example PRIVATE ${OpenCV_LIBS})