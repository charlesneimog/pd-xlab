set(ELSE_TAG "v.1.0-rc14")

file(
  DOWNLOAD
  "https://raw.githubusercontent.com/porres/pd-else/refs/tags/${ELSE_TAG}/Source/Audio/vu~.c"
  "${CMAKE_BINARY_DIR}/vu~.c")

pd_add_external(vu~ "${CMAKE_BINARY_DIR}/vu~.c")
