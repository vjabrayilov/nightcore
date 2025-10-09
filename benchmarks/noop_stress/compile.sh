#!/bin/bash

BASE_DIR=$(realpath $(dirname $0))
NIGHTCORE_ROOT=$(realpath $BASE_DIR/../..)

gcc -shared -fPIC -O2 \
    -I$NIGHTCORE_ROOT/include \
    $BASE_DIR/noop.c \
    -o $BASE_DIR/libnoop.so

echo "Compiled libnoop.so"
