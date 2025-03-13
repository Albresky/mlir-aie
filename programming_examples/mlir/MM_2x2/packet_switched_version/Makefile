include ../../../../mlir_tutorials/makefile-common


CHESSCC_FLAGS = -f -p me -P ${VITIS_AIE_INCLUDE_DIR} -I ${VITIS_AIETOOLS_DIR}/include
CHESS_FLAGS = -P ${VITIS_AIE_INCLUDE_DIR}

ARCH=x86_64

AIE_RUNTIME_LIB = $(shell realpath $(dir $(shell which aie-opt))/../runtime_lib)
AIE_INSTALL = $(AIE_RUNTIME_LIB)/$(ARCH)
AIECC_FLAGS = --host-target=x86_64-amd-linux-gnu
AIECC_HOST_FLAGS = \
	-L/usr/lib/gcc/x86_64-linux-gnu/11 \
	-Wl,--rpath=/usr/lib/gcc/x86_64-linux-gnu/11 \
    -I$(AIE_INSTALL)/test_lib/include \
    -I${AIE_INSTALL}/xaiengine/include \
    -L$(AIE_INSTALL)/test_lib/lib -ltest_lib 

all: test.elf
	@mkdir -p elf
	@mv *.elf* ./elf
	@cp ./elf/*.elf ./elf/*.elf.map .

kernel.o: ../kernel.cc
	xchesscc ${CHESSCC_FLAGS} -c $<

test.elf: test.cpp aie.mlir kernel.o
	echo "Compiling AIE kernel"
	aiecc.py -j4 --verbose --aiesim $(AIECC_FLAGS) $(word 2,$^) ./$< -o $@ $(AIECC_HOST_FLAGS)

aiesim:
	# check if ./aie.mlir.prj/aiesim.sh exists
	if [ -f ./aie.mlir.prj/aiesim.sh ]; then \
		echo "Running aiesim.sh"; \
		./aie.mlir.prj/aiesim.sh; \
	else \
		echo "aie.mlir.prj/aiesim.sh not found"; \
	fi

clean:
	rm -rf aie.mlir.prj aiesimulator_output *elf core* *log *vcd *exe pl_sample_counts *.o .AIE_SIM_CMD_LINE_OPTIONS
