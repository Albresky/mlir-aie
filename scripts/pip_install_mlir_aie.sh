#!/usr/bin/env bash
set -xe

SITE_PACKAGES=$(python -c "import site; print(site.getsitepackages()[0])")

if [ ! -z "$MLIR_AIE_COMMIT" ]; then
  MLIR_AIE_COMMIT="==$MLIR_AIE_COMMIT"
fi

if [ x"$CIBW_ARCHS" == x"arm64" ] || [ x"$CIBW_ARCHS" == x"aarch64" ]; then
  if [ x"$MATRIX_OS" == x"macos-11" ] && [ x"$CIBW_ARCHS" == x"arm64" ]; then
    PLAT=macosx_11_0_arm64
  elif [ x"$MATRIX_OS" == x"ubuntu-20.04" ] && [ x"$CIBW_ARCHS" == x"aarch64" ]; then
    PLAT=linux_aarch64
  fi
  pip install mlir-aie$MLIR_AIE_COMMIT --platform $PLAT --only-binary=:all: --target $SITE_PACKAGES  --no-deps --force -U
else
  pip install mlir-aie$MLIR_AIE_COMMIT --force -U
fi
