#!/bin/sh
# Builds bin/h2import with the system C compiler (clang/gcc, Linux/macOS).
# Emscripten: emcc -O2 -std=c99 h2import.c -c  (the library has no other deps)
set -e
d=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$d/bin"
${CC:-cc} -O2 -std=c99 -Wall -Wextra -pedantic -o "$d/bin/h2import" "$d/h2import.c" "$d/h2import_main.c"
