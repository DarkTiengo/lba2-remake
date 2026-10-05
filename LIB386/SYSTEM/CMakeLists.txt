# General sources
set(SOURCES_FILES ANDROID.CPP AVAILMEM.CPP BUNDLE.CPP CDDA.CPP CDTRACKS.CPP CMDLINE.CPP CRASH.CPP CRASH_LINUX.CPP CRASH_MACOS.CPP CRASH_POSIX.CPP CRASH_WIN.CPP CUE.CPP DEFFILE.CPP DISCIMG.CPP DISPOS.CPP EVENTS.CPP EXIT.CPP
        FILECOPY.CPP FILENAME.CPP FILES.CPP FOCUS_AUDIO.CPP GPURENDER.CPP HQFILE.CPP HQMEM.CPP HQRLOAD.CPP HQRMEM.CPP HQRMLOAD.CPP HQRRESS.CPP
        INPUT.CPP INPUT_FLOW.CPP ISO9660.CPP ITOA.CPP KEYBOARD.CPP LOADMALL.CPP LOADSAVE.CPP LOGPRINT.CPP LOG.CPP MOUSE.CPP N_MALLOC.CPP
        PLATFORM.CPP RANDOM.CPP S_MALLOC.CPP STRING.CPP SYSTEM.CPP TIMER.CPP UIOVERLAY.CPP WINDOW.CPP UTILS.CPP)

# set(SOURCES_FILES_ASM CPU.ASM FASTCPYF.ASM LZ.ASM MOUSEDAT.ASM)
set(SOURCES_FILES_ASM CPU.CPP FASTCPYF.CPP LZ.CPP MOUSEDAT.CPP)

add_library(sys ${SOURCES_FILES} ${SOURCES_FILES_ASM})
target_link_libraries(sys svg)

set(LAVA_PNG_EMBEDDED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/lava_png_embedded.h")
add_custom_command(
    OUTPUT "${LAVA_PNG_EMBEDDED_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${PROJECT_SOURCE_DIR}/assets/modern/lava-flow.png"
            "-DOUTPUT=${LAVA_PNG_EMBEDDED_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_lava_png.cmake"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/assets/modern/lava-flow.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/embed_lava_png.cmake"
    VERBATIM)
add_custom_target(embed_lava_png DEPENDS "${LAVA_PNG_EMBEDDED_HEADER}")
add_dependencies(sys embed_lava_png)
target_include_directories(sys PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

set(VEHICLE_PNG_EMBEDDED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vehicle_png_embedded.h")
add_custom_command(
    OUTPUT "${VEHICLE_PNG_EMBEDDED_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${PROJECT_SOURCE_DIR}/assets/modern/citadel/vehicle-materials.png"
            "-DOUTPUT=${VEHICLE_PNG_EMBEDDED_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_vehicle_png.cmake"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/assets/modern/citadel/vehicle-materials.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/embed_vehicle_png.cmake"
    VERBATIM)
add_custom_target(embed_vehicle_png DEPENDS "${VEHICLE_PNG_EMBEDDED_HEADER}")
add_dependencies(sys embed_vehicle_png)

# Configure all ASM files to be recognized as such (workaround for bug where .ASM uppercase extension is not recognized)
# foreach (FILE_ASM IN LISTS SOURCES_FILES_ASM)
#     set_source_files_properties(${FILE_ASM} PROPERTIES LANGUAGE ASM_MASM)
# endforeach ()
