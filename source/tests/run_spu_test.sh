#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# GEARFRAME's SPU engine on this computer (tests/spu_test.c), under UBSan: every SPU sound measured, the voices,
# the ends of the knobs. build/spu/*.wav to listen to. Exits non-zero on a failure.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/spu
cc -O1 -g -fsanitize=signed-integer-overflow,shift-exponent,bounds -fno-sanitize-recover=all -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
   -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/src -Ifirmware/gen -o build/spu/spu_test tests/spu_test.c -lm
build/spu/spu_test
