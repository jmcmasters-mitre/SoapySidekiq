#!/bin/bash

set -x
set -e

sudo rm -rf build

mkdir build

cd build

if [ -n "$1" ]; then
    cmake ../ -D$1
else
    cmake ../
fi

make clean
make
sudo make install
sudo ldconfig

