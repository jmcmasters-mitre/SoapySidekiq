#!/bin/bash

set -x
set -e

sudo rm -rf build

mkdir build

cd build
cmake ../
make clean
make
sudo make install
sudo ldconfig

