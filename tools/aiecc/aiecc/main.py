"""
aiecc - AIE compiler driver for MLIR tools
"""

import itertools
import os
import platform
import sys
import time
from subprocess import PIPE, run, call
import tempfile

import aiecc.cl_arguments


def main(builtin_params={}):
    opts = aiecc.cl_arguments.parse_args()
    is_windows = platform.system() == 'Windows'

    sys.stderr.write('\ncompiling %s\n' % opts.filename)

    thispath = os.path.dirname(os.path.realpath(__file__))
    me_basic_o = os.path.join(thispath, '..','..','..','runtime_lib', 'me_basic.o')

    # with tempfile.TemporaryDirectory() as tmpdirname:
    tmpdirname = "acdc_project"
    try:
        os.mkdir(tmpdirname)
    except FileExistsError:
        pass

    print('created temporary directory', tmpdirname)

    file_with_addresses = os.path.join(tmpdirname, 'input_with_addresses.mlir')
    call(['aie-opt', '--aie-assign-buffer-addresses', '-convert-scf-to-std', opts.filename, '-o', file_with_addresses])
    t = run(['aie-translate', '--aie-generate-corelist', file_with_addresses], stdout=PIPE, stderr=PIPE, universal_newlines=True)
    cores = eval(t.stdout)

    def corefile(core, ext):
        (corecol, corerow) = core
        return os.path.join(tmpdirname, 'core_%d_%d.%s' % (corecol, corerow, ext))

    for core in cores:
        (corecol, corerow) = core
        file_core = corefile(core, "mlir")
        call(['aie-opt', '--aie-llvm-lowering=tilecol=%d tilerow=%d' % core, file_with_addresses, '-o', file_core])
        file_core_bcf = corefile(core, "bcf")
        call(['aie-translate', file_with_addresses, '--aie-generate-bcf', '--tilecol=%d' % corecol, '--tilerow=%d' % corerow, '-o', file_core_bcf])
        file_core_ldscript = corefile(core, "ld.script")
        call(['aie-translate', file_with_addresses, '--aie-generate-ldscript', '--tilecol=%d' % corecol, '--tilerow=%d' % corerow, '-o', file_core_ldscript])
        file_core_llvmir = corefile(core, "ll")
        call(['aie-translate', '--aie-generate-llvmir', file_core, '-o', file_core_llvmir])
        file_core_llvmir_stripped = corefile(core, "stripped.ll")
        call(['opt', '-strip', '-S', file_core_llvmir, '-o', file_core_llvmir_stripped])
        file_core_elf = corefile(core, "elf")
        if(opts.xbridge == True):
          file_core_obj = corefile(core, "o")
          call(['llc', file_core_llvmir_stripped, '-O2', '--march=aie', '--filetype=obj', '-o', file_core_obj])
          call(['xbridge', file_core_obj, '-c', file_core_bcf, '-o', file_core_elf])
        else:
          call(['clang', '-O2', '--target=aie', file_core_llvmir_stripped, me_basic_o, '-Wl,-T,'+file_core_ldscript, '-o', file_core_elf])

    # Generate the included host interface
    file_physical = os.path.join(tmpdirname, 'input_physical.mlir')
    call(['aie-opt', '--aie-create-flows', file_with_addresses, '-o', file_physical]);
    file_inc_cpp = os.path.join(tmpdirname, 'aie_inc.cpp')
    call(['aie-translate', '--aie-generate-xaie', file_physical, '-o', file_inc_cpp])