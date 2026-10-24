#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# GEARFRAME's style generator on this computer (tests/style_test.c), under UBSan: a piece in every style, played
# and measured; build/style/*.wav to listen to. Exits non-zero on a failure.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/style
cc -O1 -g -fsanitize=signed-integer-overflow,shift-exponent,bounds -fno-sanitize-recover=all -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
   -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/src -Ifirmware/gen -o build/style/style_test tests/style_test.c -lm
build/style/style_test
