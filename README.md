# Soapy SDR module for Epiq Solutions Sidekiq

SoapySDR must be installed from source if using it with the X40

# Building for non-X40 cards:
Use the normal build process for cmake projects:

$ mkdir build

$ cd build

$ cmake ../

$ make -j8

$ sudo make install


# Building for the X40:
The X40 has two different versions of python, python3.8 is default and python3.9.

So when building the SoapySDR repo we need to explicitly call out the paths to the python3.8 libraries.

So we need to run:

$  cmake .. -DPython3_EXECUTABLE=/usr/bin/python3.8 -DPython3_INCLUDE_DIR=/usr/include/python3.8 -DPython3_LIBRARY=/usr/lib/aarch64-linux-gnu/libpython3.8.so

We also need to make sure PYTHONPATH and LD_LIBRARY_PATH are set correctly:

$ export LD_LIBRARY_PATH="/usr/local/lib:/usr/lib/epiq:/usr/lib/epiq:$LD_LIBRARY_PATH"

$ export PYTHONPATH="/usr/local/lib/python3.8/site-packages$PYTHONPATH"

The SoapySidekiq also needs to be build differently:

$ cmake ../ -DPLATFORM="msiq-x40"

For all other cards, the normal cmake is sufficient.

## Licensing information
* https://github.com/pothosware/SoapySidekiq/blob/master/LICENSE
