##===- Makefile -----------------------------------------------------------===##
# 
# This file licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Copyright (C) 2024, Advanced Micro Devices, Inc.
# 
##===----------------------------------------------------------------------===##

srcdir := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))

include ${srcdir}/../../makefile-common

all: build/final.xclbin

targetname = relu
trace_size = 8192

aie_py_src=relu.py
use_alt?=0

CHESS ?= false

ifeq (${use_alt}, 1)
aie_py_src=relu_alt.py
endif

VPATH := ${srcdir}/../../../aie_kernels/aie2

build/%.o: %.cc
	mkdir -p ${@D}
ifeq ($(CHESS), true)
	cd ${@D} && xchesscc_wrapper ${CHESSCCWRAP2_FLAGS} -c $< -o ${@F}; 
else 
	cd ${@D} && ${PEANO_INSTALL_DIR}/bin/clang++ ${PEANOWRAP2_FLAGS} -c $< -o ${@F}
endif

build/aie.mlir: ${srcdir}/${aie_py_src}
	mkdir -p ${@D}
	python3 $< > $@

build/aie_trace.mlir: ${srcdir}/${aie_py_src}
	mkdir -p ${@D}
	python3 $< ${trace_size} > $@

build/final.xclbin: build/aie.mlir build/relu.o
	mkdir -p ${@D}
	cd ${@D} && aiecc.py --aie-generate-cdo --aie-generate-npu --no-compile-host \
	--no-xchesscc --no-xbridge \
		--xclbin-name=${@F} --npu-insts-name=insts.txt ${<F}

build/final_trace.xclbin: build/aie_trace.mlir build/relu.o
	mkdir -p ${@D}
ifeq ($(CHESS), true)
	cd ${@D} && aiecc.py --aie-generate-cdo --aie-generate-npu --no-compile-host \
		--xclbin-name=${@F} --npu-insts-name=insts.txt ${<F}
else
	cd ${@D} && aiecc.py --aie-generate-cdo --aie-generate-npu --no-compile-host \
	--no-xchesscc --no-xbridge \
		--xclbin-name=${@F} --npu-insts-name=insts.txt ${<F}
endif

${targetname}.exe: ${srcdir}/test.cpp
	rm -rf _build
	mkdir -p _build
	cd _build && ${powershell} cmake ${srcdir} -DTARGET_NAME=${targetname}
	cd _build && ${powershell} cmake --build . --config Release
ifeq "${powershell}" "powershell.exe"
	cp _build/${targetname}.exe $@
else
	cp _build/${targetname} $@ 
endif

run: ${targetname}.exe build/final.xclbin
	${powershell} ./$< -x build/final.xclbin -i build/insts.txt -k MLIR_AIE

trace: ${targetname}.exe build/final_trace.xclbin
	${powershell} ./$< -x build/final_trace.xclbin -i build/insts.txt -k MLIR_AIE -t ${trace_size}
	../../utils/parse_trace.py --filename trace.txt --mlir build/aie_trace.mlir --colshift 1 > trace_relu.json

clean_trace:
	rm -rf trace.txt trace_relu.json

clean: clean_trace
	rm -rf build _build ${targetname}.exe
