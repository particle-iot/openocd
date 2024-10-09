#!/bin/bash

# Dependencies:
#
# git
# gcc
# cmake
# autotools
# pkg-config
#
# Linux: libudev-dev
# macOS: Homebrew (https://brew.sh)
# Windows: MSYS2 (https://www.msys2.org)
#
# MSYS2 installation notes:
# https://github.com/orlp/dev-on-windows/wiki/Installing-GCC--&-MSYS2

set -e

# OPENOCD_GIT_URL=https://github.com/particle-iot/openocd
# OPENOCD_GIT_TAG=0b88dcc59c51e3999d56dfa90eec945cff54c9d8

LIBUSB_GIT_URL=https://github.com/libusb/libusb.git
LIBUSB_GIT_TAG=v1.0.24

HIDAPI_GIT_URL=https://github.com/libusb/hidapi.git
HIDAPI_GIT_TAG=hidapi-0.14.0

LIBFTDI_GIT_URL=git://developer.intra2net.com/libftdi
LIBFTDI_GIT_TAG=v1.5

# CAPSTONE_GIT_URL=https://github.com/capstone-engine/capstone.git
# CAPSTONE_GIT_TAG=5.0.1
# CAPSTONE_CONFIG="CAPSTONE_BUILD_CORE_ONLY=yes CAPSTONE_STATIC=yes CAPSTONE_SHARED=no"

# Environment-specific settings
cmake_generator="Unix Makefiles"
cmake_command=cmake
if [[ $OSTYPE == darwin* ]]; then
  export MACOSX_DEPLOYMENT_TARGET=10.7
elif [[ $OSTYPE == msys ]]; then
  cmake_generator="MSYS Makefiles"
  cmake_command=/mingw64/bin/cmake
fi

# This script's directory
script_dir=$(cd $(dirname $0) && pwd)
cd $script_dir

# Build directory
build_dir=$script_dir/.build
rm -rf $build_dir && mkdir -p $build_dir

# Temporary installation directory for dependencies
local_dir=$script_dir/.local
rm -rf $local_dir && mkdir -p $local_dir

# Destination directory for OpenOCD files
target_dir=$script_dir/openocd
rm -rf $target_dir && mkdir -p $target_dir

# Path to local pkg-config files
export PKG_CONFIG_PATH=$local_dir/lib/pkgconfig

# echo "Building capstone"
# echo $target_dir
# cd $build_dir
# git clone $CAPSTONE_GIT_URL capstone && cd capstone
# git checkout $CAPSTONE_GIT_TAG
# git submodule update --init --recursive
# make install DESTDIR=$target_dir PREFIX=$target_dir $CAPSTONE_CONFIG

echo "Building libusb"

cd $build_dir
git clone $LIBUSB_GIT_URL libusb && cd libusb
git checkout $LIBUSB_GIT_TAG
git submodule update --init --recursive

./bootstrap.sh
./configure --prefix=$local_dir
make && make install

echo "Building hidapi"

cd $build_dir
git clone $HIDAPI_GIT_URL hidapi && cd hidapi
git checkout $HIDAPI_GIT_TAG
git submodule update --init --recursive

./bootstrap
./configure --prefix=$local_dir
make && make install

echo "Building libftdi"

cd $build_dir
git clone $LIBFTDI_GIT_URL libftdi && cd libftdi
git checkout $LIBFTDI_GIT_TAG
git submodule update --init --recursive

mkdir .build && cd .build
$cmake_command .. -G "$cmake_generator" -DCMAKE_INSTALL_PREFIX:PATH=$local_dir -DFTDI_EEPROM=OFF -DEXAMPLES=OFF
make && make install

echo "Building openocd"

cd $script_dir
# git clone $OPENOCD_GIT_URL openocd && cd openocd
# git checkout $OPENOCD_GIT_TAG
git submodule update --init --recursive

./bootstrap
if [[ $OSTYPE == linux-gnu ]]; then
LDFLAGS="-Wl,-rpath=$(pwd)/libexec"
elif [[ $OSTYPE == darwin* ]]; then
LDFLAGS="-Wl,-rpath,@loader_path/../libexec"
fi

./configure --prefix=$target_dir --disable-werror LDFLAGS="${LDFLAGS}"

make && make install

# Copy dependencies
if [[ $OSTYPE == linux-gnu ]]; then
  mkdir -p $target_dir/libexec
  cp -P $local_dir/lib/*.so $local_dir/lib/*.so.* $target_dir/libexec
elif [[ $OSTYPE == darwin* ]]; then
  mkdir -p $target_dir/libexec
  cp -P -R $local_dir/lib/*.dylib $target_dir/libexec
elif [[ $OSTYPE == msys ]]; then
  cp $local_dir/bin/*.dll $target_dir/bin
  # MinGW runtime libraries
  mingw_dir=$(dirname $(which gcc))
  cp $mingw_dir/libwinpthread-1.dll $target_dir/bin
  # This library seems to be available only on 32-bit platforms
  cp $mingw_dir/libgcc_s_dw2-1.dll $target_dir/bin || true
fi
