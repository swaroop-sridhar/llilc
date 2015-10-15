//===-------- include/gcinfo/gcinfo.cpp -------------------------*- C++ -*-===//
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
/// \brief Implements the generation of CLR GCTables in LLILC
///
//===----------------------------------------------------------------------===//

#include "earlyincludes.h"
#include "GcInfo.h"
#include "LLILCJit.h"
#include "Target.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Object/StackMapParser.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include <sstream>

using namespace llvm;

//-------------------------------GcInfo------------------------------------------

bool GcInfo::isGcPointer(const Type *Type) {
  const PointerType *PtrType = dyn_cast<PointerType>(Type);
  if (PtrType != nullptr) {
    return PtrType->getAddressSpace() == GcInfo::ManagedAddressSpace;
  }

  return false;
}

bool GcInfo::isGcAggregate(const Type *AggType) {
  const VectorType *VecType = dyn_cast<VectorType>(AggType);
  if (VecType != nullptr) {
    return isGcPointer(VecType->getScalarType());
  }

  const ArrayType *ArrType = dyn_cast<ArrayType>(AggType);
  if (ArrType != nullptr) {
    return isGcPointer(ArrType->getElementType());
  }

  const StructType *StType = dyn_cast<StructType>(AggType);
  if (StType != nullptr) {
    for (Type *SubType : StType->subtypes()) {
      if (isGcType(SubType)) {
        return true;
      }
    }
  }

  return false;
}

bool GcInfo::isGcFunction(const llvm::Function *F) {
  if (!F->hasGC()) {
    return false;
  }

  const StringRef CoreCLRName("coreclr");
  return (CoreCLRName == F->getGC());
}

void GcInfo::getGcPointers(StructType *StructTy, const DataLayout &DataLayout,
                           SmallVector<uint32_t, 4> &Pointers) {

  assert(StructTy->isSized());
  const uint32_t PointerSize = DataLayout.getPointerSize();
  const uint32_t TypeSize = DataLayout.getTypeStoreSize(StructTy);

  const StructLayout *MainStructLayout = DataLayout.getStructLayout(StructTy);

  // Walk through the type in pointer-sized jumps.
  for (uint32_t GcOffset = 0; GcOffset < TypeSize; GcOffset += PointerSize) {
    const uint32_t FieldIndex =
        MainStructLayout->getElementContainingOffset(GcOffset);
    Type *FieldTy = StructTy->getStructElementType(FieldIndex);

    // If the field is a value class we need to dive in
    // to its fields and so on, until we reach a primitive type.
    if (FieldTy->isStructTy()) {

      // Prepare to loop through the nesting.
      const StructLayout *OuterStructLayout = MainStructLayout;
      uint32_t OuterOffset = GcOffset;
      uint32_t OuterIndex = FieldIndex;

      while (FieldTy->isStructTy()) {
        // Offset of the Inner class within the outer class
        const uint32_t InnerBaseOffset =
            OuterStructLayout->getElementOffset(OuterIndex);
        // Inner class should start at or before the outer offset
        assert(InnerBaseOffset <= OuterOffset);
        // Determine target offset relative to this inner class.
        const uint32_t InnerOffset = OuterOffset - InnerBaseOffset;
        // Get the inner class layout
        StructType *InnerStructTy = cast<StructType>(FieldTy);
        const StructLayout *InnerStructLayout =
            DataLayout.getStructLayout(InnerStructTy);
        // Find the field at that target offset.
        const uint32_t InnerIndex =
            InnerStructLayout->getElementContainingOffset(InnerOffset);
        // Update for next iteration.
        FieldTy = InnerStructTy->getStructElementType(InnerIndex);
        OuterStructLayout = InnerStructLayout;
        OuterOffset = InnerOffset;
        OuterIndex = InnerIndex;
      }
    }

    if (GcInfo::isGcPointer(FieldTy)) {
      Pointers.push_back(GcOffset);
    }
  }
}

GcFuncInfo *GcInfo::newGcInfo(const llvm::Function *F) {
  assert(getGcInfo(F) == nullptr && "Duplicate GcInfo");
  GcFuncInfo *GcFInfo = new GcFuncInfo(F);
  GcInfoMap[F] = GcFInfo;
  return GcFInfo;
}

GcFuncInfo *GcInfo::getGcInfo(const llvm::Function *F) {
  auto Iterator = GcInfoMap.find(F);

  if (Iterator == GcInfoMap.end()) {
    return nullptr;
  }

  GcFuncInfo *GcFInfo = Iterator->second;
  assert(F == GcFInfo->Function && "Function mismatch");

  return GcFInfo;
}

//-------------------------------GcFuncInfo------------------------------------------

GcFuncInfo::GcFuncInfo(const llvm::Function *F)
    : GsCookie(nullptr), SecurityObject(nullptr), GenericsContext(nullptr) {
  Function = F;
  GsCookieOffset = GcInfo::InvalidPointerOffset;
  SecurityObjectOffset = GcInfo::InvalidPointerOffset;
  GenericsContextOffset = GcInfo::InvalidPointerOffset;
}

void GcFuncInfo::recordPinnedSlot(AllocaInst *Alloca) {
  assert(PinnedSlots.find(Alloca) == PinnedSlots.end());
  PinnedSlots[Alloca] = GcInfo::InvalidPointerOffset;
}

void GcFuncInfo::recordGcAggregate(AllocaInst *Alloca) {
  assert(GcAggregates.find(Alloca) == GcAggregates.end());
  GcAggregates[Alloca] = GcInfo::InvalidPointerOffset;
}

void GcFuncInfo::getEscapingLocations(SmallVector<Value *, 4> &EscapingLocs) {
  if (GsCookie != nullptr) {
    EscapingLocs.push_back(GsCookie);
  }

  if (SecurityObject != nullptr) {
    EscapingLocs.push_back(SecurityObject);
  }

  if (GenericsContext != nullptr) {
    EscapingLocs.push_back(GenericsContext);
  }

  for (auto Pin : PinnedSlots) {
    EscapingLocs.push_back((Value *)Pin.first);
  }

  for (auto GcAggregate : GcAggregates) {
    EscapingLocs.push_back((Value *)GcAggregate.first);
  }
}

//-------------------------------GcInfoRecorder-----------------------------------

char GcInfoRecorder::ID = 0;

bool GcInfoRecorder::runOnMachineFunction(MachineFunction &MF) {
  const Function *F = MF.getFunction();
  if (!GcInfo::isGcFunction(F)) {
    return false;
  }

  LLILCJitContext *Context = LLILCJit::TheJit->getLLILCJitContext();
  GcFuncInfo *GcFuncInfo = Context->GcInfo->getGcInfo(F);
  ValueMap<const AllocaInst *, int32_t> &GcAggregates =
      GcFuncInfo->GcAggregates;
  ValueMap<const AllocaInst *, int32_t> &PinnedSlots = GcFuncInfo->PinnedSlots;

#if !defined(NDEBUG)
  bool EmitLogs = Context->Options->LogGcInfo;

  if (EmitLogs) {
    dbgs() << "GcInfoRecorder: " << MF.getFunction()->getName() << "\n";
  }
#endif // !NDEBUG

  const MachineFrameInfo *FrameInfo = MF.getFrameInfo();
  int ObjectIndexBegin = FrameInfo->getObjectIndexBegin();
  int ObjectIndexEnd = FrameInfo->getObjectIndexEnd();

  // FrameInfo reports the allocation offsets in terms of the
  // incoming (caller's) StackPointer. Convert these in terms of the
  // current (callee's) StackPointer.
  uint64_t StackPointerSize = MF.getDataLayout().getPointerSize();
  uint64_t SpOffset = FrameInfo->getStackSize() + StackPointerSize;

  for (int Idx = ObjectIndexBegin; Idx < ObjectIndexEnd; Idx++) {
    const AllocaInst *Alloca = FrameInfo->getObjectAllocation(Idx);
    if (Alloca == nullptr) {
      continue;
    }

    int32_t SlotOffset = SpOffset + FrameInfo->getObjectOffset(Idx);

    if (GcFuncInfo->GsCookie == Alloca) {
      GcFuncInfo->GsCookieOffset = SlotOffset;

#if !defined(NDEBUG)
      if (EmitLogs) {
        dbgs() << "GSCookie: @" << SlotOffset << "\n";
      }
#endif // !NDEBUG

    } else if (GcFuncInfo->SecurityObject == Alloca) {
      GcFuncInfo->SecurityObjectOffset = SlotOffset;

#if !defined(NDEBUG)
      if (EmitLogs) {
        dbgs() << "SecurityObjectOffset: @" << SlotOffset << "\n";
      }
#endif // !NDEBUG
    } else if (GcFuncInfo->GenericsContext == Alloca) {
      GcFuncInfo->GenericsContextOffset = SlotOffset;

#if !defined(NDEBUG)
      if (EmitLogs) {
        dbgs() << "GenericsContext: @" << SlotOffset << "\n";
      }
#endif // !NDEBUG
    } else if (PinnedSlots.find(Alloca) != PinnedSlots.end()) {
      assert(PinnedSlots[Alloca] == GcInfo::InvalidPointerOffset &&
             "Two allocations for the same pointer!");

      PinnedSlots[Alloca] = SlotOffset;

#if !defined(NDEBUG)
      if (EmitLogs) {
        dbgs() << "Pinned Pointer: @" << SlotOffset << "\n";
      }
#endif // !NDEBUG
    }

    Type *AllocatedType = Alloca->getAllocatedType();
    if (GcInfo::isGcAggregate(AllocatedType)) {
      assert(isa<StructType>(AllocatedType) && "Unexpected GcAggregate");
      assert(GcAggregates.find(Alloca) != GcAggregates.end());
      assert(GcAggregates[Alloca] == GcInfo::InvalidPointerOffset &&
             "Two allocations for the same aggregate!");

      GcAggregates[Alloca] = SlotOffset;

#if !defined(NDEBUG)
      if (EmitLogs) {
        dbgs() << "GC Aggregate: @" << SlotOffset << "\n";
      }
#endif // !NDEBUG
    }
  }

#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "\n";
  }
#endif // !NDEBUG

  return false; // success
}

//-------------------------------GcInfoEmitter-----------------------------------

GcInfoEmitter::GcInfoEmitter(LLILCJitContext *JitCtx, uint8_t *StackMapData,
                             GcInfoAllocator *Allocator, size_t OffsetCor)

    : JitContext(JitCtx), LLVMStackMapData(StackMapData),
      Encoder(JitContext->JitInfo, JitContext->MethodInfo, Allocator),
      OffsetCorrection(OffsetCor) {
#if !defined(NDEBUG)
  this->EmitLogs = JitContext->Options->LogGcInfo;
#endif // !NDEBUG
#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  this->CallSites = nullptr;
  this->CallSiteSizes = nullptr;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
}

void GcInfoEmitter::encodeHeader(const Function &F) {
#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "GcTable for Function: " << F.getName() << "\n";
  }
#endif // !NDEBUG

  // TODO: Set Code Length accurately.
  // https://github.com/dotnet/llilc/issues/679
  // JitContext->HotCodeSize is the size of the allocated code block.
  // It is not the actual length of the current function's code.
  Encoder.SetCodeLength(JitContext->HotCodeSize);
#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Size: " << JitContext->HotCodeSize << "\n";
  }
#endif // !NDEBUG

  if (isStackBaseFramePointer(F)) {
    Encoder.SetStackBaseRegister(REGNUM_FPBASE);
#if !defined(NDEBUG)
    if (EmitLogs) {
      dbgs() << "  StackBaseRegister: FP\n";
    }
#endif // !NDEBUG
  } else {
#if !defined(NDEBUG)
    if (EmitLogs) {
      dbgs() << "  StackBaseRegister: SP\n";
    }
#endif // !NDEBUG
  }

#if defined(FIXED_STACK_PARAMETER_SCRATCH_AREA)
  // TODO: Set size of outgoing/scratch area accurately
  // https://github.com/dotnet/llilc/issues/681
  const unsigned ScratchAreaSize = 0;
  Encoder.SetSizeOfStackOutgoingAndScratchArea(ScratchAreaSize);
#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Scratch Area Size: " << ScratchAreaSize << "\n";
  }
#endif // !NDEBUG
#endif // defined(FIXED_STACK_PARAMETER_SCRATCH_AREA)
}

void GcInfoEmitter::encodeLiveness(const Function &F) {
  if (LLVMStackMapData == nullptr) {
    return;
  }

  ArrayRef<uint8_t> StackMapContentsArray(LLVMStackMapData,
                                          JitContext->StackMapSize);

#if defined(BIGENDIAN)
  typedef StackMapV1Parser<support::big> StackMapParserType;
#else
  typedef StackMapV1Parser<support::little> StackMapParserType;
#endif
  StackMapParserType StackMapParser(StackMapContentsArray);

  // TODO: Once StackMap v2 is implemented, remove this assertion about
  // one function per module, and emit the GcInfo for the records
  // corresponding to the Function 'F'
  assert(StackMapParser.getNumFunctions() == 1 &&
         "Expect only one function with GcInfo in the module");

// Loop over LLVM StackMap records to:
// 1) Note CallSites (safepoints)
// 2) Assign Slot-IDs to each unique gc-pointer location (slot)
// 3) Record liveness (birth/death) of slots per call-site.

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  NumCallSites = StackMapParser.getNumRecords();
  CallSites = new unsigned[NumCallSites];
  CallSiteSizes = new BYTE[NumCallSites];
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)

  // TODO: Determine call-site-size accurately
  // https://github.com/Microsoft/llvm/issues/56
  // Call-site size is not available in LLVM's StackMap v1, so just make up
  // a value for now. The Call-instruction generated by LLILC on X86/X64
  // is typically Call [rax], which has a two-byte encoding.
  //
  // Any size > 0 can be reported as CallSizeSize, see explanation below.
  //
  // CoreCLR's API expects that we report:
  // (a) the offset at the beginning of the Call instruction, and
  // (b) size of the call instruction.
  //
  // LLVM's stackMap currently only reports:
  // (c) the offset at the safepoint after the call instruction (= a+b)
  //
  // When not in a fully-intrruptible block, CoreCLR only
  // uses the value of (a+b) to determine the end of the call
  // instruction. Therefore, we simply report a = c-2 and b = 2 for now.
  //
  // Once call-size size is available in LLVM StackMap v2, we can remove this
  // implementation specific workaround.

  const uint8_t CallSiteSize = 2;

  // Pinned locations must be allocated before tracked ones, so
  // that the slots are correctly marked as Pinned and Untracked.
  // Since Pinned pointers are rare, we let go of the first few
  // bits in the LiveSet, instead of complicating the logic in
  // this method with offset calculations.

  const uint32_t NumPinnedSlots = SlotMap.size();

  // LLVM StackMap records all live-pointers per Safepoint, whereas
  // CoreCLR's GCTables record pointer birth/deaths per Safepoint.
  // So, we do the translation using old/new live-pointer-sets
  // using bit-sets for recording the liveness -- one bit per slot.

  size_t LiveBitSetSize = 25;
  SmallBitVector OldLiveSet(LiveBitSetSize);
  SmallBitVector NewLiveSet(LiveBitSetSize);

  // TODO: Identify Object and Managed pointers differently
  // https://github.com/dotnet/llilc/issues/28
  // We currently conservatively describe all slots as containing
  // interior pointers
  const GcSlotFlags SlotFlags = (GcSlotFlags)GC_SLOT_INTERIOR;

#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  #Safepoints: " << StackMapParser.getNumRecords() << "\n";
  }

  std::ostringstream SlotStream;
  std::ostringstream LiveStream;
#endif // !NDEBUG

  size_t RecordIndex = 0;
  for (const auto &R : StackMapParser.records()) {

    // InstructionOffset:
    // + OffsetCorrection: to account for any bytes before the start
    //                     of the function.
    // - CallSiteSize: to report the start of the Instruction
    //
    // LLVM's Safepoint reports the offset at the end of the Call
    // instruction, whereas the CoreCLR API expects that we report
    // the start of the Call instruction.

    unsigned InstructionOffset =
        R.getInstructionOffset() + OffsetCorrection - CallSiteSize;

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
    CallSites[RecordIndex] = InstructionOffset;
    CallSiteSizes[RecordIndex] = CallSiteSize;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)

#if !defined(NDEBUG)
    if (EmitLogs) {
      LiveStream << "    " << RecordIndex << ": @" << InstructionOffset;
    }
#endif // !NDEBUG

    for (const auto &Loc : R.locations()) {

      switch (Loc.getKind()) {
      case StackMapParserType::LocationKind::Constant:
      case StackMapParserType::LocationKind::ConstantIndex:
        continue;

      case StackMapParserType::LocationKind::Register:
        // TODO: Report Live - GC values in Registers
        // https://github.com/dotnet/llilc/issues/474
        // Live gc-pointers are currently spilled to the stack at Safepoints.
        assert(false && "GC-Pointer Live in Register");
        break;

      case StackMapParserType::LocationKind::Direct: {
        // __LLVM_Stackmap reports the liveness of pointers wrt SP even for
        // methods which have a FP. If this changes, we need to change
        // SlotMap from {Offset -> SlotID} mapping
        //         to {(base, offset) -> SlotID) mapping.
        //
        // All pinned/untracked pointers are already reported wrt SP base.

        assert(Loc.getDwarfRegNum() == DW_STACK_POINTER &&
               "Expect Stack Pointer to be the base");

        GcSlotId SlotID;
        int32_t Offset = Loc.getOffset();
        DenseMap<int32_t, GcSlotId>::const_iterator ExistingSlot =
            SlotMap.find(Offset);
        if (ExistingSlot == SlotMap.end()) {
          SlotID = Encoder.GetStackSlotId(Offset, SlotFlags, GC_SP_REL);
          SlotMap[Offset] = SlotID;

          size_t NumSlots = SlotMap.size();
          assert(SlotID == (NumSlots - 1) && "SlotIDs dis-contiguous");

          if (NumSlots > LiveBitSetSize) {
            LiveBitSetSize += LiveBitSetSize;

            assert(LiveBitSetSize > OldLiveSet.size() &&
                   "Overflow -- Too many live pointers");

            OldLiveSet.resize(LiveBitSetSize);
            NewLiveSet.resize(LiveBitSetSize);
          }

#if !defined(NDEBUG)
          if (EmitLogs) {
            SlotStream << "    [" << SlotID << "]: "
                       << "sp+" << Offset << "\n";
          }
#endif // !NDEBUG
        } else {
          SlotID = ExistingSlot->second;
        }

        if (SlotID >= NumPinnedSlots) {
          NewLiveSet[SlotID] = true;
        }
        break;
      }

      default:
        assert(false && "Unexpected Location Type");
        break;
      }
    }

    for (GcSlotId SlotID = 0; SlotID < SlotMap.size(); SlotID++) {
      if (!OldLiveSet[SlotID] && NewLiveSet[SlotID]) {
#if !defined(NDEBUG)
        if (EmitLogs) {
          LiveStream << "  +" << SlotID;
        }
#endif // !NDEBUG
        Encoder.SetSlotState(InstructionOffset, SlotID, GC_SLOT_LIVE);
      } else if (OldLiveSet[SlotID] && !NewLiveSet[SlotID]) {
#if !defined(NDEBUG)
        if (EmitLogs) {
          LiveStream << "  -" << SlotID;
        }
#endif // !NDEBUG
        Encoder.SetSlotState(InstructionOffset, SlotID, GC_SLOT_DEAD);
      }

      OldLiveSet[SlotID] = NewLiveSet[SlotID];
      NewLiveSet[SlotID] = false;
    }

    RecordIndex++;

#if !defined(NDEBUG)
    if (EmitLogs) {
      LiveStream << "\n";
    }
#endif // !NDEBUG
  }

#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Slots:\n" << SlotStream.str();
    dbgs() << "  Safepoints:\n" << LiveStream.str() << "\n";
  }
#endif // !NDEBUG
}

void GcInfoEmitter::encodePinned(const Function &F,
                                 const GcFuncInfo *GcFuncInfo) {

  const GcSlotFlags SlotFlags =
      (GcSlotFlags)(GC_SLOT_BASE | GC_SLOT_PINNED | GC_SLOT_UNTRACKED);

#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Pinned Slots:\n";
  }
#endif // !NDEBUG

  for (auto Pin : GcFuncInfo->PinnedSlots) {
    int32_t Offset = Pin.second;
    assert(Offset != GcInfo::InvalidPointerOffset && "Pinned Slot Not Found!");

    assert(SlotMap.find(Offset) == SlotMap.end() &&
           "Pinned slot already allocated");
    GcSlotId SlotID = Encoder.GetStackSlotId(Offset, SlotFlags, GC_SP_REL);
    SlotMap[Offset] = SlotID;

#if !defined(NDEBUG)
    if (EmitLogs) {
      dbgs() << "    [" << SlotID << "]: "
             << "sp+" << Offset << "\n";
    }
#endif // !NDEBUG
  }
}

void GcInfoEmitter::encodeGcAggregates(const Function &F,
                                       const GcFuncInfo *GcFuncInfo) {
#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Untracked Slots:\n";
  }
#endif // !NDEBUG

  const GcSlotFlags SlotFlags = (GcSlotFlags)(GC_SLOT_BASE | GC_SLOT_UNTRACKED);

  for (auto Aggregate : GcFuncInfo->GcAggregates) {
    const AllocaInst *Alloca = Aggregate.first;
    Type *Type = Alloca->getAllocatedType();
    assert(isa<StructType>(Type) && "GcAggregate is not a struct");
    StructType *StructTy = cast<StructType>(Type);

    int32_t AggregateOffset = Aggregate.second;
    assert(AggregateOffset != GcInfo::InvalidPointerOffset &&
           "GcAggregate Not Found!");

    SmallVector<uint32_t, 4> GcPtrOffsets;
    const DataLayout &DataLayout = JitContext->CurrentModule->getDataLayout();
    GcInfo::getGcPointers(StructTy, DataLayout, GcPtrOffsets);

    for (uint32_t GcPtrOffset : GcPtrOffsets) {
      uint32_t Offset = AggregateOffset + GcPtrOffset;
      assert(SlotMap.find(Offset) == SlotMap.end() &&
             "Untracked slot already allocated");

      GcSlotId SlotID = Encoder.GetStackSlotId(Offset, SlotFlags, GC_SP_REL);
      SlotMap[Offset] = SlotID;

#if !defined(NDEBUG)
      if (EmitLogs) {
        dbgs() << "    [" << SlotID << "]: "
               << "sp+" << Offset << "\n";
      }
#endif // !NDEBUG
    }
  }

#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "    --\n";
  }
#endif // !NDEBUG
}

void GcInfoEmitter::finalizeEncoding() {
  // Finalize Slot IDs to enable compact representation
  Encoder.FinalizeSlotIds();

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  // Encode Call-sites
  Encoder.DefineCallSites(CallSites, CallSiteSizes, NumCallSites);
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
}

void GcInfoEmitter::emitEncoding() {
  Encoder.Build();
  Encoder.Emit();
}

GcInfoEmitter::~GcInfoEmitter() {
#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  delete CallSites;
  delete CallSiteSizes;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
}

void GcInfoEmitter::emitGCInfo(const Function &F,
                               const GcFuncInfo *GcFuncInfo) {
  encodeHeader(F);

  if (JitContext->Options->DoInsertStatepoints) {
    assert((GcFuncInfo != nullptr) && "GC Function missing GcInfo");
    // Pinned slots must be allocated before Live Slots
    encodePinned(F, GcFuncInfo);
    // Assign Slots for Tracked pointers and report their liveness
    encodeLiveness(F);
    // Aggregate slots should be allocated after Live Slots
    encodeGcAggregates(F, GcFuncInfo);
    // Finalization must be done after all encodings
    finalizeEncoding();
  }

  emitEncoding();
}

void GcInfoEmitter::emitGCInfo() {
  GcInfo *GcInfo = JitContext->GcInfo;

  for (Function &F : *JitContext->CurrentModule) {
    if (shouldEmitGCInfo(F)) {
      emitGCInfo(F, GcInfo->getGcInfo(&F));
    }
  }
}

bool GcInfoEmitter::shouldEmitGCInfo(const Function &F) {
  return !F.isDeclaration() && GcInfo::isGcFunction(&F);
}

bool GcInfoEmitter::isStackBaseFramePointer(const Function &F) {
  Attribute Attribute = F.getFnAttribute("no-frame-pointer-elim");
  return (Attribute.getValueAsString() == "true");
}
