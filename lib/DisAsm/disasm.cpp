//===--------- disasm.cpp - Instructionwise Disassembler Utility ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Adapted from llvm-objdump.cpp
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/Triple.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/DataTypes.h"

using namespace llvm;
using namespace std;

class Disasm {
public:
  bool init();
  size_t disasmInstruction(size_t Address, const uint8_t *Bytes,
                           size_t Maxlength) const;

  Disasm(string TargetTriple) { TripleName = TargetTriple; }

private:
  const Target *getTarget();

  const Target *TheTarget;
  unique_ptr<MCRegisterInfo> MRI;
  unique_ptr<const MCAsmInfo> AsmInfo;
  unique_ptr<const MCSubtargetInfo> STI;
  unique_ptr<const MCInstrInfo> MII;
  unique_ptr<const MCObjectFileInfo> MOFI;
  unique_ptr<MCContext> Ctx;
  unique_ptr<MCDisassembler> Disassembler;

  string TripleName; // Target triple to disassemble for.
};

const Target *Disasm::getTarget() {
  // Figure out the target triple.
  if (TripleName.empty()) {
    TripleName = sys::getDefaultTargetTriple();
  }

  TripleName = Triple::normalize(TripleName);
  Triple TheTriple(TripleName);

  // Get the target specific parser.
  string Error;
  string ArchName; // Target architecture is picked up from TargetTryple.
  const Target *TheTarget =
      TargetRegistry::lookupTarget(ArchName, TheTriple, Error);
  if (!TheTarget) {
    errs() << Error;
    return nullptr;
  }

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

bool Disasm::init() {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  // Call llvm_shutdown() on exit.
  llvm_shutdown_obj Y;

  // Initialize targets and assembly printers/parsers.
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllDisassemblers();

  TheTarget = getTarget();
  if (TheTarget == nullptr) {
    // getTarget() will have already issued a diagnostic if necessary, so
    // just bail here if it failed.
    return false;
  }

  MRI.reset(TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "error: no register info for target " << TripleName << "\n";
    return false;
  }

  // Set up disassembler.
  AsmInfo.reset(TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!AsmInfo) {
    errs() << "error: no assembly info for target " << TripleName << "\n";
    return false;
  }

  string Mcpu;        // Not specifying any particular CPU type.
  string FeaturesStr; // No additional target specific attributes.
  STI.reset(TheTarget->createMCSubtargetInfo(TripleName, Mcpu, FeaturesStr));
  if (!STI) {
    errs() << "error: no subtarget info for target " << TripleName << "\n";
    return false;
  }

  MII.reset(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "error: no instruction info for target " << TripleName << "\n";
    return false;
  }

  MOFI.reset(new MCObjectFileInfo);
  Ctx.reset(new MCContext(AsmInfo.get(), MRI.get(), MOFI.get()));

  Disassembler.reset(TheTarget->createMCDisassembler(*STI, *Ctx));

  if (!Disassembler) {
    errs() << "error: no disassembler for target " << TripleName << "\n";
    return false;
  }

  return true;
}

size_t Disasm::disasmInstruction(size_t Address, const uint8_t *Bytes,
                                 size_t Maxlength) const {
  uint64_t Size;
  MCInst Inst;
  raw_ostream &CommentStream = nulls();
  raw_ostream &DebugOut = nulls();
  ArrayRef<uint8_t> ByteArray(Bytes, Maxlength);

  bool success = Disassembler->getInstruction(Inst, Size, ByteArray, Address,
                                              DebugOut, CommentStream);

  if (!success) {
    errs() << "Invalid instruction encoding\n";
    return 0;
  }

  return Size;
}

// Allocate and initialize a Disassembler object.
// Returns the disassembler on success, nullptr on failure.
extern "C" Disasm *InitDisasm(const char* TargetTriple) {
  Disasm *Disassembler = new Disasm(TargetTriple);
  if (Disassembler->init()) {
    return Disassembler;
  }

  delete Disassembler;
  return nullptr;
}

extern "C" void FinishDisasm(const Disasm *Disasm) { delete Disasm; }

extern "C" size_t DisasmInstruction(const Disasm *Disasm, size_t Address,
                                    const uint8_t *Bytes, size_t Maxlength) {
  assert((Disasm != nullptr) && "Disassembler object Expected ");

  return Disasm->disasmInstruction(Address, Bytes, Maxlength);
}
