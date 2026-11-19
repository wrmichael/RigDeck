#!/bin/sh
# Desktop preview + tap test of the touch interface. Needs an LVGL v9.5.0 checkout:
#   git clone --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl.git   and   LVGL=/path/to/lvgl sh test/ui_sim.sh
# Screenshots are written to test/out/ui/*.ppm.
set -e
cd "$(dirname "$0")/.."
: "${LVGL:?set LVGL to an LVGL v9.5.0 source directory}"
mkdir -p test/out/lv
[ -f test/out/libft8.a ] || TESTS=none sh test/build.sh
if [ ! -f test/out/lv/lv_conf.h ]; then
  sed -e '0,/#if 0/s//#if 1/' \
      -e 's/\(#define LV_USE_STDLIB_MALLOC \+\)LV_STDLIB_BUILTIN/\1LV_STDLIB_CLIB/' \
      -e 's/\(#define LV_USE_STDLIB_STRING \+\)LV_STDLIB_BUILTIN/\1LV_STDLIB_CLIB/' \
      -e 's/\(#define LV_USE_STDLIB_SPRINTF \+\)LV_STDLIB_BUILTIN/\1LV_STDLIB_CLIB/' \
      -e 's/\(#define LV_FONT_MONTSERRAT_\(16\|20\|28\|36\) \+\)0/\11/' "$LVGL/lv_conf_template.h" > test/out/lv/lv_conf.h
fi
if [ ! -f test/out/lv/liblvgl.a ]; then
  find "$LVGL/src" -name "*.c" | while read f; do
    gcc -O1 -w -DLV_CONF_INCLUDE_SIMPLE -Itest/out/lv -I"$LVGL" -c "$f" -o "test/out/lv/$(echo "$f" | sed 's#[/.]#_#g').o"
  done
  ar rcs test/out/lv/liblvgl.a test/out/lv/*.o
fi
g++ -std=c++17 -O2 -w -DLV_CONF_INCLUDE_SIMPLE -Itest/out/lv -I"$LVGL" -Icomponents/core -Icomponents/ft8_lib \
    test/ui_sim.cpp main/ui.cpp components/core/*.cpp test/out/libft8.a test/out/lv/liblvgl.a -lm -lpthread -o test/out/ui_sim
./test/out/ui_sim
