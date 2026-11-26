# New source files are picked up without a manual reconfigure (CONFIGURE_DEPENDS); ESP-IDF also reads this file in
# CMake script mode to collect requirements, where that option is not allowed.
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB CORE_SRCS "*.cpp")
else()
    file(GLOB CORE_SRCS CONFIGURE_DEPENDS "*.cpp")
endif()
idf_component_register(SRCS ${CORE_SRCS} INCLUDE_DIRS "." REQUIRES ft8_lib PRIV_REQUIRES lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-format)
