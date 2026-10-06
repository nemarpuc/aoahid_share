# Turns a binary file into a C++ source defining two symbols:
#   const unsigned char <SYMBOL>_data[];
#   const unsigned int  <SYMBOL>_size;
# Usage: cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P EmbedBinary.cmake
foreach(required INPUT OUTPUT SYMBOL)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "EmbedBinary.cmake: ${required} is not set")
  endif()
endforeach()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){20})" "\\1\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)

file(WRITE "${OUTPUT}.tmp"
"// Generated from ${name} by EmbedBinary.cmake; do not edit.
extern const unsigned char ${SYMBOL}_data[];
extern const unsigned int ${SYMBOL}_size;
alignas(4) const unsigned char ${SYMBOL}_data[] = {
${bytes}
};
const unsigned int ${SYMBOL}_size = ${size}u;
")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
