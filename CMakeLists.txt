cmake_minimum_required(VERSION 3.0.0)
project(matting-green VERSION 0.1.0)
set(CMAKE_C_STANDARD 17)

include(CTest)
enable_testing()

add_executable(${PROJECT_NAME} main.c)
target_link_libraries(${PROJECT_NAME} PRIVATE avfilter avcodec avformat avutil)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
