file(GLOB CORE_SRCS CONFIGURE_DEPENDS "*.cpp")   # new source files are picked up without a manual reconfigure
idf_component_register(SRCS ${CORE_SRCS} INCLUDE_DIRS "." REQUIRES ft8_lib PRIV_REQUIRES lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-format)
