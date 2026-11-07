cmake_minimum_required(VERSION 3.28)
project(dsptb_sofa_tool LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(mysofa CONFIG REQUIRED)
find_package(netCDF CONFIG REQUIRED)
add_executable(dsptb-sofa sofa_tool.cpp)
target_link_libraries(dsptb-sofa PRIVATE netCDF::netcdf
  $<IF:$<TARGET_EXISTS:mysofa::mysofa-shared>,mysofa::mysofa-shared,mysofa::mysofa-static>)
