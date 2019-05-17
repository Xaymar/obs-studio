#!/bin/sh
set -ex

sudo dpkg --add-architecture amd64
#sudo dpkg --add-architecture i386
sudo apt-get -qq update
sudo apt-get install -y \
        build-essential \
		checkinstall \
        cmake \
        swig \
        pkg-config \
		gcc-multilib \
        python3-dev:amd64 \
        libasound2-dev:amd64 \
        libavcodec-dev:amd64 \
        libavdevice-dev:amd64 \
        libavfilter-dev:amd64 \
        libavformat-dev:amd64 \
        libavutil-dev:amd64 \
        libcurl4-openssl-dev:amd64 \
        libfdk-aac-dev:amd64 \
        libfontconfig-dev:amd64 \
        libfreetype6-dev:amd64 \
        libgl1-mesa-dev:amd64 \
        libjack-jackd2-dev:amd64 \
        libjansson-dev:amd64 \
        libluajit-5.1-dev:amd64 \
        libpulse-dev:amd64 \
        libqt5x11extras5-dev:amd64 \
        libspeexdsp-dev:amd64 \
        libswresample-dev:amd64 \
        libswscale-dev:amd64 \
        libudev-dev:amd64 \
        libv4l-dev:amd64 \
        libvlc-dev:amd64 \
        libx11-dev:amd64 \
        libx264-dev:amd64 \
        libxcb-randr0-dev:amd64 \
        libxcb-shm0-dev:amd64 \
        libxcb-xinerama0-dev:amd64 \
        libxcomposite-dev:amd64 \
        libxinerama-dev:amd64 \
        qtbase5-dev:amd64 \
        libqt5svg5-dev:amd64
		
# build mbedTLS
cd ~/projects
mkdir mbedtls
cd mbedtls
mbedtlsPath=$PWD
curl -L -O https://github.com/ARMmbed/mbedtls/archive/mbedtls-2.12.0.tar.gz
tar -xf mbedtls-2.12.0.tar.gz
mkdir build
cd ./build
cmake -DENABLE_TESTING=Off -DUSE_SHARED_MBEDTLS_LIBRARY=On ../mbedtls-mbedtls-2.12.0
make -j 12
sudo make install

# return to OBS build dir
cd $APPVEYOR_BUILD_FOLDER
