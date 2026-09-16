# Turns PrivateStorageMaster.ini into a header holding its exact bytes, so the plugin
# can write the documented ini out when it finds none beside it. The ini file
# stays the only copy of that text.
#
# Read and written as hex on purpose. Carrying it through a CMake string and a
# raw string literal turned every LF into CRLF, so the file the plugin wrote
# was not the file that ships.
#
#   cmake -DIN=<ini> -DOUT=<header> -P embed_ini.cmake

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" nibbles)
math(EXPR count "${nibbles} / 2")
string(REGEX REPLACE "(..)" "0x\1," bytes "${hex}")

file(WRITE "${OUT}"
"// Generated from PrivateStorageMaster.ini by cmake/embed_ini.cmake. Do not edit.\n"
"#pragma once\n"
"\n"
"inline constexpr unsigned kDefaultIniSize = ${count};\n"
"inline constexpr unsigned char kDefaultIni[] = {\n${bytes}\n};\n")
