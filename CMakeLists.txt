cmake_minimum_required (VERSION 2.8.4)

project (MPMCRB)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src DIR_LIB_SRCS)

add_library(MPMCRB ${DIR_LIB_SRCS})
