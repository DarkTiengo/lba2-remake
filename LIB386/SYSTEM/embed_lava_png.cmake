if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "embed_lava_png.cmake requires INPUT and OUTPUT")
endif()

file(READ "${INPUT}" png_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," png_bytes "${png_hex}")
file(WRITE "${OUTPUT}"
    "static const unsigned char kLavaPng[] = {${png_bytes}};\n"
    "static const unsigned int kLavaPngSize = sizeof(kLavaPng);\n")
