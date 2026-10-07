add_executable(test_citadel_vehicle test_citadel_vehicle.cpp)
target_include_directories(test_citadel_vehicle PRIVATE ${CMAKE_SOURCE_DIR}/LIB386/H)
set_target_properties(test_citadel_vehicle PROPERTIES CXX_STANDARD 11)
add_test(NAME test_citadel_vehicle COMMAND test_citadel_vehicle)
register_host_test(test_citadel_vehicle)

if(EXISTS "${PROJECT_SOURCE_DIR}/assets/modern/citadel/vehicles/prototypes/boat-b-concept.glb")
    add_executable(test_citadel_boat test_citadel_boat.cpp
        ${PROJECT_SOURCE_DIR}/LIB386/OBJECT/CITADEL_BOAT.CPP)
    target_include_directories(test_citadel_boat PRIVATE
        ${PROJECT_SOURCE_DIR}/LIB386/H ${PROJECT_BINARY_DIR}/LIB386/OBJECT)
    add_dependencies(test_citadel_boat citadel_boat)
    set_target_properties(test_citadel_boat PROPERTIES CXX_STANDARD 11)
    add_test(NAME test_citadel_boat COMMAND test_citadel_boat)
    register_host_test(test_citadel_boat)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/assets/modern/citadel/vehicles/prototypes/scooter-b-concept.glb")
    add_executable(test_citadel_scooter test_citadel_scooter.cpp
        ${PROJECT_SOURCE_DIR}/LIB386/OBJECT/CITADEL_SCOOTER.CPP)
    target_include_directories(test_citadel_scooter PRIVATE
        ${PROJECT_SOURCE_DIR}/LIB386/H ${PROJECT_BINARY_DIR}/LIB386/OBJECT)
    add_dependencies(test_citadel_scooter citadel_scooter)
    set_target_properties(test_citadel_scooter PROPERTIES CXX_STANDARD 11)
    add_test(NAME test_citadel_scooter COMMAND test_citadel_scooter)
    register_host_test(test_citadel_scooter)
endif()
