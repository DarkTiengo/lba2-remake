add_executable(test_gpu_water test_gpu_water.cpp
    ${CMAKE_SOURCE_DIR}/SOURCES/3DEXT/TERRAIN_GPU.CPP
    ${CMAKE_SOURCE_DIR}/LIB386/SVGA/GPUWATER.CPP)
target_include_directories(test_gpu_water PRIVATE
    ${CMAKE_SOURCE_DIR}/SOURCES ${CMAKE_SOURCE_DIR}/LIB386/H)
set_target_properties(test_gpu_water PROPERTIES CXX_STANDARD 11)
add_test(NAME test_gpu_water COMMAND test_gpu_water)
register_host_test(test_gpu_water)

add_executable(test_water_contact test_water_contact.cpp
    ${CMAKE_SOURCE_DIR}/SOURCES/WATER.CPP
    ${CMAKE_SOURCE_DIR}/LIB386/SVGA/GPUWATER.CPP)
target_include_directories(test_water_contact PRIVATE
    ${CMAKE_SOURCE_DIR}/SOURCES ${CMAKE_SOURCE_DIR}/LIB386/H)
set_target_properties(test_water_contact PROPERTIES CXX_STANDARD 11)
add_test(NAME test_water_contact COMMAND test_water_contact)
register_host_test(test_water_contact)
