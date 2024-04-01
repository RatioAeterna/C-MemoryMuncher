# Add test executables
add_executable(standard_malloc standard_malloc.c)
add_executable(munch_functionality munch_functionality.c)

# Link the test executable with the main application/library if needed
target_link_libraries(munch_functionality MemoryMuncher)

# Add the tests to be run by CMake's testing system
add_test(NAME MallocMemoryUsageTest COMMAND standard_malloc_test)
add_test(NAME MemoryMuncherTest COMMAND memory_munch_test)
