//===- init_values_shared_mem_test.mlir -------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (C) 2025, Advanced Micro Devices, Inc.
// 
//===----------------------------------------------------------------------===//

// RUN: aie-opt --aie-objectFifo-stateful-transform %s | FileCheck %s

// CHECK: module @init_shared_mem {
// CHECK:   aie.device(xcve2302) {
// CHECK:     memref.global "public" @of1 : memref<2x2xi32>
// CHECK:     memref.global "public" @of0 : memref<2x2xi32>
// CHECK:     %{{.*}}tile_1_2 = aie.tile(1, 2)
// CHECK:     %{{.*}}tile_1_3 = aie.tile(1, 3)
// CHECK:     %of1_buff_0 = aie.buffer(%{{.*}}tile_1_3) {sym_name = "of1_buff_0"} : memref<2x2xi32> = dense<{{\[}}[0, 1], [2, 3]]>
// CHECK:     %of1_buff_1 = aie.buffer(%{{.*}}tile_1_3) {sym_name = "of1_buff_1"} : memref<2x2xi32> = dense<{{\[}}[4, 5], [6, 7]]>
// CHECK:     %of1_prod_lock = aie.lock(%{{.*}}tile_1_3, 0) {init = 0 : i32, sym_name = "of1_prod_lock"}
// CHECK:     %of1_cons_lock = aie.lock(%{{.*}}tile_1_3, 1) {init = 2 : i32, sym_name = "of1_cons_lock"}
// CHECK:     %of0_buff_0 = aie.buffer(%{{.*}}tile_1_2) {sym_name = "of0_buff_0"} : memref<2x2xi32> = dense<{{\[}}[0, 1], [2, 3]]>
// CHECK:     %of0_buff_1 = aie.buffer(%{{.*}}tile_1_2) {sym_name = "of0_buff_1"} : memref<2x2xi32> = dense<{{\[}}[4, 5], [6, 7]]>
// CHECK:     %of0_prod_lock = aie.lock(%{{.*}}tile_1_2, 0) {init = 0 : i32, sym_name = "of0_prod_lock"}
// CHECK:     %of0_cons_lock = aie.lock(%{{.*}}tile_1_2, 1) {init = 2 : i32, sym_name = "of0_cons_lock"}
// CHECK:   }
// CHECK: }

module @init_shared_mem {
 aie.device(xcve2302) {
    %tile12 = aie.tile(1, 2)
    %tile13 = aie.tile(1, 3)

    aie.objectfifo @of0 (%tile12, {%tile13}, 2 : i32) : !aie.objectfifo<memref<2x2xi32>> = [dense<[[0, 1], [2, 3]]> : memref<2x2xi32>, 
                                                                                            dense<[[4, 5], [6, 7]]> : memref<2x2xi32>]

    aie.objectfifo @of1 (%tile12, {%tile13}, 2 : i32) {via_shared_mem = 1 : i32} : !aie.objectfifo<memref<2x2xi32>> = [dense<[[0, 1], [2, 3]]> : memref<2x2xi32>, 
                                                                                                                       dense<[[4, 5], [6, 7]]> : memref<2x2xi32>]
 }
}
