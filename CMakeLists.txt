cmake_minimum_required(VERSION 3.10)

# Set the project name and version
project(C-MemoryMuncher VERSION 1.0)

# Specify the C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)


# Add compiler flags to suppress warnings
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -g")
endif()

# Add the library
add_library(MemoryMuncher STATIC muncher.c)  # Use STATIC or SHARED depending on your needs



# If you have a separate directory for tests, you might want to include it like this
enable_testing()
add_subdirectory(tests)
