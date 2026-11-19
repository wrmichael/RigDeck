# Vendored copy of https://github.com/kgoba/ft8_lib (MIT, see LICENSE), commit 9fec6ca.
# Only the protocol core is built: message packing, LDPC, sync search, waterfall monitor, KISS FFT.
file(GLOB FT8_SRCS "ft8/*.c" "fft/*.c" "common/monitor.c")
idf_component_register(SRCS ${FT8_SRCS} INCLUDE_DIRS ".")
# Third-party code: do not let its warnings fail the build.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
