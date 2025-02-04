#!/bin/bash

cd build
make clean
bear -- make
sudo make install
sudo ldconfig
cp compile_commands.json ../.
