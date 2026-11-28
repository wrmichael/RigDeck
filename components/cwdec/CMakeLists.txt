# Plain Morse (CW) decoder component. Self-contained: standard C++ only, no ESP-IDF
# or RigDeck dependencies. Feed it the same 12 kHz float sample stream the other
# modems get; it appends decoded characters to a std::string. See include/cwdec.h.
idf_component_register(SRCS "cwdec.cpp" INCLUDE_DIRS "include")
