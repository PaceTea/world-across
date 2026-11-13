# embed.cmake -- a binary file as a C array: cmake -DIN=x.spv -DOUT=x.h -DNAME=x_spv -P embed.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "/* Generated from ${IN} -- do not edit. */\nstatic const unsigned char ${NAME}[] = {${bytes}};\n")
