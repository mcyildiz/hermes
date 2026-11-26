# hermes_embed_file(<input> <output.inc> <symbol>): writes a C++ array
#   alignas(16) static const unsigned char <symbol>[] = {...};
#   static const unsigned int <symbol>_size = N;
# at configure time (re-run when the input changes).
function(hermes_embed_file input output symbol)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${input})
  if(EXISTS ${output} AND ${output} IS_NEWER_THAN ${input})
    return()
  endif()
  file(READ ${input} hex HEX)
  string(LENGTH "${hex}" n)
  math(EXPR bytes "${n} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
  file(WRITE ${output} "alignas(16) static const unsigned char ${symbol}[] = {\n${body}\n};\nstatic const unsigned int ${symbol}_size = ${bytes};\n")
endfunction()
