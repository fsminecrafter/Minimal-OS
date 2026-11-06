git submodule update --init --remote

sudo apt update
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
  make \
  nasm \
  xorriso \
  grub-pc-bin \
  grub-common \
  mtools \
  qemu-system

./build_gccbinutils.sh

fallocate -l 256M sata256.img

echo "Ready to build Minimal-OS"