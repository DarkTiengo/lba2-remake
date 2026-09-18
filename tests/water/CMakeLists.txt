add_executable(test_gpu_water test_gpu_water.cpp
    ${CMAKE_SOURCE_DIR}/SOURCES/3DEXT/TERRAIN_GPU.CPP
    ${CMAKE_SOURCE_DIR}/LIB386/SVGA/GPUWATER.CPP)
target_include_directories(test_gpu_water PRIVATE
    ${CMAKE_SOURCE_DIR}/SOURCES ${CMAKE_SOURCE_DIR}/LIB386/H)
target_compile_definitions(test_gpu_water PRIVATE LBA2_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(test_gpu_water PROPERTIES CXX_STANDARD 11)
add_test(NAME test_gpu_water COMMAND test_gpu_water)
register_host_test(test_gpu_water)
