//===------------------- include/Reader/abisignature..cpp -------*- C++ -*-===//
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
/// \brief Defines ABI signature abstractions used when lowering functions to
///        LLVM IR.
///
//===----------------------------------------------------------------------===//

#include "earlyincludes.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Statepoint.h"
#include "reader.h"
#include "readerir.h"
#include "abi.h"
#include "abisignature.h"
#include "imeta.h"
#include <cstdint>
#include <cassert>

using namespace llvm;

static CallingConv::ID getLLVMCallingConv(CorInfoCallConv CC,
                                          bool &IsManagedCallingConv) {
  switch (CC) {
  case CORINFO_CALLCONV_STDCALL:
    return CallingConv::X86_StdCall;
  case CORINFO_CALLCONV_THISCALL:
    return CallingConv::X86_ThisCall;
  case CORINFO_CALLCONV_FASTCALL:
    return CallingConv::X86_FastCall;
  case CORINFO_CALLCONV_DEFAULT:
    IsManagedCallingConv = true;
    return CallingConv::C;
  default:
    return CallingConv::C;
  }
}

static CorInfoCallConv
getNormalizedCallingConvention(const ReaderCallSignature &Signature) {
  // NOTE: this is only correct for X86-64

  CorInfoCallConv CC = Signature.getCallingConvention();
  switch (CC) {
  case CORINFO_CALLCONV_STDCALL:
  case CORINFO_CALLCONV_THISCALL:
  case CORINFO_CALLCONV_FASTCALL:
    return CORINFO_CALLCONV_C;
  default:
    return CC;
  }
}

ABISignature::ABISignature(const ReaderCallSignature &Signature, GenIR &Reader,
                           const ABIInfo &TheABIInfo) {
  const CallArgType &ResultType = Signature.getResultType();
  const std::vector<CallArgType> &ArgTypes = Signature.getArgumentTypes();
  const uint32_t NumArgs = ArgTypes.size();

  ABIType ABIResultType(Reader.getType(ResultType.CorType, ResultType.Class),
                        GenIR::isSignedIntegralType(ResultType.CorType));

  SmallVector<ABIType, 16> ABIArgTypes(NumArgs);
  uint32_t I = 0;
  for (const CallArgType &Arg : ArgTypes) {
    ABIArgTypes[I++] = ABIType(Reader.getType(Arg.CorType, Arg.Class),
                               GenIR::isSignedIntegralType(Arg.CorType));
  }

  bool IsManagedCallingConv = false;
  CallingConv::ID CC = getLLVMCallingConv(
      getNormalizedCallingConvention(Signature), IsManagedCallingConv);
  TheABIInfo.computeSignatureInfo(CC, IsManagedCallingConv, ABIResultType,
                                  ABIArgTypes, Result, Args);

  if (Result.getKind() == ABIArgInfo::Indirect) {
    FuncResultType = Reader.getManagedPointerType(Result.getType());
  } else {
    FuncResultType = Result.getType();
  }
}

Value *ABISignature::coerce(GenIR &Reader, Type *TheType, Value *TheValue) {
  assert(!TheType->isVoidTy());

  bool PointerRepresentsStruct = Reader.doesValueRepresentStruct(TheValue);
  Type *ValueType = nullptr;
  Value *ValuePtr = nullptr;
  if (PointerRepresentsStruct) {
    ValueType = TheValue->getType()->getPointerElementType();
    if (TheType == ValueType) {
      assert(TheType->isStructTy());
      return TheValue;
    }
    ValuePtr = TheValue;
  } else {
    ValueType = TheValue->getType();
    if (TheType == ValueType) {
      return TheValue;
    }
    ValuePtr = (Value *)Reader.addressOfValue((IRNode *)TheValue);
  }

  // TODO: the code spit could probably be better here.
  IRBuilder<> &Builder = *Reader.LLVMBuilder;
  Type *TargetPtrTy = TheType->getPointerTo();
  Value *TargetPtr = Builder.CreatePointerCast(ValuePtr, TargetPtrTy);
  if (TheType->isStructTy()) {
    Reader.setValueRepresentsStruct(TargetPtr);
    return TargetPtr;
  } else {
    return Builder.CreateLoad(TargetPtr);
  }
}

ABICallSignature::ABICallSignature(const ReaderCallSignature &TheSignature,
                                   GenIR &Reader, const ABIInfo &TheABIInfo)
    : ABISignature(TheSignature, Reader, TheABIInfo), Signature(TheSignature) {}

static Value *getFieldAddress(IRBuilder<> &Builder, Value *Base,
                              uint32_t Offset, Type *FieldTy) {
  // The base value should be an i8* or i8[]*.
  assert(Base->getType()->isPointerTy());
  assert(Base->getType()->getPointerElementType()->isIntegerTy(8) ||
         Base->getType()
             ->getPointerElementType()
             ->getArrayElementType()
             ->isIntegerTy(8));

  Type *Int32Ty = Type::getInt32Ty(Builder.getContext());
  Value *Indices[] = {ConstantInt::get(Int32Ty, 0),
                      ConstantInt::get(Int32Ty, Offset)};
  Value *Address = Builder.CreateInBoundsGEP(Base, Indices);
  PointerType *AddressTy = cast<PointerType>(Address->getType());
  if (AddressTy->getElementType() != FieldTy) {
    AddressTy = PointerType::get(FieldTy, AddressTy->getAddressSpace());
    Address = Builder.CreatePointerCast(Address, AddressTy);
  }
  return Address;
}

CallSite ABICallSignature::emitUnmanagedCall(GenIR &Reader, Value *Target,
                                             bool MayThrow,
                                             ArrayRef<Value *> Arguments,
                                             Value *&Result) const {
  const LLILCJitContext &JitContext = *Reader.JitContext;
  const struct CORINFO_EE_INFO::InlinedCallFrameInfo &CallFrameInfo =
      JitContext.EEInfo.inlinedCallFrameInfo;
  LLVMContext &LLVMContext = *JitContext.LLVMContext;
  Type *Int8Ty = Type::getInt8Ty(LLVMContext);
  Type *Int32Ty = Type::getInt32Ty(LLVMContext);
  Type *Int64Ty = Type::getInt64Ty(LLVMContext);
  Type *Int8PtrTy = Reader.getUnmanagedPointerType(Int8Ty);
  IRBuilder<> &Builder = *Reader.LLVMBuilder;

  Reader.insertIRForUnmanagedCallFrame();

  Value *CallFrame = Reader.UnmanagedCallFrame;
  Value *Thread = Reader.ThreadPointer;
  assert(CallFrame != nullptr);
  assert(Thread != nullptr);

  // Set m_pDatum if necessary
  //
  // TODO: this needs to be updated for direct unmanaged calls, which require
  //       the target method handle instead of the stub secret parameter.
  if (Reader.MethodSignature.hasSecretParameter()) {
    Value *SecretParameter = Reader.secretParam();
    Value *CallTargetAddress =
        getFieldAddress(Builder, CallFrame, CallFrameInfo.offsetOfCallTarget,
                        SecretParameter->getType());
    Builder.CreateStore(SecretParameter, CallTargetAddress);
  }

  // Push the unmanaged call frame
  Value *FrameVPtr = getFieldAddress(Builder, CallFrame,
                                     CallFrameInfo.offsetOfFrameVptr, Int8Ty);
  Value *ThreadBase = Builder.CreateLoad(Thread);
  Value *ThreadFrameAddress = getFieldAddress(
      Builder, ThreadBase, JitContext.EEInfo.offsetOfThreadFrame, Int8PtrTy);
  Builder.CreateStore(FrameVPtr, ThreadFrameAddress);

  // Compute the address of the return address field
  Value *ReturnAddressAddress = getFieldAddress(
      Builder, CallFrame, CallFrameInfo.offsetOfReturnAddress, Int8PtrTy);

  // Compute the address of the GC mode field
  Value *GCStateAddress = getFieldAddress(
      Builder, ThreadBase, JitContext.EEInfo.offsetOfGCState, Int8Ty);

  // Compute address of the thread trap field
  Value *ThreadTrapAddress = nullptr;
  Type *ThreadTrapAddressTy = Reader.getUnmanagedPointerType(Int32Ty);
  void *IndirectAddrOfCaptureThreadGlobal = nullptr;
  void *AddrOfCaptureThreadGlobal =
      (void *)JitContext.JitInfo->getAddrOfCaptureThreadGlobal(
          &IndirectAddrOfCaptureThreadGlobal);
  void *AddrOfCaptureThreadHandle;
  bool IsIndirect;
  const bool IsReadOnly = true;
  const bool IsRelocatable = true;
  const bool IsCallTarget = false;
  if (AddrOfCaptureThreadGlobal != nullptr) {
    AddrOfCaptureThreadHandle = AddrOfCaptureThreadGlobal;
    IsIndirect = false;
  } else {
    AddrOfCaptureThreadHandle = IndirectAddrOfCaptureThreadGlobal;
    IsIndirect = true;
  }
  Value *RawThreadTrapAddress =
      Reader.handleToIRNode(mdtCaptureThreadGlobal, AddrOfCaptureThreadHandle,
                            AddrOfCaptureThreadHandle, IsIndirect, IsReadOnly,
                            IsRelocatable, IsCallTarget);
  ThreadTrapAddress =
      Builder.CreateIntToPtr(RawThreadTrapAddress, ThreadTrapAddressTy);

  // Compute address of GC pause helper
  Value *PauseHelperAddress =
      (Value *)Reader.getHelperCallAddress(CORINFO_HELP_STOP_FOR_GC);

  // Construct the call.
  //
  // The signature of the intrinsic is:
  // @llvm.experimental_gc_transition(
  //   fn_ptr target,
  //   i32 numCallArgs,
  //   i32 flags,
  //   ... call args ...,
  //   i32 numTransitionArgs,
  //   ... transition args...,
  //   i32 numDeoptArgs,
  //   ... deopt args...)
  //
  // In the case of CoreCLR, there are 4 transition args and 0 deopt args.
  //
  // The transition args are:
  // 0) Address of the return address field
  // 1) Address of the GC mode field
  // 2) Address of the thread trap global
  // 3) Address of CORINFO_HELP_STOP_FOR_GC
  Module *M = Reader.Function->getParent();
  Type *CallTypeArgs[] = {Target->getType()};
  Function *CallIntrinsic = Intrinsic::getDeclaration(
      M, Intrinsic::experimental_gc_statepoint, CallTypeArgs);

  const uint32_t PrefixArgCount = 5;
  const uint32_t TransitionArgCount = 4;
  const uint32_t PostfixArgCount = TransitionArgCount + 2;
  const uint32_t TargetArgCount = Arguments.size();
  SmallVector<Value *, 24> IntrinsicArgs(PrefixArgCount + TargetArgCount +
                                         PostfixArgCount);

  // ID, nop bytes, call target and target arguments
  IntrinsicArgs[0] = ConstantInt::get(Int64Ty, 0);
  IntrinsicArgs[1] = ConstantInt::get(Int32Ty, 0);
  IntrinsicArgs[2] = Target;
  IntrinsicArgs[3] = ConstantInt::get(Int32Ty, TargetArgCount);
  IntrinsicArgs[4] =
      ConstantInt::get(Int32Ty, (uint32_t)StatepointFlags::GCTransition);

  uint32_t I, J;
  for (I = 0, J = PrefixArgCount; I < TargetArgCount; I++, J++) {
    IntrinsicArgs[J] = Arguments[I];
  }

  // GC transition arguments
  IntrinsicArgs[J] = ConstantInt::get(Int32Ty, TransitionArgCount);
  IntrinsicArgs[J + 1] = ReturnAddressAddress;
  IntrinsicArgs[J + 2] = GCStateAddress;
  IntrinsicArgs[J + 3] = ThreadTrapAddress;
  IntrinsicArgs[J + 4] = PauseHelperAddress;

  // Deopt arguments
  IntrinsicArgs[J + 5] = ConstantInt::get(Int32Ty, 0);

  CallSite Call = Reader.makeCall(CallIntrinsic, MayThrow, IntrinsicArgs);

  // Get the call result if necessary
  if (!FuncResultType->isVoidTy()) {
    Type *ResultTypeArgs[] = {FuncResultType};
    Function *ResultIntrinsic = Intrinsic::getDeclaration(
        M, Intrinsic::experimental_gc_result, ResultTypeArgs);
    Result = Builder.CreateCall(ResultIntrinsic, Call.getInstruction());
  }

  // Deactivate the unmanaged call frame
  Builder.CreateStore(Constant::getNullValue(Int8PtrTy), ReturnAddressAddress);

  // Pop the unmanaged call frame
  Value *FrameLinkAddress = getFieldAddress(
      Builder, CallFrame, CallFrameInfo.offsetOfFrameLink, Int8PtrTy);
  Value *FrameLink = Builder.CreateLoad(FrameLinkAddress);
  Builder.CreateStore(FrameLink, ThreadFrameAddress);

  return Call;
}

Value *ABICallSignature::emitCall(GenIR &Reader, Value *Target, bool MayThrow,
                                  ArrayRef<Value *> Args,
                                  Value *IndirectionCell, bool IsJmp,
                                  Value **CallNode) const {
  assert(Target->getType()->isIntegerTy(Reader.TargetPointerSizeInBits));

  LLVMContext &Context = *Reader.JitContext->LLVMContext;

  // Compute the function type
  bool HasIndirectResult = Result.getKind() == ABIArgInfo::Indirect;
  bool HasIndirectionCell = IndirectionCell != nullptr;
  bool IsUnmanagedCall =
      Signature.getCallingConvention() != CORINFO_CALLCONV_DEFAULT;
  bool CallerHasSecretParameter = Reader.MethodSignature.hasSecretParameter();
  bool IsJmpWithSecretParam = IsJmp && CallerHasSecretParameter;
  assert(((HasIndirectionCell ? 1 : 0) + (IsUnmanagedCall ? 1 : 0) +
          (IsJmpWithSecretParam ? 1 : 0)) <= 1);

  uint32_t NumSpecialArgs = 0;
  if (HasIndirectionCell || IsJmpWithSecretParam) {
    NumSpecialArgs = 1;
  }

  uint32_t NumExtraArgs = (HasIndirectResult ? 1 : 0) + NumSpecialArgs;
  const uint32_t NumArgs = Args.size() + NumExtraArgs;
  Value *ResultNode = nullptr;
  SmallVector<Type *, 16> ArgumentTypes(NumArgs);
  SmallVector<Value *, 16> Arguments(NumArgs);
  SmallVector<AttributeSet, 16> Attrs(NumArgs + 1);
  IRBuilder<> &Builder = *Reader.LLVMBuilder;

  // Check for calls with special args.
  //
  // Any special arguments are passed immediately preceeding the normal
  // arguments. The backend will place these arguments in the appropriate
  // registers according to the calling convention. Each special argument should
  // be machine-word-sized.
  if (HasIndirectionCell) {
    assert(IndirectionCell->getType()->isIntegerTy(
        Reader.TargetPointerSizeInBits));
    ArgumentTypes[0] = IndirectionCell->getType();
    Arguments[0] = IndirectionCell;
  } else if (IsJmpWithSecretParam) {
    Arguments[0] = Reader.secretParam();
    ArgumentTypes[0] = Arguments[0]->getType();
  }

  int32_t ResultIndex = -1;
  if (HasIndirectResult) {
    ResultIndex = (int32_t)NumSpecialArgs + (Signature.hasThis() ? 1 : 0);
    Type *ResultTy = Result.getType();
    // Jmp target signature has to match the caller's signature. Since we type
    // the caller's indirect result parameters as managed pointers, jmp target's
    // indirect result parameters also have to be typed as managed pointers.
    ArgumentTypes[ResultIndex] = IsJmp
                                     ? Reader.getManagedPointerType(ResultTy)
                                     : Reader.getUnmanagedPointerType(ResultTy);
    if (IsJmp) {
      // When processing jmp, pass the pointer that we got from the caller
      // rather than a pointer to a copy in the current frame.
      Arguments[ResultIndex] = ResultNode = Reader.IndirectResult;
    } else {
      Arguments[ResultIndex] = ResultNode = Reader.createTemporary(ResultTy);
    }
    if (ResultTy->isStructTy()) {
      Reader.setValueRepresentsStruct(ResultNode);
    }
  } else {
    AttrBuilder RetAttrs;

    if (Result.getKind() == ABIArgInfo::ZeroExtend) {
      RetAttrs.addAttribute(Attribute::ZExt);
    } else if (Result.getKind() == ABIArgInfo::SignExtend) {
      RetAttrs.addAttribute(Attribute::SExt);
    }

    if (RetAttrs.hasAttributes()) {
      Attrs.push_back(
          AttributeSet::get(Context, AttributeSet::ReturnIndex, RetAttrs));
    }
  }

  uint32_t I = NumSpecialArgs, J = 0;
  for (auto Arg : Args) {
    AttrBuilder ArgAttrs;

    if (ResultIndex >= 0 && I == (uint32_t)ResultIndex) {
      I++;
    }

    const ABIArgInfo &ArgInfo = this->Args[J];
    Type *ArgType = Arg->getType();

    if (ArgInfo.getKind() == ABIArgInfo::Indirect) {
      // TODO: byval attribute support
      if (IsJmp) {
        // When processing jmp pass the pointer that we got from the caller
        // rather than a pointer to a copy in the current frame.
        Arguments[I] = Arg;
        ArgumentTypes[I] = ArgType;
      } else {
        Value *Temp = nullptr;
        if (Reader.doesValueRepresentStruct(Arg)) {
          StructType *ArgStructTy =
              cast<StructType>(ArgType->getPointerElementType());
          ArgumentTypes[I] = ArgType;
          Temp = Reader.createTemporary(ArgStructTy);
          const bool IsVolatile = false;
          Reader.copyStruct(ArgStructTy, Temp, Arg, IsVolatile);
        } else {
          ArgumentTypes[I] = ArgType->getPointerTo();
          Temp = Reader.createTemporary(ArgType);
          Builder.CreateStore(Arg, Temp);
        }
        Arguments[I] = Temp;
      }
    } else {
      ArgumentTypes[I] = ArgInfo.getType();
      Arguments[I] = coerce(Reader, ArgInfo.getType(), Arg);

      if (ArgInfo.getKind() == ABIArgInfo::ZeroExtend) {
        ArgAttrs.addAttribute(Attribute::ZExt);
      } else if (ArgInfo.getKind() == ABIArgInfo::SignExtend) {
        ArgAttrs.addAttribute(Attribute::SExt);
      }

      if (ArgAttrs.hasAttributes()) {
        const unsigned Idx = I + 1; // Add one to accomodate the return attrs.
        Attrs.push_back(AttributeSet::get(Context, Idx, ArgAttrs));
      }
    }

    I++, J++;
  }

  const bool IsVarArg = false;
  Type *FunctionTy = FunctionType::get(FuncResultType, ArgumentTypes, IsVarArg);
  Type *FunctionPtrTy = Reader.getUnmanagedPointerType(FunctionTy);

  Target = Builder.CreateIntToPtr(Target, FunctionPtrTy);

  // The most straightforward way to satisfy the constraints imposed by the GC
  // on threads that are executing unmanaged code is to make the transition to
  // and from unmanaged code immediately preceeding and following the machine
  // call instruction, respectively. Unfortunately, there is no way to express
  // this in "standard" LLVM IR, hence the intrinsic. This intrinsic is also
  // a special GC statepoint that forces any GC pointers in callee-saved
  // registers to be spilled to the stack.
  CallSite Call;
  Value *UnmanagedCallResult = nullptr;
  if (IsUnmanagedCall) {
    Call = emitUnmanagedCall(Reader, Target, MayThrow, Arguments,
                             UnmanagedCallResult);
  } else {
    Call = Reader.makeCall(Target, MayThrow, Arguments);
  }

  CallingConv::ID CC;
  if (HasIndirectionCell) {
    assert(Signature.getCallingConvention() == CORINFO_CALLCONV_DEFAULT);
    CC = CallingConv::CLR_VirtualDispatchStub;
  } else if (IsJmpWithSecretParam) {
    assert(Signature.getCallingConvention() == CORINFO_CALLCONV_DEFAULT);
    CC = CallingConv::CLR_SecretParameter;
  } else {
    bool Unused;
    CC = getLLVMCallingConv(getNormalizedCallingConvention(Signature), Unused);
  }
  Call.setCallingConv(CC);

  if (Attrs.size() > 0) {
    Call.setAttributes(AttributeSet::get(Context, Attrs));
  }

  if (ResultNode == nullptr) {
    assert(!HasIndirectResult);
    const CallArgType &SigResultType = Signature.getResultType();
    Type *Ty = Reader.getType(SigResultType.CorType, SigResultType.Class);
    if (!Ty->isVoidTy()) {
      ResultNode = coerce(Reader, Ty, IsUnmanagedCall ? UnmanagedCallResult
                                                      : Call.getInstruction());
    } else {
      ResultNode = Call.getInstruction();
    }
  } else {
    if (!Reader.doesValueRepresentStruct(ResultNode)) {
      ResultNode = Builder.CreateLoad(ResultNode);
    }
  }

  *CallNode = Call.getInstruction();

  if (IsJmp) {
    CallInst *TheCallInst = cast<CallInst>(*CallNode);
    TheCallInst->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
  }

  return ResultNode;
}

ABIMethodSignature::ABIMethodSignature(
    const ReaderMethodSignature &TheSignature, GenIR &Reader,
    const ABIInfo &TheABIInfo)
    : ABISignature(TheSignature, Reader, TheABIInfo), Signature(&TheSignature) {
}

Function *ABIMethodSignature::createFunction(GenIR &Reader, Module &M) {
  // Compute the function type
  LLVMContext &Context = M.getContext();
  bool HasIndirectResult = Result.getKind() == ABIArgInfo::Indirect;
  uint32_t NumExtraArgs = HasIndirectResult ? 1 : 0;
  const uint32_t NumArgs = Args.size() + NumExtraArgs;
  int32_t ResultIndex = -1;
  SmallVector<Type *, 16> ArgumentTypes(NumArgs);
  SmallVector<AttributeSet, 16> Attrs(NumArgs + 1);

  if (HasIndirectResult) {
    ResultIndex = Signature->hasThis() ? 1 : 0;
    Result.setIndex((uint32_t)ResultIndex);
    ArgumentTypes[ResultIndex] = Reader.getManagedPointerType(Result.getType());
  } else {
    AttrBuilder RetAttrs;

    if (Result.getKind() == ABIArgInfo::ZeroExtend) {
      RetAttrs.addAttribute(Attribute::ZExt);
    } else if (Result.getKind() == ABIArgInfo::SignExtend) {
      RetAttrs.addAttribute(Attribute::SExt);
    }

    if (RetAttrs.hasAttributes()) {
      Attrs.push_back(
          AttributeSet::get(Context, AttributeSet::ReturnIndex, RetAttrs));
    }
  }

  uint32_t I = 0;
  for (auto &Arg : Args) {
    AttrBuilder ArgAttrs;

    if (ResultIndex >= 0 && I == (uint32_t)ResultIndex) {
      I++;
    }

    if (Arg.getKind() == ABIArgInfo::Indirect) {
      // TODO: byval attribute support
      ArgumentTypes[I] = Reader.getManagedPointerType(Arg.getType());
    } else {
      ArgumentTypes[I] = Arg.getType();

      if (Arg.getKind() == ABIArgInfo::ZeroExtend) {
        ArgAttrs.addAttribute(Attribute::ZExt);
      } else if (Arg.getKind() == ABIArgInfo::SignExtend) {
        ArgAttrs.addAttribute(Attribute::SExt);
      }

      if (ArgAttrs.hasAttributes()) {
        const unsigned Idx = I + 1; // Add one to accomodate the return attrs.
        Attrs.push_back(AttributeSet::get(Context, Idx, ArgAttrs));
      }
    }
    Arg.setIndex(I);

    I++;
  }

  const bool IsVarArg = false;
  FunctionType *FunctionTy =
      FunctionType::get(FuncResultType, ArgumentTypes, IsVarArg);
  Function *F = Function::Create(FunctionTy, Function::ExternalLinkage,
                                 M.getModuleIdentifier(), &M);

  // Use "param" for these initial parameter values. Numbering here
  // is strictly positional (hence includes implicit parameters).
  uint32_t N = 0;
  for (Function::arg_iterator Args = F->arg_begin(); Args != F->arg_end();
       Args++) {
    Args->setName(Twine("param") + Twine(N++));
  }

  CallingConv::ID CC = Signature->hasSecretParameter()
                           ? CallingConv::CLR_SecretParameter
                           : CallingConv::C;
  F->setCallingConv(CC);

  if (Attrs.size() > 0) {
    F->setAttributes(AttributeSet::get(Context, Attrs));
  }

  F->setGC("coreclr");

  return F;
}

const ABIArgInfo &ABIMethodSignature::getResultInfo() const { return Result; }

const ABIArgInfo &ABIMethodSignature::getArgumentInfo(uint32_t I) const {
  assert(I < Args.size());
  return Args[I];
}
