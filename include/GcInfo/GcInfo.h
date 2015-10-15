//===---- include/gcinfo/gcinfo.h -------------------------------*- C++ -*-===//
//
// LLILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief GCInfo Generator for LLILC
///
//===----------------------------------------------------------------------===//

#ifndef GCINFO_H
#define GCINFO_H

#include "gcinfoencoder.h"
#include "jitpch.h"
#include "LLILCJit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include <vector>

class GcInfoAllocator;
class GcInfoEncoder;

/// Per Jit Invocation GcInfo

class GcInfo {
public:
  static const uint32_t UnmanagedAddressSpace = 0;
  static const uint32_t ManagedAddressSpace = 1;
  static const int32_t InvalidPointerOffset = -1;

  static bool isGcPointer(const llvm::Type *Type);
  static bool isGcAggregate(const llvm::Type *Type);
  static bool isGcType(const llvm::Type *Type) {
    return isGcPointer(Type) || isGcAggregate(Type);
  }
  static bool isUnmanagedPointer(const llvm::Type *Type) {
    return Type->isPointerTy() && !isGcPointer(Type);
  }
  static bool isGcFunction(const llvm::Function *F);
  static void getGcPointers(llvm::StructType *StructTy, 
                            const llvm::DataLayout &DataLayout, 
                            llvm::SmallVector<uint32_t, 4> &GcPtrOffsets);

  GcInfo(const llvm::Function *F);
  ~GcInfo();

  void recordPinnedSlot(llvm::AllocaInst* Alloca);
  void recordGcAggregate(llvm::AllocaInst* Alloca);

  void getEscapingLocations(llvm::SmallVector<llvm::Value*, 4> &EscapingLocs);

  const llvm::Function *Function;
  llvm::ValueMap<const llvm::AllocaInst *, int32_t> PinnedSlots;
  llvm::ValueMap<const llvm::AllocaInst *, int32_t> GcAggregates;
  llvm::AllocaInst *GsCookie;
  int32_t GSCookieOffset;
  llvm::AllocaInst *SecurityObject;
  int32_t SecurityObjectOffset;
  llvm::AllocaInst *GenericsContext;
  int32_t GenericsContextOffset;
};

/// Per Function GcInfo

class GcFuncInfo {
public:
  GcFuncInfo() {}
  ~GcFuncInfo() {}

};

/// \brief This is the translator from LLVM's GC StackMaps
///  to CoreCLR's GcInfo encoding.
class GcInfoEmitter {
public:
  /// Construct a GCInfo object
  /// \param JitCtx Context record for the method's jit request.
  /// \param StackMapData A pointer to the .llvm_stackmaps section
  ///        loaded in memory
  /// \param Allocator The allocator to be used by GcInfo encoder
  /// \param OffsetCorrection FunctionStart - CodeBlockStart difference
  GcInfoEmitter(LLILCJitContext *JitCtx, uint8_t *StackMapData,
         GcInfoAllocator *Allocator, size_t OffsetCorrection);

  /// Emit GC Info to the EE using GcInfoEncoder.
  void emitGCInfo();

  /// Destructor -- delete allocated memory
  ~GcInfoEmitter();

private:
  void emitGCInfo(const llvm::Function &F, const GcInfo &GcInfo);
  void encodeHeader(const llvm::Function &F);
  void encodeLiveness(const llvm::Function &F);
  void encodePinned(const llvm::Function &F, const GcInfo &GcInfo);
  void encodeGcAggregates(const llvm::Function &F, const GcInfo &GcInfo);
  void emitEncoding();

  bool shouldEmitGCInfo(const llvm::Function &F);
  bool isStackBaseFramePointer(const llvm::Function &F);

  const LLILCJitContext *JitContext;
  const uint8_t *LLVMStackMapData;
  GcInfoEncoder Encoder;

  // The InstructionOffsets reported at Call-sites are with respect to:
  // (1) FunctionEntry in LLVM's StackMap
  // (2) CodeBlockStart in CoreCLR's GcTable
  // OffsetCorrection accounts for the difference:
  // FunctionStart - CodeBlockStart
  //
  // There is typically a difference between the two even in the JIT case
  // (where we emit one function per module) because of some additional
  // code like the gc.statepoint_poll() method.
  size_t OffsetCorrection;

  // SlotMap
  llvm::DenseMap<int32_t, uint32_t> SlotMap;

#if !defined(NDEBUG)
  bool EmitLogs;
#endif // !NDEBUG

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  size_t NumCallSites;
  unsigned *CallSites;
  BYTE *CallSiteSizes;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
};


class GcInfoRecorder : public llvm::MachineFunctionPass {
public:
  explicit GcInfoRecorder() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(llvm::MachineFunction &MF) override;

private:
  static char ID;
};

#endif // GCINFO_H
