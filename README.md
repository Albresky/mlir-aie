# MLIR-based AIEngine toolchain

![GitHub Workflow Status](https://img.shields.io/github/workflow/status/Xilinx/mlir-aie/Build%20and%20Test)

This repository contains an [MLIR-based](https://mlir.llvm.org/) toolchain for Xilinx Versal AIEngine-based devices.  This can be used to generate low-level configuration for the AIEngine portion of the device, including processors, stream switches, TileDMA and ShimDMA blocks. Backend code generation is included, targetting the LibXAIE library.  This project is primarily intended to support tool builders with convenient low-level access to devices and enable the development of a wide variety of programming models from higher level abstractions.  As such, although it contains some examples, this project is not intended to represent end-to-end compilation flows or to be particularly easy to use for system design.

[Full Documentation](https://xilinx.github.io/mlir-aie/)

[Building the code](docs/Building.md)

-----
 (c) Copyright 2019-2021 Xilinx Inc.
