export PREFIX="$HOME/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

sudo apt update && sudo apt install -y build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo wget

mkdir -p ~/src && cd ~/src

wget https://ftp.gnu.org/gnu/binutils/binutils-2.45.tar.xz
tar -xf binutils-2.45.tar.xz

mkdir build-binutils && cd build-binutils
../binutils-2.45/configure --target=$TARGET --prefix=$PREFIX --with-sysroot --disable-nls --disable-werror
make -j$(nproc)
make install

cd ~/src

wget https://ftp.gnu.org/gnu/gcc/gcc-15.1.0/gcc-15.1.0.tar.xz
tar -xf gcc-15.1.0.tar.xz

cd gcc-15.1.0
./contrib/download_prerequisites
cd ..

mkdir build-gcc && cd build-gcc
../gcc-15.1.0/configure --target=$TARGET --prefix=$PREFIX --disable-nls --enable-languages=c,c++ --without-headers
make all-gcc -j$(nproc)
make all-target-libgcc -j$(nproc)
make install-gcc
make install-target-libgcc
