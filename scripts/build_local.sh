#!/usr/bin/env bash
set -xe
HERE=$(dirname "$(realpath "$0")")

unameOut="$(uname -s)"
case "${unameOut}" in
    Linux*)     machine=linux;;
    Darwin*)    machine=macos;;
    CYGWIN*)    machine=windows;;
    MINGW*)     machine=windows;;
    MSYS_NT*)   machine=windows;;
    *)          machine="UNKNOWN:${unameOut}"
esac
echo "${machine}"

export APPLY_PATCHES=true
export PIP_FIND_LINKS="https://makslevental.github.io/wheels"
export MLIR_COMMIT="17.0.0.2023083017+35ca6498"

if [ "$machine" == "linux" ]; then
  export CIBW_ARCHS=${CIBW_ARCHS:-x86_64}
  export PARALLEL_LEVEL=15
  export MATRIX_OS=ubuntu-20.04
elif [ "$machine" == "macos" ]; then
  export CIBW_ARCHS=${CIBW_ARCHS:-arm64}
  export MATRIX_OS=macos-11
  export PARALLEL_LEVEL=32
else
  export MATRIX_OS=windows-2019
  export CIBW_ARCHS=${CIBW_ARCHS:-AMD64}
fi

ccache --show-stats
ccache --print-stats
ccache --show-config

export HOST_CCACHE_DIR="$(ccache --get-config cache_dir)"

if [ x"$CIBW_ARCHS" == x"aarch64" ]; then
  export PIP_NO_BUILD_ISOLATION="false"
  pip install -r $HERE/../requirements.txt
  $HERE/../scripts/pip_install_mlir.sh

  CMAKE_GENERATOR=Ninja \
  pip wheel $HERE/.. -v -w $HERE/../wheelhouse
else
  cibuildwheel "$HERE"/.. --platform "$machine"
fi

cp -a $HERE/../scripts $HERE/../python_bindings/
cp -a $HERE/../requirements.txt $HERE/../python_bindings/
cibuildwheel "$HERE/../python_bindings" --platform linux --output-dir $HERE/../wheelhouse