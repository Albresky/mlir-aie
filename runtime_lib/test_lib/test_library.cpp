//===- test_library.cpp -----------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

/// \file
/// This file contains common libraries used for testing. Many of these
/// functions are relatively thin wrappers around underlying libXAIE call and
/// are provided to expose a relatively consistent API.  Others are more
/// complex.

#include "test_library.h"
#include "math.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <vector>

#define SYSFS_PATH_MAX 63

extern "C" {
extern aie_libxaie_ctx_t *ctx /* = nullptr*/;

const char vck5000_driver_name[] = "/dev/amdair";
}

// namespace aie_device {
//}

/// @brief  Release access to the libXAIE context.
/// @param ctx The context
void mlir_aie_deinit_libxaie(aie_libxaie_ctx_t *ctx) {
  AieRC RC = XAie_Finish(&(ctx->DevInst));
  if (RC != XAIE_OK) {
    printf("Failed to finish tiles.\n");
  }
  free(ctx);
}

/// @brief Initialize the device represented by the context.
/// @param ctx The context
/// @return Zero on success
int mlir_aie_init_device(aie_libxaie_ctx_t *ctx, uint32_t device_id) {
  AieRC RC = XAIE_OK;

#ifdef __x86_64__

  // Want to get rid of this but need it right now -- Need to clean up mlir-air
  // to remove this
  hsa_status_t hsa_ret = air_get_physical_devices();

  // Discovering agents
  std::vector<air_agent_t> agents;
  auto get_agents_ret = air_get_agents(agents);

  if (get_agents_ret != HSA_STATUS_SUCCESS || agents.empty()) {
    printf("No agents found. Exiting.\n");
    return -1;
  }

  // Creating a queue on the first agent that we see
  queue_t *q = nullptr;
  auto create_queue_ret = air_queue_create(MB_QUEUE_SIZE, HSA_QUEUE_TYPE_SINGLE,
                                           &q, agents[0].handle, device_id);
  if (create_queue_ret != 0) {
    printf("Failed to create queue. Exiting\n");
    return -1;
  }

  // Initializing the device
  uint64_t wr_idx = queue_add_write_index(q, 1);
  uint64_t packet_id = wr_idx % q->size;
  dispatch_packet_t *shim_pkt =
      (dispatch_packet_t *)(q->base_address_vaddr) + packet_id;
  air_packet_device_init(shim_pkt, 50);
  air_queue_dispatch_and_wait(q, wr_idx, shim_pkt);

  // Attaching the queue to the context so we can send more packets if needed
  ctx->cmd_queue = q;

  // Creating the sysfs path to issue read/write 32 commands
  char sysfs_path[SYSFS_PATH_MAX + 1];
  if (snprintf(sysfs_path, SYSFS_PATH_MAX, "/sys/class/amdair/amdair/%02u",
               device_id) == SYSFS_PATH_MAX)
    sysfs_path[SYSFS_PATH_MAX] = 0;

  int fda;
  if ((fda = open(vck5000_driver_name, O_RDWR | O_SYNC)) == -1) {
    printf("[ERROR] %s failed to open %s\n", __func__, vck5000_driver_name);
    return -1;
  }

  // Using the AMDAIR libxaie backend, which utilizes the AMDAIR driver
  XAie_BackendType backend;
  ctx->AieConfigPtr.Backend = XAIE_IO_BACKEND_AMDAIR;
  backend = XAIE_IO_BACKEND_AMDAIR;
  ctx->AieConfigPtr.BaseAddr = 0;
  ctx->DevInst.IOInst = (void *)sysfs_path;

  // Initialize the device memory allocator
  air_init_dev_mem_allocator(0x20000);
#endif

  RC = XAie_CfgInitialize(&(ctx->DevInst), &(ctx->AieConfigPtr));
  if (RC != XAIE_OK) {
    printf("Driver initialization failed.\n");
    return -1;
  }

  // Without this special case, the simulator generates
  // FATAL::[ xtlm::907 ] b_transport_cb is not registered with the utils
  const XAie_Backend *Backend = ctx->DevInst.Backend;
  if (Backend->Type != XAIE_IO_BACKEND_SIM) {
    RC = XAie_PmRequestTiles(&(ctx->DevInst), NULL, 0);
    if (RC != XAIE_OK) {
      printf("Failed to request tiles.\n");
      return -1;
    }

    // TODO Extra code to really teardown the partitions
    RC = XAie_Finish(&(ctx->DevInst));
    if (RC != XAIE_OK) {
      printf("Failed to finish tiles.\n");
      return -1;
    }

#ifdef __x86_64__
    // Because we tear this down, need to do it again
    ctx->AieConfigPtr.Backend = XAIE_IO_BACKEND_AMDAIR;
    backend = XAIE_IO_BACKEND_AMDAIR;
    ctx->AieConfigPtr.BaseAddr = 0;
    ctx->DevInst.IOInst = (void *)sysfs_path;
#endif

    RC = XAie_CfgInitialize(&(ctx->DevInst), &(ctx->AieConfigPtr));
    if (RC != XAIE_OK) {
      printf("Driver initialization failed.\n");
      return -1;
    }
    RC = XAie_PmRequestTiles(&(ctx->DevInst), NULL, 0);
    if (RC != XAIE_OK) {
      printf("Failed to request tiles.\n");
      return -1;
    }
  }

  if (Backend->Type == XAIE_IO_BACKEND_SIM) {
    printf("Turning ecc off\n");
    XAie_TurnEccOff(&(ctx->DevInst));
  }

  return 0;
}

/// @brief Acquire a physical lock
/// @param ctx The context
/// @param col The column of the lock
/// @param row The row of the lock
/// @param lockid The ID of the lock in the tile.
/// @param lockval The value to acquire the lock with.
/// @param timeout The number of microseconds to wait
/// @return Return non-zero on success, i.e. the operation did not timeout.
int mlir_aie_acquire_lock(aie_libxaie_ctx_t *ctx, int col, int row, int lockid,
                          int lockval, int timeout) {
  return (XAie_LockAcquire(&(ctx->DevInst), XAie_TileLoc(col, row),
                           XAie_LockInit(lockid, lockval), timeout) == XAIE_OK);
}

/// @brief Release a physical lock
/// @param ctx The context
/// @param col The column of the lock
/// @param row The row of the lock
/// @param lockid The ID of the lock in the tile.
/// @param lockval The value to acquire the lock with.
/// @param timeout The number of microseconds to wait
/// @return Return non-zero on success, i.e. the operation did not timeout.
int mlir_aie_release_lock(aie_libxaie_ctx_t *ctx, int col, int row, int lockid,
                          int lockval, int timeout) {
  return (XAie_LockRelease(&(ctx->DevInst), XAie_TileLoc(col, row),
                           XAie_LockInit(lockid, lockval), timeout) == XAIE_OK);
}

/// @brief Read the AIE configuration memory at the given physical address.
u32 mlir_aie_read32(aie_libxaie_ctx_t *ctx, u64 addr) {
  u32 val;
  XAie_Read32(&(ctx->DevInst), addr, &val);
  return val;
}

/// @brief Write the AIE configuration memory at the given physical address.
/// It's almost always better to use some more indirect method of accessing
/// configuration registers, but this is provided as a last resort.
void mlir_aie_write32(aie_libxaie_ctx_t *ctx, u64 addr, u32 val) {
  XAie_Write32(&(ctx->DevInst), addr, val);
}

/// @brief Read a value from the data memory of a particular tile memory
/// @param addr The address in the given tile.
/// @return The data
u32 mlir_aie_data_mem_rd_word(aie_libxaie_ctx_t *ctx, int col, int row,
                              u64 addr) {
  u32 data;
  XAie_DataMemRdWord(&(ctx->DevInst), XAie_TileLoc(col, row), addr, &data);
  return data;
}

/// @brief Write a value to the data memory of a particular tile memory
/// @param addr The address in the given tile.
/// @param data The data
void mlir_aie_data_mem_wr_word(aie_libxaie_ctx_t *ctx, int col, int row,
                               u64 addr, u32 data) {
  XAie_DataMemWrWord(&(ctx->DevInst), XAie_TileLoc(col, row), addr, data);
}

/// @brief Return the base address of the given tile.
/// The configuration address space of most tiles is very similar,
/// relative to this base address.
u64 mlir_aie_get_tile_addr(aie_libxaie_ctx_t *ctx, int col, int row) {
  return _XAie_GetTileAddr(&(ctx->DevInst), row, col);
}

/// @brief Dump the tile memory of the given tile
/// Values that are zero are not shown
void mlir_aie_dump_tile_memory(aie_libxaie_ctx_t *ctx, int col, int row) {
  for (int i = 0; i < 0x2000; i++) {
    uint32_t d;
    AieRC rc = XAie_DataMemRdWord(&(ctx->DevInst), XAie_TileLoc(col, row),
                                  (i * 4), &d);
    if (rc == XAIE_OK && d != 0)
      printf("Tile[%d][%d]: mem[%d] = %d\n", col, row, i, d);
  }
}

/// @brief Fill the tile memory of the given tile with zeros.
/// Values that are zero are not shown
void mlir_aie_clear_tile_memory(aie_libxaie_ctx_t *ctx, int col, int row) {
  for (int i = 0; i < 0x2000; i++) {
    XAie_DataMemWrWord(&(ctx->DevInst), XAie_TileLoc(col, row), (i * 4), 0);
  }
}

static void print_aie2_dmachannel_status(aie_libxaie_ctx_t *ctx, int col,
                                         int row, const char *dmatype,
                                         const char *channel, int channelNum,
                                         u32 statusOffset, u32 controlOffset,
                                         int &current_bd) {
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);
  u32 status, control;
  XAie_Read32(&(ctx->DevInst), tileAddr + statusOffset, &status);
  XAie_Read32(&(ctx->DevInst), tileAddr + controlOffset, &control);
  u32 running = status & 0x3;
  u32 stalled_acq = (status >> 2) & 0x1;
  u32 stalled_rel = (status >> 3) & 0x1;
  u32 stalled_data = (status >> 4) & 0x1;
  u32 stalled_complete = (status >> 5) & 0x1;
  current_bd = status >> 24;
  printf("%s [%d, %d] AIE2 %s%d ", dmatype, col, row, channel, channelNum);
  switch (running) {
  case 0:
    printf("IDLE ");
    break;
  case 1:
    printf("STARTING ");
    break;
  case 2:
    printf("RUNNING ");
    break;
  }
  if (stalled_acq)
    printf("Stalled on Acquire ");
  if (stalled_rel)
    printf("Stalled on Release ");
  if (stalled_data)
    printf("Stalled on Data ");
  if (stalled_complete)
    printf("Stalled on Completion ");
  printf("status:%08X ctrl:%02X\n", status, control);
};

static void print_bd(int bd, int bd_valid, u32 nextBd, u32 useNextBd,
                     int isPacket, u32 packetID, u32 packetType,
                     int words_to_transfer, int base_address,
                     int acquireEnabled, u32 acquireLock, int acquireValue,
                     int releaseEnabled, u32 releaseLock, int releaseValue,
                     int s2mm_current_bd[], int mm2s_current_bd[],
                     int numchannels) {

  if (bd_valid) {
    printf("BD %d valid ", bd);
    if (useNextBd)
      printf("(Next BD: %d)\n", nextBd);
    else
      printf("(Last BD)\n");

    for (int i = 0; i < numchannels; i++) {
      if (bd == s2mm_current_bd[i]) {
        printf(" * Current BD for s2mm channel %d\n", i);
      }
      if (bd == mm2s_current_bd[i]) {
        printf(" * Current BD for mm2s channel %d\n", i);
      }
    }

    if (isPacket) {
      printf("   Packet ID: %02X\n", packetID);
      printf("   Packet Type: %01X\n", packetType);
    }
    printf("   Transferring %d 32 bit words to/from byte address %06X\n",
           words_to_transfer, base_address * 4);

    // printf("   ");
    // for (int w = 0; w < 7; w++) {
    //   u32 tmpd;
    //   XAie_DataMemRdWord(&(ctx->DevInst), XAie_TileLoc(col, row),
    //                     (base_address + w) * 4, &tmpd);
    //   printf("%08X ", tmpd);
    // }
    // printf("\n");
    if (acquireEnabled) { // acquire is enabled
      printf("   Acquires lock %d ", acquireLock);
      printf("with value %d\n", acquireValue);
    }
    if (releaseEnabled) {
      printf("   Releases lock %d ", releaseLock);
      printf("with value %d\n", releaseValue);
    }
    // printf("currently ");
    // u32 locks;
    // XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001EF00, &locks);
    // u32 two_bits = (locks >> (lock_id * 2)) & 0x3;
    // if (two_bits) {
    //   u32 acquired = two_bits & 0x1;
    //   u32 value = two_bits & 0x2;
    //   if (acquired)
    //     printf("Acquired ");
    //   printf(value ? "1" : "0");
    // } else
    //   printf("0");
    // }
  }
}

/// @brief Print a summary of the status of the given Tile DMA.
void mlir_aie_print_dma_status(aie_libxaie_ctx_t *ctx, int col, int row) {
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);
  auto TileType = ctx->DevInst.DevOps->GetTTypefromLoc(&(ctx->DevInst),
                                                       XAie_TileLoc(col, row));
  assert(TileType == XAIEGBL_TILE_TYPE_AIETILE);

  if (ctx->AieConfigPtr.AieGen == XAIE_DEV_GEN_AIEML) {
    const int num_bds = 2;
    int s2mm_current_bd[num_bds];
    int mm2s_current_bd[num_bds];

    for (int i = 0; i < num_bds; i++) {
      print_aie2_dmachannel_status(ctx, col, row, "DMA", "s2mm", i,
                                   0x0001DF00 + 4 * i, 0x0001DE00 + 8 * i,
                                   s2mm_current_bd[i]);
      u32 write_count;
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D230 + (0x4 * i),
                  &write_count);
      printf("DMA [%d, %d] s2mm%d write_count = %d\n", col, row, i,
             write_count);
    }
    for (int i = 0; i < num_bds; i++)
      print_aie2_dmachannel_status(ctx, col, row, "DMA", "mm2s", i,
                                   0x0001DF10 + 4 * i, 0x0001DE10 + 8 * i,
                                   mm2s_current_bd[i]);

    for (int bd = 0; bd < 8; bd++) {
      u32 dma_bd_addr;
      u32 dma_bd_packet;
      u32 dma_bd_control;
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D000 + (0x20 * bd),
                  &dma_bd_addr);
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D004 + (0x20 * bd),
                  &dma_bd_packet);
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D014 + (0x20 * bd),
                  &dma_bd_control);

      int bd_valid = (dma_bd_control >> 25) & 0x1;
      u32 nextBd = ((dma_bd_control >> 27) & 0xF);
      u32 useNextBd = ((dma_bd_control >> 26) & 0x1);
      int isPacket = (dma_bd_packet >> 30) & 0x1;
      u32 packetID = (dma_bd_packet >> 19) & 0x1F;
      u32 packetType = (dma_bd_packet >> 16) & 0x7;
      int words_to_transfer = (dma_bd_addr & 0x3FFF);
      int base_address = dma_bd_addr >> 14;
      int acquireEnabled = (dma_bd_control >> 12) & 0x1;
      u32 acquireLock = dma_bd_control & 0xf;
      int acquireValue = (((int)dma_bd_control << 20) >> 25);
      u32 releaseLock = (dma_bd_control >> 13) & 0xf;
      int releaseValue = (((int)dma_bd_control << 7) >> 25);
      int releaseEnabled = releaseValue != 0;

      print_bd(bd, bd_valid, nextBd, useNextBd, isPacket, packetID, packetType,
               words_to_transfer, base_address, acquireEnabled, acquireLock,
               acquireValue, releaseEnabled, releaseLock, releaseValue,
               s2mm_current_bd, mm2s_current_bd, num_bds);
    }
  } else { // AIE1
    u32 dma_mm2s_status;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001DF10, &dma_mm2s_status);
    u32 dma_s2mm_status;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001DF00, &dma_s2mm_status);
    u32 dma_mm2s0_control;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001DE10, &dma_mm2s0_control);
    u32 dma_mm2s1_control;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001DE18, &dma_mm2s1_control);
    u32 dma_s2mm0_control;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001DE00, &dma_s2mm0_control);
    u32 dma_s2mm1_control;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001DE08, &dma_s2mm1_control);

    u32 s2mm_ch0_running = dma_s2mm_status & 0x3;
    u32 s2mm_ch1_running = (dma_s2mm_status >> 2) & 0x3;
    u32 mm2s_ch0_running = dma_mm2s_status & 0x3;
    u32 mm2s_ch1_running = (dma_mm2s_status >> 2) & 0x3;
    int s2mm0_current_bd, s2mm1_current_bd;
    int mm2s0_current_bd, mm2s1_current_bd;
    s2mm0_current_bd = (dma_s2mm_status >> 16) & 0xf;
    s2mm1_current_bd = (dma_s2mm_status >> 20) & 0xf;
    mm2s0_current_bd = (dma_mm2s_status >> 16) & 0xf;
    mm2s1_current_bd = (dma_mm2s_status >> 20) & 0xf;

    printf("DMA [%d, %d] mm2s_status/0ctrl/1ctrl is %08X %02X %02X, "
           "s2mm_status/0ctrl/1ctrl is %08X %02X %02X\n",
           col, row, dma_mm2s_status, dma_mm2s0_control, dma_mm2s1_control,
           dma_s2mm_status, dma_s2mm0_control, dma_s2mm1_control);
    for (int bd = 0; bd < 8; bd++) {
      u32 dma_bd_addr_a;
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D000 + (0x20 * bd),
                  &dma_bd_addr_a);
      u32 dma_bd_control;
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D018 + (0x20 * bd),
                  &dma_bd_control);
      // It appears that in the simulator, bd's are not initialized according to
      // the spec, and instead the control word is all 1's.
      if ((dma_bd_control >> 31) & 0x1 && (dma_bd_control != 0xFFFFFFFF)) {
        printf("BD %d valid ", bd);
        u32 nextBd = ((dma_bd_control >> 13) & 0xF);
        u32 useNextBd = ((dma_bd_control >> 17) & 0x1);
        if (useNextBd)
          printf("(Next BD: %d)\n", nextBd);
        else
          printf("(Last BD)\n");

        if (bd == s2mm0_current_bd) {
          printf(" * Current BD for s2mm channel 0\n");
        }
        if (bd == s2mm1_current_bd) {
          printf(" * Current BD for s2mm channel 1\n");
        }
        if (bd == mm2s0_current_bd) {
          printf(" * Current BD for mm2s channel 0\n");
        }
        if (bd == mm2s1_current_bd) {
          printf(" * Current BD for mm2s channel 1\n");
        }

        if (dma_bd_control & 0x08000000) {
          u32 dma_packet;
          XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D010 + (0x20 * bd),
                      &dma_packet);
          printf("   Packet mode: %02X\n", dma_packet & 0x1F);
        }
        int words_to_transfer = 1 + (dma_bd_control & 0x1FFF);
        int base_address = dma_bd_addr_a & 0x1FFF;
        printf("   Transferring %d 32 bit words to/from byte address %06X\n",
               words_to_transfer, base_address * 4);

        printf("   ");
        for (int w = 0; w < 7; w++) {
          u32 tmpd;
          XAie_DataMemRdWord(&(ctx->DevInst), XAie_TileLoc(col, row),
                             (base_address + w) * 4, &tmpd);
          printf("%08X ", tmpd);
        }
        printf("\n");
        int hasAcquire = (dma_bd_addr_a >> 18) & 0x1;
        int hasRelease = (dma_bd_addr_a >> 21) & 0x1;
        if (hasAcquire || hasRelease) {
          u32 lock_id = (dma_bd_addr_a >> 22) & 0xf;
          if (hasAcquire) {
            printf("   Acquires lock %d ", lock_id);
            if ((dma_bd_addr_a >> 16) & 0x1)
              printf("with value %d ", (dma_bd_addr_a >> 17) & 0x1);
          }
          if (hasRelease) {
            printf("   Releases lock %d ", lock_id);
            if ((dma_bd_addr_a >> 19) & 0x1)
              printf("with value %d ", (dma_bd_addr_a >> 20) & 0x1);
          }

          printf("currently ");
          u32 locks;
          XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001EF00, &locks);
          u32 two_bits = (locks >> (lock_id * 2)) & 0x3;
          if (two_bits) {
            u32 acquired = two_bits & 0x1;
            u32 value = two_bits & 0x2;
            if (acquired)
              printf("Acquired ");
            printf(value ? "1" : "0");
          } else
            printf("0");
          printf("\n");
        }

        if (dma_bd_control & 0x30000000) { // FIFO MODE
          int FIFO = (dma_bd_control >> 28) & 0x3;
          u32 dma_fifo_counter;
          XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001DF20,
                      &dma_fifo_counter);
          printf("   Using FIFO Cnt%d : %08X\n", FIFO, dma_fifo_counter);
        }
      }
    }
  }
}

void print_aie2_lock_status(aie_libxaie_ctx_t *ctx, int col, int row,
                            const char *type, int lockOffset, int locks) {
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);
  printf("%s [%d, %d] AIE2 locks are: ", type, col, row);
  int lockAddr = tileAddr + lockOffset;
  for (int lock = 0; lock < locks; lock++) {
    u32 val;
    XAie_Read32(&(ctx->DevInst), lockAddr, &val);
    printf("%X ", val);
    lockAddr += 0x10;
  }
  printf("\n");
}

/// @brief Print a summary of the status of the given MemTile DMA.
void mlir_aie_print_memtiledma_status(aie_libxaie_ctx_t *ctx, int col,
                                      int row) {
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);
  auto TileType = ctx->DevInst.DevOps->GetTTypefromLoc(&(ctx->DevInst),
                                                       XAie_TileLoc(col, row));
  assert(TileType == XAIEGBL_TILE_TYPE_MEMTILE);
  assert(ctx->AieConfigPtr.AieGen == XAIE_DEV_GEN_AIEML);

  int s2mm_current_bd[6];
  int mm2s_current_bd[6];

  for (int i = 0; i < 6; i++) {
    print_aie2_dmachannel_status(ctx, col, row, "MemTileDMA", "s2mm", i,
                                 0x000A0660 + 4 * i, 0x000A0600 + 8 * i,
                                 s2mm_current_bd[i]);
    u32 write_count;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000A06B0 + (0x4 * i),
                &write_count);
    printf("MemTileDMA [%d, %d] s2mm%d write_count = %d\n", col, row, i,
           write_count);
  }
  for (int i = 0; i < 6; i++)
    print_aie2_dmachannel_status(ctx, col, row, "MemTileDMA", "mm2s", i,
                                 0x000A0680 + 4 * i, 0x000A0630 + 8 * i,
                                 mm2s_current_bd[i]);

  print_aie2_lock_status(ctx, col, row, "MemTileDMA", 0x000C0000, 64);

  for (int bd = 0; bd < 8; bd++) {
    u32 dma_bd_0;
    u32 dma_bd_1;
    u32 dma_bd_7;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000A0000 + (0x20 * bd),
                &dma_bd_0);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000A0004 + (0x20 * bd),
                &dma_bd_1);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000A001C + (0x20 * bd),
                &dma_bd_7);

    int bd_valid = (dma_bd_7 >> 31) & 0x1;
    u32 nextBd = ((dma_bd_1 >> 20) & 0xF);
    u32 useNextBd = ((dma_bd_1 >> 19) & 0x1);
    int isPacket = (dma_bd_0 >> 31) & 0x1;
    u32 packetID = (dma_bd_0 >> 23) & 0x1F;
    u32 packetType = (dma_bd_0 >> 28) & 0x7;
    int words_to_transfer = (dma_bd_0 & 0x1FFFF);
    int base_address = dma_bd_1 & 0x7FFFF;
    int acquireEnabled = (dma_bd_7 >> 12) & 0x1;
    u32 acquireLock = dma_bd_7 & 0xff;
    int acquireValue = (((int)dma_bd_7 << 17) >> 25);
    u32 releaseLock = (dma_bd_7 >> 16) & 0xff;
    int releaseValue = (((int)dma_bd_7 << 1) >> 25);
    int releaseEnabled = releaseValue != 0;

    print_bd(bd, bd_valid, nextBd, useNextBd, isPacket, packetID, packetType,
             words_to_transfer, base_address, acquireEnabled, acquireLock,
             acquireValue, releaseEnabled, releaseLock, releaseValue,
             s2mm_current_bd, mm2s_current_bd, 6);
  }
}

/// @brief Print a summary of the status of the given Shim DMA.
void mlir_aie_print_shimdma_status(aie_libxaie_ctx_t *ctx, int col, int row) {
  // int col = loc.Col;
  // int row = loc.Row;
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);
  auto TileType = ctx->DevInst.DevOps->GetTTypefromLoc(&(ctx->DevInst),
                                                       XAie_TileLoc(col, row));
  assert(TileType == XAIEGBL_TILE_TYPE_SHIMNOC);

  const int num_bds = 2;
  int s2mm_current_bd[num_bds];
  int mm2s_current_bd[num_bds];
  if (ctx->AieConfigPtr.AieGen == XAIE_DEV_GEN_AIEML) {
    for (int i = 0; i < num_bds; i++) {
      print_aie2_dmachannel_status(ctx, col, row, "ShimDMA", "s2mm", i,
                                   0x0001D220 + 4 * i, 0x0001D200 + 8 * i,
                                   s2mm_current_bd[i]);
      u32 write_count;
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D230 + (0x4 * i),
                  &write_count);
      printf("ShimDMA [%d, %d] s2mm%d write_count = %d\n", col, row, i,
             write_count);
    }
    for (int i = 0; i < num_bds; i++)
      print_aie2_dmachannel_status(ctx, col, row, "ShimDMA", "mm2s", i,
                                   0x0001D228 + 4 * i, 0x0001D210 + 8 * i,
                                   mm2s_current_bd[i]);
  } else {
    u32 dma_mm2s_status, dma_s2mm_status;
    u32 dma_mm2s0_control, dma_mm2s1_control;
    u32 dma_s2mm0_control, dma_s2mm1_control;
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D164, &dma_mm2s_status);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D160, &dma_s2mm_status);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D150, &dma_mm2s0_control);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D158, &dma_mm2s1_control);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D140, &dma_s2mm0_control);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D148, &dma_s2mm1_control);

    u32 s2mm_ch0_running = dma_s2mm_status & 0x3;
    u32 s2mm_ch1_running = (dma_s2mm_status >> 2) & 0x3;
    u32 mm2s_ch0_running = dma_mm2s_status & 0x3;
    u32 mm2s_ch1_running = (dma_mm2s_status >> 2) & 0x3;
    s2mm_current_bd[0] = (dma_s2mm_status >> 16) & 0xf;
    s2mm_current_bd[1] = (dma_s2mm_status >> 20) & 0xf;
    mm2s_current_bd[0] = (dma_mm2s_status >> 16) & 0xf;
    mm2s_current_bd[1] = (dma_mm2s_status >> 20) & 0xf;

    printf("ShimDMA [%d, %d] AIE1 mm2s_status/0ctrl/1ctrl is %08X %02X %02X, "
           "s2mm_status/0ctrl/1ctrl is %08X %02X %02X\n",
           col, row, dma_mm2s_status, dma_mm2s0_control, dma_mm2s1_control,
           dma_s2mm_status, dma_s2mm0_control, dma_s2mm1_control);
  }

  u32 locks;
  if (ctx->AieConfigPtr.AieGen == XAIE_DEV_GEN_AIEML) {
    print_aie2_lock_status(ctx, col, row, "ShimDMA", 0x00014000, 16);
    int overflowAddr = tileAddr + 0x00014120;
    int underflowAddr = tileAddr + 0x00014128;
    u32 overflow, underflow;
    // XAie_Read32(&(ctx->DevInst), overflowAddr, &overflow);
    // XAie_Read32(&(ctx->DevInst), underflowAddr, &underflow);
    printf(" overflow?:%x underflow?:%x\n", overflow, underflow);
  } else {
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001EF00, &locks);
    printf("ShimDMA [%d, %d] AIE1 locks are %08X\n", col, row, locks);
    for (int lock = 0; lock < 16; lock++) {
      u32 two_bits = (locks >> (lock * 2)) & 0x3;
      if (two_bits) {
        printf("Lock %d: ", lock);
        u32 acquired = two_bits & 0x1;
        u32 value = two_bits & 0x2;
        if (acquired)
          printf("Acquired ");
        printf(value ? "1" : "0");
        printf("\n");
      }
    }
  }

  for (int bd = 0; bd < 8; bd++) {
    int words_to_transfer; // transfer size in 32-bit words
    u64 base_address;      // address in bytes
    bool bd_valid;
    int use_next_bd;
    int next_bd;
    int acquire_lockID, release_lockID;
    int enable_lock_release;
    int lock_release_val;
    int use_release_val;
    int enable_lock_acquire;
    int lock_acquire_val;
    int use_acquire_val;

    if (ctx->AieConfigPtr.AieGen == XAIE_DEV_GEN_AIEML) {
      u32 dma_bd_addr_low;
      u32 dma_bd_buffer_length;
      u32 dma_bd_2;
      u32 dma_bd_7;
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D000 + (0x20 * bd),
                  &dma_bd_buffer_length);
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D004 + (0x20 * bd),
                  &dma_bd_addr_low);
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D008 + (0x20 * bd),
                  &dma_bd_2);
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D01C + (0x20 * bd),
                  &dma_bd_7);
      // printf("test: %d %d %d %d\n", dma_bd_buffer_length, dma_bd_addr_low,
      // dma_bd_2, dma_bd_7);
      words_to_transfer = dma_bd_buffer_length;
      base_address =
          u64(dma_bd_addr_low & 0xFFFC) + (u64(dma_bd_2 & 0xFF) << 32);
      bd_valid = (dma_bd_7 >> 25) & 0x1;
      use_next_bd = ((dma_bd_7 >> 26) & 0x1);
      next_bd = ((dma_bd_7 >> 27) & 0xF);
      acquire_lockID = ((dma_bd_7 >> 0) & 0xF);
      release_lockID = ((dma_bd_7 >> 13) & 0xF);
      lock_release_val = (s32(dma_bd_7) << 7) >> 25; // sign extend
      enable_lock_release = lock_release_val != 0;
      use_release_val = 1;
      lock_acquire_val = (s32(dma_bd_7) << 20) >> 25; // sign extend
      enable_lock_acquire = ((dma_bd_7 >> 12) & 0x1);
      use_acquire_val = 1;
    } else {
      u32 dma_bd_addr_a;
      u32 dma_bd_buffer_length;
      u32 dma_bd_control;
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D000 + (0x14 * bd),
                  &dma_bd_addr_a);
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D004 + (0x14 * bd),
                  &dma_bd_buffer_length);
      XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001D008 + (0x14 * bd),
                  &dma_bd_control);
      words_to_transfer = dma_bd_buffer_length;
      base_address =
          (u64)dma_bd_addr_a + ((u64)((dma_bd_control >> 16) & 0xFFFF) << 32);
      bd_valid = dma_bd_control & 0x1;
      use_next_bd = ((dma_bd_control >> 15) & 0x1);
      next_bd = ((dma_bd_control >> 11) & 0xF);
      release_lockID = acquire_lockID = ((dma_bd_control >> 7) & 0xF);
      enable_lock_release = ((dma_bd_control >> 6) & 0x1);
      lock_release_val = ((dma_bd_control >> 5) & 0x1);
      use_release_val = ((dma_bd_control >> 4) & 0x1);
      enable_lock_acquire = ((dma_bd_control >> 3) & 0x1);
      lock_acquire_val = ((dma_bd_control >> 2) & 0x1);
      use_acquire_val = ((dma_bd_control >> 1) & 0x1);
    }
    bool isPacket = false;
    int packetID = 0;
    int packetType = 0;
    print_bd(bd, bd_valid, next_bd, use_next_bd, isPacket, packetID, packetType,
             words_to_transfer, base_address, enable_lock_acquire,
             acquire_lockID, lock_acquire_val, enable_lock_release,
             release_lockID, lock_release_val, s2mm_current_bd, mm2s_current_bd,
             num_bds);
  }
}

/// @brief Print the status of a core represented by the given tile, at the
/// given coordinates.
void mlir_aie_print_tile_status(aie_libxaie_ctx_t *ctx, int col, int row) {
  // int col = loc.Col;
  // int row = loc.Row;
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);
  u32 status, coreTimerLow, PC, LR, SP, locks, R0, R4;
  u32 trace_status;
  if (ctx->AieConfigPtr.AieGen == XAIE_DEV_GEN_AIEML) {
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x032004, &status);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0340F8, &coreTimerLow);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00031100, &PC);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00031130, &LR);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00031120, &SP);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000340D8, &trace_status);

    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00030C00, &R0);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00030C40, &R4);

  } else {
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x032004, &status);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0340F8, &coreTimerLow);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00030280, &PC);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000302B0, &LR);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000302A0, &SP);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x000140D8, &trace_status);

    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00030000, &R0);
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x00030040, &R4);
  }
  printf("Core [%d, %d] status is %08X, timer is %u, PC is %08X"
         ", LR is %08X, SP is %08X, R0 is %08X,R4 is %08X\n",
         col, row, status, coreTimerLow, PC, LR, SP, R0, R4);
  printf("Core [%d, %d] trace status is %08X\n", col, row, trace_status);

  if (ctx->AieConfigPtr.AieGen == XAIE_DEV_GEN_AIEML) {
    print_aie2_lock_status(ctx, col, row, "Core", 0x0001F000, 16);
  } else {
    XAie_Read32(&(ctx->DevInst), tileAddr + 0x0001EF00, &locks);
    printf("Core [%d, %d] AIE1 locks are %08X\n", col, row, locks);
    for (int lock = 0; lock < 16; lock++) {
      u32 two_bits = (locks >> (lock * 2)) & 0x3;
      if (two_bits) {
        printf("Lock %d: ", lock);
        u32 acquired = two_bits & 0x1;
        u32 value = two_bits & 0x2;
        if (acquired)
          printf("Acquired ");
        printf(value ? "1" : "0");
        printf("\n");
      }
    }
  }

  const char *core_status_strings[] = {"Enabled",
                                       "In Reset",
                                       "Memory Stall S",
                                       "Memory Stall W",
                                       "Memory Stall N",
                                       "Memory Stall E",
                                       "Lock Stall S",
                                       "Lock Stall W",
                                       "Lock Stall N",
                                       "Lock Stall E",
                                       "Stream Stall S",
                                       "Stream Stall W",
                                       "Stream Stall N",
                                       "Stream Stall E",
                                       "Cascade Stall Master",
                                       "Cascade Stall Slave",
                                       "Debug Halt",
                                       "ECC Error",
                                       "ECC Scrubbing",
                                       "Error Halt",
                                       "Core Done"};
  printf("Core Status: ");
  for (int i = 0; i <= 20; i++) {
    if ((status >> i) & 0x1)
      printf("%s ", core_status_strings[i]);
  }
  printf("\n");
}

static void clear_range(XAie_DevInst *devInst, u64 tileAddr, u64 low,
                        u64 high) {
  for (int i = low; i <= high; i += 4) {
    XAie_Write32(devInst, tileAddr + i, 0);
    // int x = XAie_Read32(ctx->DevInst,tileAddr+i);
    // if(x != 0) {
    //   printf("@%x = %x\n", i, x);
    //   XAie_Write32(ctx->DevInst,tileAddr+i, 0);
    // }
  }
}

/// @brief Clear the configuration of the given (non-shim) tile.
/// This includes: clearing the program memory, data memory,
/// DMA descriptors, and stream switch configuration.
void mlir_aie_clear_config(aie_libxaie_ctx_t *ctx, int col, int row) {
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);

  // Put the core in reset first, otherwise bus collisions
  // result in arm bus errors.
  // TODO Check if this works
  XAie_CoreDisable(&(ctx->DevInst), XAie_TileLoc(col, row));

  // Program Memory
  clear_range(&(ctx->DevInst), tileAddr, 0x20000, 0x200FF);
  // TileDMA
  clear_range(&(ctx->DevInst), tileAddr, 0x1D000, 0x1D1F8);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1DE00, 0);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1DE08, 0);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1DE10, 0);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1DE08, 0);
  // Stream Switch master config
  clear_range(&(ctx->DevInst), tileAddr, 0x3F000, 0x3F060);
  // Stream Switch slave config
  clear_range(&(ctx->DevInst), tileAddr, 0x3F100, 0x3F168);
  // Stream Switch slave slot config
  clear_range(&(ctx->DevInst), tileAddr, 0x3F200, 0x3F3AC);

  // TODO Check if this works
  XAie_CoreEnable(&(ctx->DevInst), XAie_TileLoc(col, row));
}

/// @brief Clear the configuration of the given shim tile.
/// This includes: clearing the program memory, data memory,
/// DMA descriptors, and stream switch configuration.
void mlir_aie_clear_shim_config(aie_libxaie_ctx_t *ctx, int col, int row) {
  u64 tileAddr = _XAie_GetTileAddr(&(ctx->DevInst), row, col);

  // ShimDMA
  clear_range(&(ctx->DevInst), tileAddr, 0x1D000, 0x1D13C);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1D140, 0);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1D148, 0);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1D150, 0);
  XAie_Write32(&(ctx->DevInst), tileAddr + 0x1D158, 0);

  // Stream Switch master config
  clear_range(&(ctx->DevInst), tileAddr, 0x3F000, 0x3F058);
  // Stream Switch slave config
  clear_range(&(ctx->DevInst), tileAddr, 0x3F100, 0x3F15C);
  // Stream Switch slave slot config
  clear_range(&(ctx->DevInst), tileAddr, 0x3F200, 0x3F37C);
}

/*
 ******************************************************************************
 * COMMON
 ******************************************************************************
 */

/// @brief Given an array of values, compute and print statistics about those
/// values.
/// @param performance_counter An array of values
/// @param n The number of values
void computeStats(u32 performance_counter[], int n) {
  u32 total_0 = 0;

  for (int i = 0; i < n; i++) {
    total_0 += performance_counter[i];
  }

  float mean_0 = (float)total_0 / n;

  float sdev_0 = 0;

  for (int i = 0; i < n; i++) {
    float x = (float)performance_counter[i] - mean_0;
    sdev_0 += x * x;
  }

  sdev_0 = sqrtf(sdev_0 / n);

  printf("Mean and Standard Devation: %f, %f \n", mean_0, sdev_0);
}
