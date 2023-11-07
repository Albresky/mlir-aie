//===- AIETargets.cpp -------------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

#include "AIETargets.h"

#include "aie/Dialect/ADF/ADFDialect.h"
#include "aie/Dialect/AIE/IR/AIEDialect.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/Dialect/EmitC/IR/EmitC.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/Attributes.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Target/LLVMIR/Import.h"
#include "mlir/Tools/mlir-translate/MlirTranslateMain.h"
#include "mlir/Tools/mlir-translate/Translation.h"

#include "llvm/Support/JSON.h"

using namespace mlir;
using namespace mlir::vector;
using namespace xilinx;
using namespace xilinx::AIE;

static llvm::cl::opt<int>
    tileCol("tilecol", llvm::cl::desc("column coordinate of core to translate"),
            llvm::cl::init(0));
static llvm::cl::opt<int>
    tileRow("tilerow", llvm::cl::desc("row coordinate of core to translate"),
            llvm::cl::init(0));

llvm::json::Value attrToJSON(Attribute &attr) {
  if (auto a = attr.dyn_cast<StringAttr>()) {
    return llvm::json::Value(a.getValue().str());
  } else if (auto arrayAttr = attr.dyn_cast<ArrayAttr>()) {
    llvm::json::Array arrayJSON;
    for (auto a : arrayAttr)
      arrayJSON.push_back(attrToJSON(a));
    return llvm::json::Value(std::move(arrayJSON));
  } else if (auto dictAttr = attr.dyn_cast<DictionaryAttr>()) {
    llvm::json::Object dictJSON;
    for (auto a : dictAttr) {
      auto ident = a.getName();
      auto attr = a.getValue();
      dictJSON[ident.str()] = attrToJSON(attr);
    }
    return llvm::json::Value(std::move(dictJSON));
  } else if (auto intAttr = attr.dyn_cast<IntegerAttr>()) {
    return llvm::json::Value(intAttr.getInt());
  } else
    return llvm::json::Value(std::string(""));
}

namespace xilinx {
namespace AIE {

static void registerDialects(DialectRegistry &registry) {
  registry.insert<xilinx::AIE::AIEDialect>();
  registry.insert<func::FuncDialect>();
  registry.insert<scf::SCFDialect>();
  registry.insert<cf::ControlFlowDialect>();
  registry.insert<DLTIDialect>();
  registry.insert<arith::ArithDialect>();
  registry.insert<math::MathDialect>();
  registry.insert<memref::MemRefDialect>();
  registry.insert<VectorDialect>();
  registry.insert<LLVM::LLVMDialect>();
  registry.insert<emitc::EmitCDialect>();
}

// Output the buffer map for the given buffer operations, with the given offset.
// The offset is different depending on where the buffers are accessed from.
void writeBufferMap(raw_ostream &output, BufferOp buf, int offset) {
  std::string bufName(buf.name().getValue());
  int bufferBaseAddr = getBufferBaseAddress(buf);
  int numBytes = buf.getAllocationSize();
  output << "_symbol " << bufName << " "
         << "0x" << llvm::utohexstr(offset + bufferBaseAddr) << " " << numBytes
         << '\n';
}
// Output the memorymap in BCF format for the given buffer operations, with the
// given offset. The offset is different depending on where the buffers are
// accessed from.
void writeBCFMap(raw_ostream &output, BufferOp buf, int offset) {
  std::string bufName(buf.name().getValue());
  int bufferBaseAddr = getBufferBaseAddress(buf);
  int numBytes = buf.getAllocationSize();
  output << "_symbol " << bufName << " "
         << "0x" << llvm::utohexstr(offset + bufferBaseAddr) << " "
         << "0x" << llvm::utohexstr(numBytes) << '\n';
  output << "_extern " << bufName << "\n";
  output << "_reserved DMb "
         << "0x" << llvm::utohexstr(offset + bufferBaseAddr) << " "
         << "0x" << llvm::utohexstr(numBytes) << '\n';
}
// Output the memorymap in gnu linker format for the given buffer operations,
// with the given offset. The offset is different depending on where the buffers
// are accessed from.
void writeLDScriptMap(raw_ostream &output, BufferOp buf, int offset) {
  std::string bufName(buf.name().getValue());
  int bufferBaseAddr = getBufferBaseAddress(buf);
  int numBytes = buf.getAllocationSize();
  output << ". = 0x" << llvm::utohexstr(offset + bufferBaseAddr) << ";\n";
  output << bufName << " = .;\n";
  output << ". += 0x" << llvm::utohexstr(numBytes) << ";\n";
}

void registerAIETranslations() {
  TranslateFromMLIRRegistration registrationMMap(
      "aie-generate-mmap", "Generate AIE memory map",
      [](ModuleOp module, raw_ostream &output) {
        DenseMap<TileID, Operation *> tiles;
        DenseMap<Operation *, CoreOp> cores;
        DenseMap<Operation *, MemOp> mems;
        DenseMap<std::pair<Operation *, int>, LockOp> locks;
        DenseMap<Operation *, SmallVector<BufferOp, 4>> buffers;
        DenseMap<Operation *, SwitchboxOp> switchboxes;

        if (module.getOps<DeviceOp>().empty()) {
          module.emitOpError("expected AIE.device operation at toplevel");
        }
        DeviceOp targetOp = *(module.getOps<DeviceOp>().begin());

        collectTiles(targetOp, tiles);
        collectBuffers(targetOp, buffers);

        for (auto tile : tiles) {
          Operation *srcTileOp = tile.second;
          TileID srcCoord = cast<TileOp>(srcTileOp).getTileID();
          int srcCol = srcCoord.col;
          int srcRow = srcCoord.row;

          output << "// Tile(" << srcCol << ", " << srcRow << ")\n";
          output << "// Memory map: name base_address num_bytes\n";

          auto doBuffer = [&](std::optional<TileID> tile, int offset) {
            if (tiles.count(*tile))
              for (auto buf : buffers[tiles[*tile]])
                writeBufferMap(output, buf, offset);
          };

          const auto &targetModel = xilinx::AIE::getTargetModel(srcTileOp);

          if (auto tile = targetModel.getMemSouth(srcCoord))
            doBuffer(tile, targetModel.getMemSouthBaseAddress());
          if (auto tile = targetModel.getMemWest(srcCoord))
            doBuffer(tile, targetModel.getMemWestBaseAddress());
          if (auto tile = targetModel.getMemNorth(srcCoord))
            doBuffer(tile, targetModel.getMemNorthBaseAddress());
          if (auto tile = targetModel.getMemEast(srcCoord))
            doBuffer(tile, targetModel.getMemEastBaseAddress());
        }
        return success();
      },
      registerDialects);

  TranslateFromMLIRRegistration registrationShimDMAToJSON(
      "aie-generate-json", "Transform AIE shim DMA allocation info into JSON",
      [](ModuleOp module, raw_ostream &output) {
        for (auto d : module.getOps<DeviceOp>()) {
          llvm::json::Object moduleJSON;
          for (auto shimDMAMeta : d.getOps<ShimDMAAllocationOp>()) {
            llvm::json::Object shimJSON;
            auto channelDir = shimDMAMeta.getChannelDirAttr();
            shimJSON["channelDir"] = attrToJSON(channelDir);
            auto channelIndex = shimDMAMeta.getChannelIndexAttr();
            shimJSON["channelIndex"] = attrToJSON(channelIndex);
            auto col = shimDMAMeta.getColAttr();
            shimJSON["col"] = attrToJSON(col);
            moduleJSON[shimDMAMeta.getSymName()] =
                llvm::json::Value(std::move(shimJSON));
          }
          llvm::json::Value topv(std::move(moduleJSON));
          std::string ret;
          llvm::raw_string_ostream ss(ret);
          ss << llvm::formatv("{0:2}", topv) << "\n";
          output << ss.str();
        }
        return success();
      },
      registerDialects);

  ///// ld.script format:
  //
  // MEMORY
  // {
  //    program (RX) : ORIGIN = 0, LENGTH = 0x0020000
  //    data (!RX) : ORIGIN = 0x20000, LENGTH = 0x0020000
  // }
  // ENTRY(_main_init)
  // INPUT(something.o)
  // SECTIONS
  // {
  //   . = 0x0;
  //   .text : {
  //      // the _main_init symbol from me_basic.o has to come at address zero.
  //      *me_basic.o(.text)
  //      . = 0x200;
  //      __ctors_start__ = .;
  //      __init_array_start = .;
  //      KEEP(SORT(*)(.init_array))
  //      __ctors_end__ = .;
  //      __init_array_end = .;
  //      __dtors_start__ = .;
  //      __dtors_end__ = .;
  //      *(.text)
  //   } > program
  //   .data : { *(.data) } > data
  //   . = 0x20000;
  //   _sp_start_value_DM_stack = .;
  //   . = 0x24000;
  //   a = .;
  //   . += 1024;
  //   .bss : { *(.bss) } > data
  // }

  TranslateFromMLIRRegistration registrationLDScript(
      "aie-generate-ldscript", "Generate AIE loader script",
      [](ModuleOp module, raw_ostream &output) {
        DenseMap<TileID, Operation *> tiles;
        DenseMap<Operation *, CoreOp> cores;
        DenseMap<Operation *, MemOp> mems;
        DenseMap<std::pair<Operation *, int>, LockOp> locks;
        DenseMap<Operation *, SmallVector<BufferOp, 4>> buffers;
        DenseMap<Operation *, SwitchboxOp> switchboxes;

        if (module.getOps<DeviceOp>().empty()) {
          module.emitOpError("expected AIE.device operation at toplevel");
        }
        DeviceOp targetOp = *(module.getOps<DeviceOp>().begin());

        collectTiles(targetOp, tiles);
        collectBuffers(targetOp, buffers);

        for (auto tile : targetOp.getOps<TileOp>())
          if (tile.colIndex() == tileCol && tile.rowIndex() == tileRow) {
            TileID srcCoord = {tile.colIndex(), tile.rowIndex()};
            const auto &targetModel = getTargetModel(tile);

            // Figure out how much memory we have left for random allocations
            auto core = tile.getCoreOp();
            int max = core.getStackSize();
            for (auto buf : buffers[tiles[srcCoord]]) {
              int bufferBaseAddr = getBufferBaseAddress(buf);
              int numBytes = buf.getAllocationSize();
              max = std::max(max, bufferBaseAddr + numBytes);
            }
            int origin = targetModel.getMemInternalBaseAddress(srcCoord) + max;
            int length = targetModel.getLocalMemorySize() - max;
            output << R"THESCRIPT(
MEMORY
{
   program (RX) : ORIGIN = 0, LENGTH = 0x0020000
)THESCRIPT";
            output << "   data (!RX) : ORIGIN = 0x" << llvm::utohexstr(origin)
                   << ", LENGTH = 0x" << llvm::utohexstr(length);
            output << R"THESCRIPT(
}
ENTRY(_main_init)
SECTIONS
{
  . = 0x0;
  .text : { 
     /* the _main_init symbol from me_basic.o has to come at address zero. */
     *me_basic.o(.text)
     . = 0x200;
     _ctors_start = .;
     _init_array_start = .;
     KEEP(SORT(*.init_array))
     _ctors_end = .;
     _init_array_end = .;
     _dtors_start = .;
     _dtors_end = .;
     *(.text)
  } > program
  .data : { 
     *(.data*);
     *(.rodata*)
  } > data
)THESCRIPT";
            auto doBuffer = [&](std::optional<TileID> tile, int offset,
                                std::string dir) {
              if (tile) {
                if (tiles.count(*tile))
                  for (auto buf : buffers[tiles[*tile]])
                    writeLDScriptMap(output, buf, offset);
              } else {
                output << "/* No tile with memory exists to the " << dir
                       << ". */\n";
                output << ". = 0x" << llvm::utohexstr(offset) << ";\n";
                uint32_t localMemSize = targetModel.getLocalMemorySize();
                output << ". += 0x" << llvm::utohexstr(localMemSize) << ";\n";
              }
            };

            // Stack
            output << ". = 0x"
                   << llvm::utohexstr(
                          targetModel.getMemInternalBaseAddress(srcCoord))
                   << ";\n";
            output << "_sp_start_value_DM_stack = .;\n";

            if (auto core = tile.getCoreOp())
              output << ". += 0x" << llvm::utohexstr(core.getStackSize())
                     << "; /* stack */\n";
            else
              output << "/* no stack allocated */\n";

            doBuffer(targetModel.getMemSouth(srcCoord),
                     targetModel.getMemSouthBaseAddress(),
                     std::string("south"));
            doBuffer(targetModel.getMemWest(srcCoord),
                     targetModel.getMemWestBaseAddress(), std::string("west"));
            doBuffer(targetModel.getMemNorth(srcCoord),
                     targetModel.getMemNorthBaseAddress(),
                     std::string("north"));
            doBuffer(targetModel.getMemEast(srcCoord),
                     targetModel.getMemEastBaseAddress(), std::string("east"));

            output << "  .bss : { *(.bss) } > data\n";
            output << "  .bss.DMb.4 : { *(.bss.DMb.4) } > data\n";
            output << "}\n";
            if (auto coreOp = tile.getCoreOp()) {
              if (auto fileAttr =
                      coreOp->getAttrOfType<StringAttr>("link_with")) {
                auto fileName = std::string(fileAttr.getValue());
                output << "INPUT(" << fileName << ")\n";
              }
              output << "PROVIDE(_main = core_" << tile.getCol() << "_"
                     << tile.getRow() << ");\n";
            }
          }
        return success();
      },
      registerDialects);

  //   _entry_point _main_init
  // _symbol      _main _after _main_init
  // _symbol      _main_init 0
  // _reserved DMb      0x00000 0x20000
  // _symbol   a        0x38000 0x2000
  // _extern   a
  // _stack    DM_stack 0x20000  0x400 //stack for core
  // _reserved DMb 0x40000 0xc0000 // And everything else the core can't see

  TranslateFromMLIRRegistration registrationBCF(
      "aie-generate-bcf", "Generate AIE bcf",
      [](ModuleOp module, raw_ostream &output) {
        DenseMap<TileID, Operation *> tiles;
        DenseMap<Operation *, CoreOp> cores;
        DenseMap<Operation *, MemOp> mems;
        DenseMap<std::pair<Operation *, int>, LockOp> locks;
        DenseMap<Operation *, SmallVector<BufferOp, 4>> buffers;
        DenseMap<Operation *, SwitchboxOp> switchboxes;

        if (module.getOps<DeviceOp>().empty()) {
          module.emitOpError("expected AIE.device operation at toplevel");
        }
        DeviceOp targetOp = *(module.getOps<DeviceOp>().begin());

        collectTiles(targetOp, tiles);
        collectBuffers(targetOp, buffers);

        // _entry_point _main_init
        // _symbol      _main _after _main_init
        // _symbol      _main_init 0
        // _reserved DMb      0x00000 0x20000
        // _symbol   a        0x38000 0x2000
        // _extern   a
        // _stack    DM_stack 0x20000  0x400 //stack for core
        // _reserved DMb 0x40000 0xc0000 // And everything else the core can't
        // see
        // // Include all symbols from rom.c
        // _include _file rom.o
        for (auto tile : targetOp.getOps<TileOp>())
          if (tile.colIndex() == tileCol && tile.rowIndex() == tileRow) {
            const auto &targetModel = getTargetModel(tile);

            std::string corefunc = std::string("core_") +
                                   std::to_string(tile.getCol()) + "_" +
                                   std::to_string(tile.getRow());
            output << "_entry_point _main_init\n";
            output << "_symbol " << corefunc << " _after _main_init\n";
            output << "_symbol      _main_init 0\n";
            std::string initReserved =
                (targetModel.getTargetArch() == AIEArch::AIE2) ? "0x40000"
                                                               : "0x20000";
            output << "_reserved DMb      0x00000 " << initReserved
                   << " //Don't put data in code memory\n";

            TileID srcCoord = {tile.colIndex(), tile.rowIndex()};
            auto doBuffer = [&](std::optional<TileID> tile, int offset,
                                const std::string &dir) {
              if (tile) {
                if (tiles.count(*tile))
                  for (auto buf : buffers[tiles[*tile]])
                    writeBCFMap(output, buf, offset);
                uint32_t localMemSize = targetModel.getLocalMemorySize();
                if (tile != srcCoord)
                  output << "_reserved DMb 0x" << llvm::utohexstr(offset) << " "
                         << "0x" << llvm::utohexstr(localMemSize) << " "
                         << " // Don't allocate variables outside of local "
                            "memory.\n";
                // TODO How to set as reserved if no buffer exists (or reserve
                // remaining buffer)
              } else {
                uint32_t localMemSize = targetModel.getLocalMemorySize();
                output << "_reserved DMb 0x" << llvm::utohexstr(offset) << " "
                       << "0x" << llvm::utohexstr(localMemSize) << " "
                       << " // No tile with memory exists to the " << dir
                       << ".\n";
              }
            };

            doBuffer(targetModel.getMemSouth(srcCoord),
                     targetModel.getMemSouthBaseAddress(),
                     std::string("south"));
            doBuffer(targetModel.getMemWest(srcCoord),
                     targetModel.getMemWestBaseAddress(), std::string("west"));
            doBuffer(targetModel.getMemNorth(srcCoord),
                     targetModel.getMemNorthBaseAddress(),
                     std::string("north"));
            doBuffer(targetModel.getMemEast(srcCoord),
                     targetModel.getMemEastBaseAddress(), std::string("east"));

            int stacksize = 0;
            if (auto core = tile.getCoreOp())
              stacksize = core.getStackSize();
            output << "_stack    DM_stack 0x"
                   << llvm::utohexstr(
                          targetModel.getMemInternalBaseAddress(srcCoord))
                   << "  0x" << llvm::utohexstr(stacksize)
                   << " //stack for core\n";

            if (targetModel.getTargetArch() == AIEArch::AIE2) {
              output << "_reserved DMb 0x80000 0x80000 // And everything else "
                        "the core can't see\n";
            } else {
              output << "_reserved DMb 0x40000 0xc0000 // And everything else "
                        "the core can't see\n";
            }
            if (auto coreOp = tile.getCoreOp()) {
              if (auto fileAttr =
                      coreOp->getAttrOfType<StringAttr>("link_with")) {
                auto fileName = std::string(fileAttr.getValue());
                output << "_include _file " << fileName << "\n";
              }
            }
            output << "_resolve _main core_" << tile.getCol() << "_"
                   << tile.getRow() << "\n";
          }
        return success();
      },
      registerDialects);

  TranslateFromMLIRRegistration registrationTargetArch(
      "aie-generate-target-arch", "Get the target architecture",
      [](ModuleOp module, raw_ostream &output) {
        AIEArch arch = AIEArch::AIE1;
        if (!module.getOps<DeviceOp>().empty()) {
          DeviceOp targetOp = *(module.getOps<DeviceOp>().begin());
          arch = targetOp.getTargetModel().getTargetArch();
        }
        if (arch == AIEArch::AIE1)
          output << "AIE\n";
        else
          output << stringifyEnum(arch) << "\n";
        return success();
      },
      registerDialects);

  TranslateFromMLIRRegistration registrationCoreList(
      "aie-generate-corelist", "Generate python list of cores",
      [](ModuleOp module, raw_ostream &output) {
        if (module.getOps<DeviceOp>().empty()) {
          module.emitOpError("expected AIE.device operation at toplevel");
        }
        DeviceOp targetOp = *(module.getOps<DeviceOp>().begin());

        output << "[";
        for (auto tileOp : targetOp.getOps<TileOp>()) {
          int col = tileOp.colIndex();
          int row = tileOp.rowIndex();
          if (auto coreOp = tileOp.getCoreOp()) {
            std::string elf_file = "None";
            if (auto fileAttr = coreOp->getAttrOfType<StringAttr>("elf_file"))
              elf_file = "\"" + std::string(fileAttr.getValue()) + "\"";
            output << '(' << std::to_string(col) << ',' << std::to_string(row)
                   << ',' << elf_file << "),";
          }
        }
        output << "]\n";
        return success();
      },
      registerDialects);

  TranslateFromMLIRRegistration registrationXADF(
      "adf-generate-cpp-graph", "Translate ADFDialect to C++ graph",
      ADFGenerateCPPGraph, [](DialectRegistry &registry) {
        registry.insert<xilinx::ADF::ADFDialect>();
        registerDialects(registry);
      });
  TranslateFromMLIRRegistration registrationXAIE(
      "aie-generate-xaie", "Generate libxaie configuration",
      [](ModuleOp module, raw_ostream &output) {
        return AIETranslateToXAIEV2(module, output);
      },
      registerDialects);
  TranslateFromMLIRRegistration registrationXJSON(
      "aie-flows-to-json", "Translate AIE flows to JSON", AIEFlowsToJSON,
      registerDialects);
  TranslateFromMLIRRegistration registrationXPE(
      "aie-mlir-to-xpe", "Translate AIE design to XPE file for simulation",
      AIETranslateGraphXPE, registerDialects);
  TranslateFromMLIRRegistration registrationSCSimConfig(
      "aie-mlir-to-scsim-config",
      "Translate AIE design to SCSimConfig file for simulation",
      AIETranslateSCSimConfig, registerDialects);
  TranslateFromMLIRRegistration registrationShimSolution(
      "aie-mlir-to-shim-solution",
      "Translate AIE design to ShimSolution file for simulation",
      AIETranslateShimSolution, registerDialects);
}
} // namespace AIE
} // namespace xilinx
