# Copyright (c) 2021, SBEL GPU Development Team
# Copyright (c) 2021, University of Wisconsin - Madison
# 
#	SPDX-License-Identifier: BSD-3-Clause

# ---------------------------------------------------------------------------- #
# CMake Project Settings
# ---------------------------------------------------------------------------- #

cmake_minimum_required(VERSION 3.18)

# Version information
set(VERSION_MAJOR   0)
set(VERSION_MINOR   0)
set(VERSION_PATCH   0)

project(
	Simulator-MUlti-Gpu 
	VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
	LANGUAGES CXX CUDA
)

# ---------------------------------------------------------------------------- #
# Additional Packages
# ---------------------------------------------------------------------------- #

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

include(cmake/FixNinjaColors.cmake)
fix_ninja_colors()


find_package(CUDAToolkit REQUIRED)

# Find CUB library (this might need to be done in source-level config)
find_package(
	CUB REQUIRED
	#PATHS ${CMAKE_CURRENT_LIST_DIR}/thirdparty/cub/cub
)

# Find NVIDIA's Jitify library 
find_path(
	NVIDIAJitifyPath
	NAMES jitify.hpp
	PATHS "${CMAKE_CURRENT_LIST_DIR}/thirdparty/jitify"
)

# nvidia_helper_math doesn't require any extra configuration, so just set the path 
set(NVIDIAMathDir ${CMAKE_CURRENT_LIST_DIR}/thirdparty/nvidia_helper_math)

# Find ChPF, else fetch it 
find_package(ChPF 3.0 QUIET)

if (NOT ChPF_FOUND) 		
	FetchContent_Declare(
		ChPF
		GIT_REPOSITORY	https://gitlab.com/uwsbel/chpf.git
		GIT_TAG			0699f01e720cecf58719a1651ab797c6a62a1897
	)
	FetchContent_MakeAvailable(ChPF)

endif()

# Based on whether ChPF was found or fetched, set the target name CMake can use to find it 
if (ChPF_FOUND) 
	set(ChPF_IMPORTED_NAME "ChPF::ChPF")
elseif (chpf_POPULATED)
	set(ChPF_IMPORTED_NAME "ChPF")
endif()


# ---------------------------------------------------------------------------- #
# Global Configuration
# ---------------------------------------------------------------------------- #

include(cmake/CxxStdAutodetect.cmake)

# The compiler will build against this C++ standard, if available.
set(TargetCXXStandard "STD_AUTODETECT" CACHE STRING "The C++ standard used by the compiler")
set_property(
	CACHE TargetCXXStandard
	PROPERTY
	STRINGS STD_AUTODETECT STD_CXX11 STD_CXX14 STD_CXX17 STD_CXX20
)

# Convert the standard into something CMake will understand
if(TargetCXXStandard STREQUAL STD_CXX11)
	set(CXXSTD_MAX 11)
elseif(TargetCXXStandard STREQUAL STD_CXX14)
	set(CXXSTD_MAX 14)
elseif(TargetCXXStandard STREQUAL STD_CXX17)
	set(CXXSTD_MAX 17)
elseif(TargetCXXStandard STREQUAL STD_CXX20)
	set(CXXSTD_MAX 20)
else()
	set(CXXSTD_MAX 20)
endif()
cxx_std_autodetect()

# Allow the use of #include <> for project headers (allowing cleaner better relative pathing)
set(ProjectIncludeSource "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(ProjectIncludeGenerated "${CMAKE_BINARY_DIR}/src")

# Global fix for CUDA language bug
include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})

# ---------------------------------------------------------------------------- #
# Source-level configuration
# ---------------------------------------------------------------------------- #

add_subdirectory(src/core)
add_subdirectory(src/DEM)
add_subdirectory(src/algorithms)

# ---------------------------------------------------------------------------- #
# Final Library Generation
# ---------------------------------------------------------------------------- #

add_library(
	simulator_multi_gpu 
	STATIC
	$<TARGET_OBJECTS:core>
	$<TARGET_OBJECTS:DEM>
	$<TARGET_OBJECTS:algorithms>
)

# Extract include directories from the object bundles
get_target_property(CORE_INTERFACE core INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(DEM_INTERFACE DEM INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(ALGORITHMS_INTERFACE algorithms INTERFACE_INCLUDE_DIRECTORIES)

# All targets use the same include directories, so we can cheat and just export one of them
target_include_directories(simulator_multi_gpu 
	PUBLIC ${CORE_INTERFACE}
)

target_link_libraries(simulator_multi_gpu
	PUBLIC CUDA::cudart
	PUBLIC CUDA::nvrtc
	PUBLIC ${ChPF_IMPORTED_NAME}
)

# Attach include directories to the top-level library target
set_target_properties(simulator_multi_gpu
	PROPERTIES
	LINKER_LANGUAGE CUDA
)

# ---------------------------------------------------------------------------- #
# Export and Install The Generated Targets 
# ---------------------------------------------------------------------------- #

set(config_directory ${CMAKE_INSTALL_LIBDIR}/cmake/DEME)

# Install the library, creating an export target for it as well 
install(
	TARGETS simulator_multi_gpu EXPORT DEMETargets
	LIBRARY
		DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Export the generated library target
export(EXPORT DEMETargets
	FILE "${CMAKE_CURRENT_BINARY_DIR}/DEME/DEMETargets.cmake"
	NAMESPACE DEME::
)

# Set the install location for the exported target file 
install(
	EXPORT DEMETargets
	DESTINATION 
		${config_directory}
	NAMESPACE DEME::
)


# These macros aid in the generation of the installable config files 


# Generate the **Build Tree** version of DEMEConfig.cmake from the base file
set(CONF_INCLUDE_DIRS "${ProjectIncludeSource}" "${ProjectIncludeGenerated}")

configure_file(cmake/DEMEConfig.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/DEME/DEMEConfig.cmake"
	@ONLY
)


# Generate the _Install Tree_ version of DEMEConfig.cmake from the base file
file(RELATIVE_PATH REL_INCLUDE_DIR ${CMAKE_INSTALL_PREFIX}/${config_directory} ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR})
set(CONF_INCLUDE_DIRS "\${DEMECMakeDir}/${REL_INCLUDE_DIR}")

# NOTE: This is placed in CMAKE_FILES_DIRECTORY to keep it out of the build tree
configure_file(cmake/DEMEConfig.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/DEMEConfig.cmake"
	@ONLY
)


# Automatically generate a DEMEConfigVersion.cmake file

write_basic_package_version_file(
	${CMAKE_CURRENT_BINARY_DIR}/DEME/DEMEConfigVersion.cmake
	VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
	COMPATIBILITY AnyNewerVersion
)

# Install the generated config files
install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/DEMEConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/DEME/DEMEConfigVersion.cmake"
	DESTINATION
		${config_directory}
)

# ---------------------------------------------------------------------------- #
# Copy data directory over
# ---------------------------------------------------------------------------- #
# set(DataDir "data")
file(COPY ${CMAKE_CURRENT_LIST_DIR}/data/ DESTINATION ${CMAKE_BINARY_DIR}/data/)
install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/data/)

# ---------------------------------------------------------------------------- #
# Build embedded demos
# ---------------------------------------------------------------------------- #
add_subdirectory(src/demo)

