#!/bin/sh
# Host-side tests for the portable core (no ESP toolchain needed):  sh test/build.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p test/out
if [ ! -f test/out/libft8.a ]; then
  for f in components/ft8_lib/ft8/*.c components/ft8_lib/fft/*.c components/ft8_lib/common/monitor.c; do
    gcc -O2 -w -Icomponents/ft8_lib -c "$f" -o "test/out/$(basename "$f").o"
  done
  ar rcs test/out/libft8.a test/out/*.o
fi
CORE="$(ls components/core/*.cpp)"
for t in ${TESTS:-t_codec t_units t_qso t_psk t_cw t_cwpath t_cat t_aprs}; do
  [ -f test/$t.cpp ] || continue
  g++ -std=c++17 -O2 -Wall -Wextra -Icomponents/ft8_lib -Icomponents/core test/$t.cpp $CORE test/out/libft8.a -lm -lpthread -o test/out/$t
  ./test/out/$t
done
if command -v node >/dev/null 2>&1 && [ -f test/out/state_ft8.json ]; then
  python3 -c "import json,sys; [json.load(open('test/out/state_%s.json' % t)) for t in ('ft8','slow','ft4')]; print('state JSON: valid')"
  node test/ui_check.js test/out/state_ft8.json
fi
