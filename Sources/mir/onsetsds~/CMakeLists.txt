cmake_minimum_required(VERSION 3.25)
project(onsetsds~)

file(GLOB ONSETSDS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

pd_add_external(onsetsds~ "${ONSETSDS_SOURCES}" LINK_LIBRARIES fftw3)
