# Copyright (C) 2018 Fondazione Istituto Italiano di Tecnologia (IIT)
# All Rights Reserved.
# Authors: Giulio Romualdi <giulio.romualdi@iit.it>
cmake_minimum_required(VERSION 3.5)
project(BerdyUnitTest CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# set target name
set(EXE_TARGET_NAME berdyUnitTest)

option(ENABLE_RPATH "Enable RPATH for this library" ON)
mark_as_advanced(ENABLE_RPATH)
#add_install_rpath_support(BIN_DIRS "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
#  LIB_DIRS "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}"
#  INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}"
#  DEPENDS ENABLE_RPATH
#  USE_LINK_PATH)

# Find required package
#find_package(ICUB REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(iDynTree REQUIRED)
include_directories(SYSTEM ${EIGEN3_INCLUDE_DIR})
include(FindPackageHandleStandardArgs)
add_subdirectory(data)

# set cpp files
set(${EXE_TARGET_NAME}_SRC
  src/BerdyHelperUnitTest.cpp
#  src/BerdyMAPSolverUnitTest.cpp
)

# set hpp files
set(${EXE_TARGET_NAME}_HDR
)

# add include directories to the build.
message(${CMAKE_CURRENT_BINARY_DIR})
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/data
  include/ModelTestUtils.h
  ${CMAKE_CURRENT_BINARY_DIR}/data/testModels.h
)

# add an executable to the project using the specified source files.
add_executable(${EXE_TARGET_NAME} ${${EXE_TARGET_NAME}_SRC} ${${EXE_TARGET_NAME}_HDR})

target_link_libraries(${EXE_TARGET_NAME} LINK_PUBLIC
  ${YARP_LIBRARIES}
  ${iDynTree_LIBRARIES}
)

install(TARGETS ${EXE_TARGET_NAME} DESTINATION bin)
