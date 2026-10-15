# Turns a file into a C++ source with its bytes in an array (prompts_patch.h declares the names).
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P embed.cmake
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}" "// Generated from ${name} by launcher/embed.cmake; do not edit.
#include <cstddef>
#include <cstdint>

namespace dcp {

extern const uint8_t ${SYMBOL}[] = {
${bytes}
};
extern const size_t ${SYMBOL}Size = ${size};

} // namespace dcp
")
