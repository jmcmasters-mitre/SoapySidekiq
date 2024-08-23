#!/bin/bash

set -x
set -e

sudo rm -r build

mkdir build

cd build
cmake ../
make clean
make
sudo make install
sudo ldconfig

