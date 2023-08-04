#!/usr/bin/env bash
##===- utils/build-mlir-aie.sh - Build mlir-aie --*- Script -*-===##
# 
# This file licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
##===----------------------------------------------------------------------===##
#
# This script builds mlir-aie given <llvm dir>.
# Assuming they are all in the same subfolder, it would look like:
#
# build-mlir-aie.sh <llvm dir> <build dir> <install dir> <mlir-air-dir> <x86-libxaie-dir> 
#
# e.g. build-mlir-aie.sh /scratch/llvm
#
# <build dir>    - optional, mlir-aie/build dir name, default is 'build'
# <install dir>  - optional, mlir-aie/install dir name, default is 'install'
# <mlir-air-dir>    - optional, path to mlir-air to get runtime functions, necessary when compiling for VCK5000
# <x86-libxaie-dir> - optional, path to the x86 libxaie installation, necessary when compiling for VCK5000
#
##===----------------------------------------------------------------------===##

if [ "$#" -lt 1 ]; then
    echo "ERROR: Needs at least 1 arguments for <llvm build dir>."
    exit 1
fi

BASE_DIR=`realpath $(dirname $0)/..`
CMAKEMODULES_DIR=$BASE_DIR/cmake

LLVM_BUILD_DIR=`realpath $1`

BUILD_DIR=${2:-"build"}
INSTALL_DIR=${3:-"install"}
MLIR_AIR_DIR=${4:-""}
LIBXAIE_DIR=${5:-""}

mkdir -p $BUILD_DIR
mkdir -p $INSTALL_DIR
cd $BUILD_DIR
set -o pipefail
set -e
cmake -GNinja\
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_DIR=${LLVM_BUILD_DIR}/lib/cmake/llvm \
    -DLibXAIE_x86_64_DIR=${LIBXAIE_DIR} \
    -DMLIR_AIR_DIR=${MLIR_AIR_DIR} \
    -DMLIR_DIR=${LLVM_BUILD_DIR}/lib/cmake/mlir \
    -DCMAKE_MODULE_PATH=${CMAKEMODULES_DIR}/modulesXilinx \
    -DCMAKE_INSTALL_PREFIX="../${INSTALL_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DAIE_ENABLE_BINDINGS_PYTHON=ON \
    "-DAIE_RUNTIME_TARGETS=x86_64;aarch64" \
    -DAIE_RUNTIME_TEST_TARGET=aarch64 \
    .. |& tee cmake.log

ninja |& tee ninja.log
ninja install |& tee ninja-install.log
#ninja check-aie |& tee ninja-check-aie.log
