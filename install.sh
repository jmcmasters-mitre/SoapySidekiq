#!/bin/bash

set -x
set -e

sudo rm -rf build

mkdir build

cd build
cmake ../ -D$1
make clean
make
sudo make install
sudo ldconfig

