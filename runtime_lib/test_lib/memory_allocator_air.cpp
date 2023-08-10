//===- memory_allocator_air.cpp ---------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

#include "memory_allocator.h"
#ifdef __x86_64__
#include "air_host.h"
#endif
#include <iostream>

//
// This memory allocator links against the air allocator and is used for vck5000
//
int *mlir_aie_mem_alloc(ext_mem_model_t &handle, int size) {
#ifdef __x86_64__
  int size_bytes = size * sizeof(int);

  handle.virtualAddr = air_dev_mem_alloc(size_bytes);
  if (handle.virtualAddr) {
    handle.size = size_bytes;
    handle.physicalAddr = air_dev_mem_get_pa(handle.virtualAddr);
  } else {
    printf("ExtMemModel: Failed to allocate %d memory.\n", size_bytes);
  }

  std::cout << "ExtMemModel constructor: virtual address " << std::hex
            << handle.virtualAddr << ", physical address "
            << handle.physicalAddr << ", size " << std::dec << handle.size
            << std::endl;

  return (int *)handle.virtualAddr;
#elif
  return NULL;
#endif
}

/*
  The device memory allocator directly maps device memory over
  PCIe MMIO. These accesses are uncached and thus don't require
  explicit synchronization between the host and device
*/
void mlir_aie_sync_mem_cpu(ext_mem_model_t &handle) {}

void mlir_aie_sync_mem_dev(ext_mem_model_t &handle) {}
