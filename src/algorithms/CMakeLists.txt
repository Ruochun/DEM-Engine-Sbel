# Copyright (c) 2021, SBEL GPU Development Team
# Copyright (c) 2021, University of Wisconsin - Madison
# 
#	SPDX-License-Identifier: BSD-3-Clause

message(STATUS "==== Configuring GPU Algorithm Components ====")

set (algorithms_message "[algorithms]")



add_library(algorithms OBJECT)

target_include_directories(
	algorithms	
	PUBLIC ${ProjectIncludeSource}
	PUBLIC ${ProjectIncludeGenerated}
	PUBLIC ${NVIDIAMathDir}
)

target_link_libraries(
	algorithms
	PUBLIC CUB::CUB
)

### HOST HEADERS ONLY (.h, .hpp) ###
set(algorithms_interface
	${CMAKE_CURRENT_SOURCE_DIR}/DEMCubBasedSubroutines.h
)

### INTERNAL HEADERS ONLY (.h, .hpp, or .cuh) ###
set(algorithms_headers
	${NVIDIAMathDir}/nvmath/helper_math.cuh
)

set(algorithms_sources
	${CMAKE_CURRENT_SOURCE_DIR}/DEMCubWrappers.cu 
	${CMAKE_CURRENT_SOURCE_DIR}/DEMCubForceCollection.cu
	${CMAKE_CURRENT_SOURCE_DIR}/DEMCubUtilities.cu
	${CMAKE_CURRENT_SOURCE_DIR}/DEMCubContactDetection.cu
)

target_sources(
	algorithms
	PUBLIC ${algorithms_interface}
	INTERFACE ${algorithms_headers}
	PRIVATE ${algorithms_sources}
)

# Get a list of supported architectures
include(${CMAKE_SOURCE_DIR}/cmake/CudaSupportedArchitectures.cmake)
cuda_supported_architectures()

# Convert the cuda supported architectures into their respective CMake properties
set(virtual_architectures "")
foreach(arch ${CUDASUP_ARCHITECTURES})
	list(APPEND virtual_architectures "${arch}-virtual")
endforeach()

set_target_properties(
	algorithms PROPERTIES 
	POSITION_INDEPENDENT_CODE True
	CXX_STANDARD ${CXXSTD_SUPPORTED}
	PUBLIC_HEADER "${algorithms_interface}"
	CUDA_ARCHITECTURES "${virtual_architectures}"
)

install(
	TARGETS algorithms
	PUBLIC_HEADER
		DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/algorithms
)
