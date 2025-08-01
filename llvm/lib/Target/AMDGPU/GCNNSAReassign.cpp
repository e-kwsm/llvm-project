//===-- GCNNSAReassign.cpp - Reassign registers in NSA instructions -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief Try to reassign registers on GFX10+ from non-sequential to sequential
/// in NSA image instructions. Later SIShrinkInstructions pass will replace NSA
/// with sequential versions where possible.
///
//===----------------------------------------------------------------------===//

#include "GCNNSAReassign.h"
#include "AMDGPU.h"
#include "GCNSubtarget.h"
#include "SIMachineFunctionInfo.h"
#include "SIRegisterInfo.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

#define DEBUG_TYPE "amdgpu-nsa-reassign"

STATISTIC(NumNSAInstructions,
          "Number of NSA instructions with non-sequential address found");
STATISTIC(NumNSAConverted,
          "Number of NSA instructions changed to sequential");

namespace {
class GCNNSAReassignImpl {
public:
  GCNNSAReassignImpl(VirtRegMap *VM, LiveRegMatrix *LM, LiveIntervals *LS)
      : VRM(VM), LRM(LM), LIS(LS) {}

  bool run(MachineFunction &MF);

private:
  using NSA_Status = enum {
    NOT_NSA,        // Not an NSA instruction
    FIXED,          // NSA which we cannot modify
    NON_CONTIGUOUS, // NSA with non-sequential address which we can try
                    // to optimize.
    CONTIGUOUS      // NSA with all sequential address registers
  };

  const GCNSubtarget *ST;

  const MachineRegisterInfo *MRI;

  const SIRegisterInfo *TRI;

  VirtRegMap *VRM;

  LiveRegMatrix *LRM;

  LiveIntervals *LIS;

  unsigned MaxNumVGPRs;

  const MCPhysReg *CSRegs;

  NSA_Status CheckNSA(const MachineInstr &MI, bool Fast = false) const;

  bool tryAssignRegisters(SmallVectorImpl<LiveInterval *> &Intervals,
                          unsigned StartReg) const;

  bool canAssign(unsigned StartReg, unsigned NumRegs) const;

  bool scavengeRegs(SmallVectorImpl<LiveInterval *> &Intervals) const;
};

class GCNNSAReassignLegacy : public MachineFunctionPass {
public:
  static char ID;

  GCNNSAReassignLegacy() : MachineFunctionPass(ID) {
    initializeGCNNSAReassignLegacyPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "GCN NSA Reassign"; };

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addRequired<VirtRegMapWrapperLegacy>();
    AU.addRequired<LiveRegMatrixWrapperLegacy>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // End anonymous namespace.

INITIALIZE_PASS_BEGIN(GCNNSAReassignLegacy, DEBUG_TYPE, "GCN NSA Reassign",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_END(GCNNSAReassignLegacy, DEBUG_TYPE, "GCN NSA Reassign", false,
                    false)

char GCNNSAReassignLegacy::ID = 0;

char &llvm::GCNNSAReassignID = GCNNSAReassignLegacy::ID;

bool GCNNSAReassignImpl::tryAssignRegisters(
    SmallVectorImpl<LiveInterval *> &Intervals, unsigned StartReg) const {
  unsigned NumRegs = Intervals.size();

  for (unsigned N = 0; N < NumRegs; ++N)
    if (VRM->hasPhys(Intervals[N]->reg()))
      LRM->unassign(*Intervals[N]);

  for (unsigned N = 0; N < NumRegs; ++N)
    if (LRM->checkInterference(*Intervals[N], MCRegister::from(StartReg + N)))
      return false;

  for (unsigned N = 0; N < NumRegs; ++N)
    LRM->assign(*Intervals[N], MCRegister::from(StartReg + N));

  return true;
}

bool GCNNSAReassignImpl::canAssign(unsigned StartReg, unsigned NumRegs) const {
  for (unsigned N = 0; N < NumRegs; ++N) {
    unsigned Reg = StartReg + N;
    if (!MRI->isAllocatable(Reg))
      return false;

    for (unsigned I = 0; CSRegs[I]; ++I)
      if (TRI->isSubRegisterEq(Reg, CSRegs[I]) &&
          !LRM->isPhysRegUsed(CSRegs[I]))
      return false;
  }

  return true;
}

bool GCNNSAReassignImpl::scavengeRegs(
    SmallVectorImpl<LiveInterval *> &Intervals) const {
  unsigned NumRegs = Intervals.size();

  if (NumRegs > MaxNumVGPRs)
    return false;
  unsigned MaxReg = MaxNumVGPRs - NumRegs + AMDGPU::VGPR0;

  for (unsigned Reg = AMDGPU::VGPR0; Reg <= MaxReg; ++Reg) {
    if (!canAssign(Reg, NumRegs))
      continue;

    if (tryAssignRegisters(Intervals, Reg))
      return true;
  }

  return false;
}

GCNNSAReassignImpl::NSA_Status
GCNNSAReassignImpl::CheckNSA(const MachineInstr &MI, bool Fast) const {
  const AMDGPU::MIMGInfo *Info = AMDGPU::getMIMGInfo(MI.getOpcode());
  if (!Info)
    return NSA_Status::NOT_NSA;

  switch (Info->MIMGEncoding) {
  case AMDGPU::MIMGEncGfx10NSA:
  case AMDGPU::MIMGEncGfx11NSA:
    break;
  default:
    return NSA_Status::NOT_NSA;
  }

  int VAddr0Idx =
    AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::vaddr0);

  unsigned VgprBase = 0;
  bool NSA = false;
  for (unsigned I = 0; I < Info->VAddrOperands; ++I) {
    const MachineOperand &Op = MI.getOperand(VAddr0Idx + I);
    Register Reg = Op.getReg();
    if (Reg.isPhysical() || !VRM->isAssignedReg(Reg))
      return NSA_Status::FIXED;

    Register PhysReg = VRM->getPhys(Reg);

    if (!Fast) {
      if (!PhysReg)
        return NSA_Status::FIXED;

      // TODO: address the below limitation to handle GFX11 BVH instructions
      // Bail if address is not a VGPR32. That should be possible to extend the
      // optimization to work with subregs of a wider register tuples, but the
      // logic to find free registers will be much more complicated with much
      // less chances for success. That seems reasonable to assume that in most
      // cases a tuple is used because a vector variable contains different
      // parts of an address and it is either already consecutive or cannot
      // be reassigned if not. If needed it is better to rely on register
      // coalescer to process such address tuples.
      if (TRI->getRegSizeInBits(*MRI->getRegClass(Reg)) != 32 || Op.getSubReg())
        return NSA_Status::FIXED;

      // InlineSpiller does not call LRM::assign() after an LI split leaving
      // it in an inconsistent state, so we cannot call LRM::unassign().
      // See llvm bug #48911.
      // Skip reassign if a register has originated from such split.
      // FIXME: Remove the workaround when bug #48911 is fixed.
      if (VRM->getPreSplitReg(Reg))
        return NSA_Status::FIXED;

      const MachineInstr *Def = MRI->getUniqueVRegDef(Reg);

      if (Def && Def->isCopy() && Def->getOperand(1).getReg() == PhysReg)
        return NSA_Status::FIXED;

      for (auto U : MRI->use_nodbg_operands(Reg)) {
        if (U.isImplicit())
          return NSA_Status::FIXED;
        const MachineInstr *UseInst = U.getParent();
        if (UseInst->isCopy() && UseInst->getOperand(0).getReg() == PhysReg)
          return NSA_Status::FIXED;
      }

      if (!LIS->hasInterval(Reg))
        return NSA_Status::FIXED;
    }

    if (I == 0)
      VgprBase = PhysReg;
    else if (VgprBase + I != PhysReg)
      NSA = true;
  }

  return NSA ? NSA_Status::NON_CONTIGUOUS : NSA_Status::CONTIGUOUS;
}

bool GCNNSAReassignImpl::run(MachineFunction &MF) {
  ST = &MF.getSubtarget<GCNSubtarget>();
  if (!ST->hasNSAEncoding() || !ST->hasNonNSAEncoding())
    return false;

  MRI = &MF.getRegInfo();
  TRI = ST->getRegisterInfo();

  const SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  MaxNumVGPRs = ST->getMaxNumVGPRs(MF);
  MaxNumVGPRs = std::min(
      ST->getMaxNumVGPRs(MFI->getOccupancy(), MFI->getDynamicVGPRBlockSize()),
      MaxNumVGPRs);
  CSRegs = MRI->getCalleeSavedRegs();

  using Candidate = std::pair<const MachineInstr*, bool>;
  SmallVector<Candidate, 32> Candidates;
  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      switch (CheckNSA(MI)) {
      default:
        continue;
      case NSA_Status::CONTIGUOUS:
        Candidates.push_back(std::pair(&MI, true));
        break;
      case NSA_Status::NON_CONTIGUOUS:
        Candidates.push_back(std::pair(&MI, false));
        ++NumNSAInstructions;
        break;
      }
    }
  }

  bool Changed = false;
  for (auto &C : Candidates) {
    if (C.second)
      continue;

    const MachineInstr *MI = C.first;
    if (CheckNSA(*MI, true) == NSA_Status::CONTIGUOUS) {
      // Already happen to be fixed.
      C.second = true;
      ++NumNSAConverted;
      continue;
    }

    const AMDGPU::MIMGInfo *Info = AMDGPU::getMIMGInfo(MI->getOpcode());
    int VAddr0Idx =
      AMDGPU::getNamedOperandIdx(MI->getOpcode(), AMDGPU::OpName::vaddr0);

    SmallVector<LiveInterval *, 16> Intervals;
    SmallVector<MCRegister, 16> OrigRegs;
    SlotIndex MinInd, MaxInd;
    for (unsigned I = 0; I < Info->VAddrOperands; ++I) {
      const MachineOperand &Op = MI->getOperand(VAddr0Idx + I);
      Register Reg = Op.getReg();
      LiveInterval *LI = &LIS->getInterval(Reg);
      if (llvm::is_contained(Intervals, LI)) {
        // Same register used, unable to make sequential
        Intervals.clear();
        break;
      }
      Intervals.push_back(LI);
      OrigRegs.push_back(VRM->getPhys(Reg));
      if (LI->empty()) {
        // The address input is undef, so it doesn't contribute to the relevant
        // range. Seed a reasonable index range if required.
        if (I == 0)
          MinInd = MaxInd = LIS->getInstructionIndex(*MI);
        continue;
      }
      MinInd = I != 0 ? std::min(MinInd, LI->beginIndex()) : LI->beginIndex();
      MaxInd = I != 0 ? std::max(MaxInd, LI->endIndex()) : LI->endIndex();
    }

    if (Intervals.empty())
      continue;

    LLVM_DEBUG(dbgs() << "Attempting to reassign NSA: " << *MI
                      << "\tOriginal allocation:\t";
               for (auto *LI
                    : Intervals) dbgs()
               << " " << llvm::printReg((VRM->getPhys(LI->reg())), TRI);
               dbgs() << '\n');

    bool Success = scavengeRegs(Intervals);
    if (!Success) {
      LLVM_DEBUG(dbgs() << "\tCannot reallocate.\n");
      if (VRM->hasPhys(Intervals.back()->reg())) // Did not change allocation.
        continue;
    } else {
      // Check we did not make it worse for other instructions.
      auto *I =
          std::lower_bound(Candidates.begin(), &C, MinInd,
                           [this](const Candidate &C, SlotIndex I) {
                             return LIS->getInstructionIndex(*C.first) < I;
                           });
      for (auto *E = Candidates.end();
           Success && I != E && LIS->getInstructionIndex(*I->first) < MaxInd;
           ++I) {
        if (I->second && CheckNSA(*I->first, true) < NSA_Status::CONTIGUOUS) {
          Success = false;
          LLVM_DEBUG(dbgs() << "\tNSA conversion conflict with " << *I->first);
        }
      }
    }

    if (!Success) {
      for (unsigned I = 0; I < Info->VAddrOperands; ++I)
        if (VRM->hasPhys(Intervals[I]->reg()))
          LRM->unassign(*Intervals[I]);

      for (unsigned I = 0; I < Info->VAddrOperands; ++I)
        LRM->assign(*Intervals[I], OrigRegs[I]);

      continue;
    }

    C.second = true;
    ++NumNSAConverted;
    LLVM_DEBUG(
        dbgs() << "\tNew allocation:\t\t ["
               << llvm::printReg((VRM->getPhys(Intervals.front()->reg())), TRI)
               << " : "
               << llvm::printReg((VRM->getPhys(Intervals.back()->reg())), TRI)
               << "]\n");
    Changed = true;
  }

  return Changed;
}

bool GCNNSAReassignLegacy::runOnMachineFunction(MachineFunction &MF) {
  auto *VRM = &getAnalysis<VirtRegMapWrapperLegacy>().getVRM();
  auto *LRM = &getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM();
  auto *LIS = &getAnalysis<LiveIntervalsWrapperPass>().getLIS();

  GCNNSAReassignImpl Impl(VRM, LRM, LIS);
  return Impl.run(MF);
}

PreservedAnalyses
GCNNSAReassignPass::run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM) {
  auto &VRM = MFAM.getResult<VirtRegMapAnalysis>(MF);
  auto &LRM = MFAM.getResult<LiveRegMatrixAnalysis>(MF);
  auto &LIS = MFAM.getResult<LiveIntervalsAnalysis>(MF);

  GCNNSAReassignImpl Impl(&VRM, &LRM, &LIS);
  Impl.run(MF);
  return PreservedAnalyses::all();
}
