#!/usr/bin/env bash

# Stop executing script on first error
set -ex

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y net-tools
apt-get install -y python3 python3-dev python3-pip
pip3 install --user virtualenv
pip3 install --user virtualenvwrapper

#other dependencies
apt-get install -y \
    build-essential cmake g++ gdb git lcov 

#ROCm
apt-get install -y wget gnupg2
# usermod -a -G video $LOGNAME
# usermod -a -G render $LOGNAME
# those work under docker
groupadd -f render
groupadd -f video

apt-get update
wget https://repo.radeon.com/amdgpu-install/21.40/ubuntu/focal/amdgpu-install-21.40.40500-1_all.deb
apt-get install -y ./amdgpu-install-21.40.40500-1_all.deb
apt-get update
amdgpu-install -y --usecase=hiplibsdk,rocm
rocm_version=rocm-4.5.0
echo export LD_LIBRARY_PATH=/opt/rocm-${rocm_version}/lib | tee -a ${HOME}/.bashrc
echo export PATH=$PATH:/opt/${rocm_version}/bin:/opt/${rocm_version}/opencl/bin | tee -a ${HOME}/.bashrc

#verify ROCm installation
/opt/${rocm_version}/bin/rocminfo
echo "ROCm ${rocm_version} build successfully!"

export CXX=/opt/${rocm_version}/llvm/bin/clang++
export CC=/opt/${rocm_version}/llvm/bin/clang

#Sqlite3
wget https://sqlite.org/2017/sqlite-autoconf-3170000.tar.gz
tar -xf sqlite-autoconf-3170000.tar.gz
cd sqlite-autoconf-3170000
CFLAGS="Os -DCMAKE_POSITION_INDEPENDENT_CODE=On" ./configure
make -j 12
# install to /usr/local/
make install
cd ..
echo "SQlite 3.17 build successfully!"

#boost 1.72
wget https://boostorg.jfrog.io/artifactory/main/release/1.72.0/source/boost_1_72_0.tar.bz2
tar --bzip2 -xf boost_1_72_0.tar.bz2
cd boost_1_72_0
./bootstrap.sh -with-libraries=filesystem,system
# install to /usr/local
./b2 install
cd ..
echo "Boost 1.72 libs build successfully!"

#Half
wget https://sourceforge.net/projects/half/files/latest/download
mv download half-2.2.0.zip
unzip half-2.2.0 -d half-2.2.0 
mv half-2.2.0/include/half.hpp /usr/local/include
echo "Half header installed successfully!"

# https://github.com/ROCmSoftwarePlatform/llvm-project-mlir.git
git clone --depth=100 --branch miopen-dialect https://github.com/ROCmSoftwarePlatform/llvm-project-mlir.git
cd llvm-project-mlir
mkdir build && cd build
cmake -G Unix Makefiles -DCMAKE_BUILD_TYPE=Release -DBUILD_FAT_LIBMLIRMIOPEN=1 ../llvm
cmake --build . -j 12
echo "llvm-mlir build successfully!"
