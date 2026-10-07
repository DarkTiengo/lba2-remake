# add_library(obj AFF_OBJ.ASM)
add_library(obj AFF_OBJ.CPP AFF_GPU.CPP)
add_library(citadel_boat CITADEL_BOAT.CPP)
add_library(citadel_scooter CITADEL_SCOOTER.CPP)
target_link_libraries(obj PRIVATE 3D citadel_boat citadel_scooter)
set(BOAT_GLB "${PROJECT_SOURCE_DIR}/assets/modern/citadel/vehicles/prototypes/boat-b-concept.glb")
set(BOAT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/citadel_boat_data.h")
if(EXISTS "${BOAT_GLB}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(OUTPUT "${BOAT_HEADER}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/dev/export_citadel_boat.py"
                "${BOAT_GLB}" "${BOAT_HEADER}"
        DEPENDS "${BOAT_GLB}" "${PROJECT_SOURCE_DIR}/scripts/dev/export_citadel_boat.py"
        VERBATIM)
else()
    file(WRITE "${BOAT_HEADER}"
        "static const T_CITADEL_BOAT_VERTEX s_boatVertices[1] = {};\nstatic const U32 s_boatIndices[1] = {0};\n")
endif()
target_sources(citadel_boat PRIVATE "${BOAT_HEADER}")
target_include_directories(citadel_boat PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set(SCOOTER_GLB "${PROJECT_SOURCE_DIR}/assets/modern/citadel/vehicles/prototypes/scooter-b-concept.glb")
set(SCOOTER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/citadel_scooter_data.h")
if(EXISTS "${SCOOTER_GLB}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(OUTPUT "${SCOOTER_HEADER}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/dev/export_citadel_boat.py"
                "${SCOOTER_GLB}" "${SCOOTER_HEADER}" --vehicle scooter
        DEPENDS "${SCOOTER_GLB}" "${PROJECT_SOURCE_DIR}/scripts/dev/export_citadel_boat.py"
        VERBATIM)
else()
    file(WRITE "${SCOOTER_HEADER}"
        "static const T_CITADEL_SCOOTER_VERTEX s_scooterVertices[1] = {};\nstatic const U32 s_scooterIndices[1] = {0};\n")
endif()
target_sources(citadel_scooter PRIVATE "${SCOOTER_HEADER}")
target_include_directories(citadel_scooter PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# set_source_files_properties(AFF_OBJ.ASM PROPERTIES LANGUAGE ASM_MASM)

# Source files in folder but not used on CMakeScript:
# - 3D_OBJ.ASM
