#!/bin/bash

set -e

# Check if the script is being run as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run script as root"
    exit 1
fi

# Check if the script is being run on a supported distribution
if [ -f /etc/os-release ]; then
    . /etc/os-release
    if [[ "$ID" != "ubuntu" && "$ID" != "debian" ]]; then
        echo "Unsupported distribution: $ID"
        exit 1
    fi
else
    echo "Cannot determine distribution"
    echo "Continue? (y/N)"
    read -r response
    if [[ "$response" != "y" && "$response" != "Y" ]]; then
        exit 1
    fi
fi 

# Update package lists and install dependencies
apt update
apt install -y python3-pip python3-venv python3-dev

sudo apt install -y \
  build-essential \
  bison \
  flex \
  libgmp3-dev \
  libmpc-dev \
  libmpfr-dev \
  texinfo \
  libisl-dev \
  wget \
  curl \
  nasm

# Set up environment variables for cross-compiler
export PREFIX="/usr/local"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

# Create source directory
mkdir -p $PREFIX/src
cd $PREFIX/src

# Download binutils
echo "Downloading binutils..."
wget -q https://ftp.gnu.org/gnu/binutils/binutils-2.36.tar.xz
tar -xf binutils-2.36.tar.xz

# Download GCC
echo "Downloading GCC..."
wget -q https://ftp.gnu.org/gnu/gcc/gcc-11.1.0/gcc-11.1.0.tar.xz
tar -xf gcc-11.1.0.tar.xz

# Build Binutils
echo "Building binutils..."
mkdir build-binutils
cd build-binutils
../binutils-2.36/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
make -j $(nproc)
make install
cd ..
rm -rf build-binutils binutils-2.36 binutils-2.36.tar.xz

# Apply GCC patch and copy configuration
echo "Preparing GCC build..."
cd gcc-11.1.0/gcc
patch < /media/joelminecrafter/Thesoup/"Minimal OS"/buildenv/gcc/config.gcc.patch
cd ..
cp /media/joelminecrafter/Thesoup/"Minimal OS"/buildenv/gcc/t-x86_64-elf gcc/config/i386/t-x86_64-elf
cd ..

# Build GCC
echo "Building GCC..."
mkdir build-gcc
cd build-gcc
../gcc-11.1.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headers
make -j $(nproc) all-gcc
make -j $(nproc) all-target-libgcc
make install-gcc
make install-target-libgcc
cd ..
rm -rf build-gcc gcc-11.1.0 gcc-11.1.0.tar.xz

echo "Setup complete! Cross-compiler and NASM installed successfully."
