file(GLOB CORE_SRCS "*.cpp")
idf_component_register(SRCS ${CORE_SRCS} INCLUDE_DIRS "." REQUIRES ft8_lib PRIV_REQUIRES lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-format)
