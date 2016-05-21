//===-- LiveIntervalAnalysis.cpp - Live Interval Analysis -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiveInterval analysis pass which is used
// by the Linear Scan Register allocator. This pass linearizes the
// basic blocks of the function in DFS order and uses the
// LiveVariables pass to conservatively compute live intervals for
// each virtual and physical register.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "LiveRangeCalc.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/BlockFrequency.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <algorithm>
#include <cmath>
using namespace llvm;

#define DEBUG_TYPE "regalloc"

char LiveIntervals::ID = 0;
char &llvm::LiveIntervalsID = LiveIntervals::ID;
INITIALIZE_PASS_BEGIN(LiveIntervals, "liveintervals",
                "Live Interval Analysis", false, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveVariables)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(SlotIndexes)
INITIALIZE_PASS_END(LiveIntervals, "liveintervals",
                "Live Interval Analysis", false, false)

#ifndef NDEBUG
static cl::opt<bool> EnablePrecomputePhysRegs(
  "precompute-phys-liveness", cl::Hidden,
  cl::desc("Eagerly compute live intervals for all physreg units."));
#else
static bool EnablePrecomputePhysRegs = false;
#endif // NDEBUG

static cl::opt<bool> EnableSubRegLiveness(
  "enable-subreg-liveness", cl::Hidden, cl::init(true),
  cl::desc("Enable subregister liveness tracking."));

namespace llvm {
cl::opt<bool> UseSegmentSetForPhysRegs(
    "use-segment-set-for-physregs", cl::Hidden, cl::init(true),
    cl::desc(
        "Use segment set for the computation of the live ranges of physregs."));
}

void LiveIntervals::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  // LiveVariables isn't really required by this analysis, it is only required
  // here to make sure it is live during TwoAddressInstructionPass and
  // PHIElimination. This is temporary.
  AU.addRequired<LiveVariables>();
  AU.addPreserved<LiveVariables>();
  AU.addPreservedID(MachineLoopInfoID);
  AU.addRequiredTransitiveID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addPreserved<SlotIndexes>();
  AU.addRequiredTransitive<SlotIndexes>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

LiveIntervals::LiveIntervals() : MachineFunctionPass(ID),
  DomTree(nullptr), LRCalc(nullptr) {
  initializeLiveIntervalsPass(*PassRegistry::getPassRegistry());
}

LiveIntervals::~LiveIntervals() {
  delete LRCalc;
}

void LiveIntervals::releaseMemory() {
  // Free the live intervals themselves.
  for (unsigned i = 0, e = VirtRegIntervals.size(); i != e; ++i)
    delete VirtRegIntervals[TargetRegisterInfo::index2VirtReg(i)];
  VirtRegIntervals.clear();
  RegMaskSlots.clear();
  RegMaskBits.clear();
  RegMaskBlocks.clear();

  for (unsigned i = 0, e = RegUnitRanges.size(); i != e; ++i)
    delete RegUnitRanges[i];
  RegUnitRanges.clear();

  // Release VNInfo memory regions, VNInfo objects don't need to be dtor'd.
  VNInfoAllocator.Reset();
}

/// runOnMachineFunction - calculates LiveIntervals
///
bool LiveIntervals::runOnMachineFunction(MachineFunction &fn) {
  MF = &fn;
  MRI = &MF->getRegInfo();
  TRI = MF->getSubtarget().getRegisterInfo();
  TII = MF->getSubtarget().getInstrInfo();
  AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  Indexes = &getAnalysis<SlotIndexes>();
  DomTree = &getAnalysis<MachineDominatorTree>();

  if (EnableSubRegLiveness && MF->getSubtarget().enableSubRegLiveness())
    MRI->enableSubRegLiveness(true);

  if (!LRCalc)
    LRCalc = new LiveRangeCalc();

  // Allocate space for all virtual registers.
  VirtRegIntervals.resize(MRI->getNumVirtRegs());

  computeVirtRegs();
  computeRegMasks();
  computeLiveInRegUnits();

  if (EnablePrecomputePhysRegs) {
    // For stress testing, precompute live ranges of all physical register
    // units, including reserved registers.
    for (unsigned i = 0, e = TRI->getNumRegUnits(); i != e; ++i)
      getRegUnit(i);
  }
  DEBUG(dump());
  return true;
}

/// print - Implement the dump method.
void LiveIntervals::print(raw_ostream &OS, const Module* ) const {
  OS << "********** INTERVALS **********\n";

  // Dump the regunits.
  for (unsigned i = 0, e = RegUnitRanges.size(); i != e; ++i)
    if (LiveRange *LR = RegUnitRanges[i])
      OS << PrintRegUnit(i, TRI) << ' ' << *LR << '\n';

  // Dump the virtregs.
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if (hasInterval(Reg))
      OS << getInterval(Reg) << '\n';
  }

  OS << "RegMasks:";
  for (unsigned i = 0, e = RegMaskSlots.size(); i != e; ++i)
    OS << ' ' << RegMaskSlots[i];
  OS << '\n';

  printInstrs(OS);
}

void LiveIntervals::printInstrs(raw_ostream &OS) const {
  OS << "********** MACHINEINSTRS **********\n";
  MF->print(OS, Indexes);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void LiveIntervals::dumpInstrs() const {
  printInstrs(dbgs());
}
#endif

LiveInterval* LiveIntervals::createInterval(unsigned reg) {
  float Weight = TargetRegisterInfo::isPhysicalRegister(reg) ?
                  llvm::huge_valf : 0.0F;
  return new LiveInterval(reg, Weight);
}


/// computeVirtRegInterval - Compute the live interval of a virtual register,
/// based on defs and uses.
void LiveIntervals::computeVirtRegInterval(LiveInterval &LI) {
  assert(LRCalc && "LRCalc not initialized.");
  assert(LI.empty() && "Should only compute empty intervals.");
  bool ShouldTrackSubRegLiveness = MRI->shouldTrackSubRegLiveness(LI.reg);
  LRCalc->reset(MF, getSlotIndexes(), DomTree, &getVNInfoAllocator());
  LRCalc->calculate(LI, ShouldTrackSubRegLiveness);
  bool SeparatedComponents = computeDeadValues(LI, nullptr);
  if (SeparatedComponents) {
    assert(ShouldTrackSubRegLiveness
           && "Separated components should only occur for unused subreg defs");
    SmallVector<LiveInterval*, 8> SplitLIs;
    splitSeparateComponents(LI, SplitLIs);
  }
}

void LiveIntervals::computeVirtRegs() {
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    createAndComputeVirtRegInterval(Reg);
  }
}

void LiveIntervals::computeRegMasks() {
  RegMaskBlocks.resize(MF->getNumBlockIDs());

  // Find all instructions with regmask operands.
  for (MachineBasicBlock &MBB : *MF) {
    std::pair<unsigned, unsigned> &RMB = RegMaskBlocks[MBB.getNumber()];
    RMB.first = RegMaskSlots.size();

    // Some block starts, such as EH funclets, create masks.
    if (const uint32_t *Mask = MBB.getBeginClobberMask(TRI)) {
      RegMaskSlots.push_back(Indexes->getMBBStartIdx(&MBB));
      RegMaskBits.push_back(Mask);
    }

    for (MachineInstr &MI : MBB) {
      for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isRegMask())
          continue;
        RegMaskSlots.push_back(Indexes->getInstructionIndex(MI).getRegSlot());
        RegMaskBits.push_back(MO.getRegMask());
      }
    }

    // Some block ends, such as funclet returns, create masks. Put the mask on
    // the last instruction of the block, because MBB slot index intervals are
    // half-open.
    if (const uint32_t *Mask = MBB.getEndClobberMask(TRI)) {
      assert(!MBB.empty() && "empty return block?");
      RegMaskSlots.push_back(
          Indexes->getInstructionIndex(MBB.back()).getRegSlot());
      RegMaskBits.push_back(Mask);
    }

    // Compute the number of register mask instructions in this block.
    RMB.second = RegMaskSlots.size() - RMB.first;
  }
}

//===----------------------------------------------------------------------===//
//                           Register Unit Liveness
//===----------------------------------------------------------------------===//
//
// Fixed interference typically comes from ABI boundaries: Function arguments
// and return values are passed in fixed registers, and so are exception
// pointers entering landing pads. Certain instructions require values to be
// present in specific registers. That is also represented through fixed
// interference.
//

/// computeRegUnitInterval - Compute the live range of a register unit, based
/// on the uses and defs of aliasing registers.  The range should be empty,
/// or contain only dead phi-defs from ABI blocks.
void LiveIntervals::computeRegUnitRange(LiveRange &LR, unsigned Unit) {
  assert(LRCalc && "LRCalc not initialized.");
  LRCalc->reset(MF, getSlotIndexes(), DomTree, &getVNInfoAllocator());

  // The physregs aliasing Unit are the roots and their super-registers.
  // Create all values as dead defs before extending to uses. Note that roots
  // may share super-registers. That's OK because createDeadDefs() is
  // idempotent. It is very rare for a register unit to have multiple roots, so
  // uniquing super-registers is probably not worthwhile.
  for (MCRegUnitRootIterator Roots(Unit, TRI); Roots.isValid(); ++Roots) {
    for (MCSuperRegIterator Supers(*Roots, TRI, /*IncludeSelf=*/true);
         Supers.isValid(); ++Supers) {
      if (!MRI->reg_empty(*Supers))
        LRCalc->createDeadDefs(LR, *Supers);
    }
  }

  // Now extend LR to reach all uses.
  // Ignore uses of reserved registers. We only track defs of those.
  for (MCRegUnitRootIterator Roots(Unit, TRI); Roots.isValid(); ++Roots) {
    for (MCSuperRegIterator Supers(*Roots, TRI, /*IncludeSelf=*/true);
         Supers.isValid(); ++Supers) {
      unsigned Reg = *Supers;
      if (!MRI->isReserved(Reg) && !MRI->reg_empty(Reg))
        LRCalc->extendToUses(LR, Reg);
    }
  }

  // Flush the segment set to the segment vector.
  if (UseSegmentSetForPhysRegs)
    LR.flushSegmentSet();
}


/// computeLiveInRegUnits - Precompute the live ranges of any register units
/// that are live-in to an ABI block somewhere. Register values can appear
/// without a corresponding def when entering the entry block or a landing pad.
///
void LiveIntervals::computeLiveInRegUnits() {
  RegUnitRanges.resize(TRI->getNumRegUnits());
  DEBUG(dbgs() << "Computing live-in reg-units in ABI blocks.\n");

  // Keep track of the live range sets allocated.
  SmallVector<unsigned, 8> NewRanges;

  // Check all basic blocks for live-ins.
  for (MachineFunction::const_iterator MFI = MF->begin(), MFE = MF->end();
       MFI != MFE; ++MFI) {
    const MachineBasicBlock *MBB = &*MFI;

    // We only care about ABI blocks: Entry + landing pads.
    if ((MFI != MF->begin() && !MBB->isEHPad()) || MBB->livein_empty())
      continue;

    // Create phi-defs at Begin for all live-in registers.
    SlotIndex Begin = Indexes->getMBBStartIdx(MBB);
    DEBUG(dbgs() << Begin << "\tBB#" << MBB->getNumber());
    for (const auto &LI : MBB->liveins()) {
      for (MCRegUnitIterator Units(LI.PhysReg, TRI); Units.isValid(); ++Units) {
        unsigned Unit = *Units;
        LiveRange *LR = RegUnitRanges[Unit];
        if (!LR) {
          // Use segment set to speed-up initial computation of the live range.
          LR = RegUnitRanges[Unit] = new LiveRange(UseSegmentSetForPhysRegs);
          NewRanges.push_back(Unit);
        }
        VNInfo *VNI = LR->createDeadDef(Begin, getVNInfoAllocator());
        (void)VNI;
        DEBUG(dbgs() << ' ' << PrintRegUnit(Unit, TRI) << '#' << VNI->id);
      }
    }
    DEBUG(dbgs() << '\n');
  }
  DEBUG(dbgs() << "Created " << NewRanges.size() << " new intervals.\n");

  // Compute the 'normal' part of the ranges.
  for (unsigned i = 0, e = NewRanges.size(); i != e; ++i) {
    unsigned Unit = NewRanges[i];
    computeRegUnitRange(*RegUnitRanges[Unit], Unit);
  }
}


static void createSegmentsForValues(LiveRange &LR,
      iterator_range<LiveInterval::vni_iterator> VNIs) {
  for (auto VNI : VNIs) {
    if (VNI->isUnused())
      continue;
    SlotIndex Def = VNI->def;
    LR.addSegment(LiveRange::Segment(Def, Def.getDeadSlot(), VNI));
  }
}

typedef SmallVector<std::pair<SlotIndex, VNInfo*>, 16> ShrinkToUsesWorkList;

static void extendSegmentsToUses(LiveRange &LR, const SlotIndexes &Indexes,
                                 ShrinkToUsesWorkList &WorkList,
                                 const LiveRange &OldRange) {
  // Keep track of the PHIs that are in use.
  SmallPtrSet<VNInfo*, 8> UsedPHIs;
  // Blocks that have already been added to WorkList as live-out.
  SmallPtrSet<MachineBasicBlock*, 16> LiveOut;

  // Extend intervals to reach all uses in WorkList.
  while (!WorkList.empty()) {
    SlotIndex Idx = WorkList.back().first;
    VNInfo *VNI = WorkList.back().second;
    WorkList.pop_back();
    const MachineBasicBlock *MBB = Indexes.getMBBFromIndex(Idx.getPrevSlot());
    SlotIndex BlockStart = Indexes.getMBBStartIdx(MBB);

    // Extend the live range for VNI to be live at Idx.
    if (VNInfo *ExtVNI = LR.extendInBlock(BlockStart, Idx)) {
      assert(ExtVNI == VNI && "Unexpected existing value number");
      (void)ExtVNI;
      // Is this a PHIDef we haven't seen before?
      if (!VNI->isPHIDef() || VNI->def != BlockStart ||
          !UsedPHIs.insert(VNI).second)
        continue;
      // The PHI is live, make sure the predecessors are live-out.
      for (auto &Pred : MBB->predecessors()) {
        if (!LiveOut.insert(Pred).second)
          continue;
        SlotIndex Stop = Indexes.getMBBEndIdx(Pred);
        // A predecessor is not required to have a live-out value for a PHI.
        if (VNInfo *PVNI = OldRange.getVNInfoBefore(Stop))
          WorkList.push_back(std::make_pair(Stop, PVNI));
      }
      continue;
    }

    // VNI is live-in to MBB.
    DEBUG(dbgs() << " live-in at " << BlockStart << '\n');
    LR.addSegment(LiveRange::Segment(BlockStart, Idx, VNI));

    // Make sure VNI is live-out from the predecessors.
    for (auto &Pred : MBB->predecessors()) {
      if (!LiveOut.insert(Pred).second)
        continue;
      SlotIndex Stop = Indexes.getMBBEndIdx(Pred);
      assert(OldRange.getVNInfoBefore(Stop) == VNI &&
             "Wrong value out of predecessor");
      WorkList.push_back(std::make_pair(Stop, VNI));
    }
  }
}

bool LiveIntervals::shrinkToUses(LiveInterval *li,
                                 SmallVectorImpl<MachineInstr*> *dead) {
  DEBUG(dbgs() << "Shrink: " << *li << '\n');
  assert(TargetRegisterInfo::isVirtualRegister(li->reg)
         && "Can only shrink virtual registers");

  // Shrink subregister live ranges.
  bool NeedsCleanup = false;
  for (LiveInterval::SubRange &S : li->subranges()) {
    shrinkToUses(S, li->reg);
    if (S.empty())
      NeedsCleanup = true;
  }
  if (NeedsCleanup)
    li->removeEmptySubRanges();

  // Find all the values used, including PHI kills.
  ShrinkToUsesWorkList WorkList;

  // Visit all instructions reading li->reg.
  for (MachineRegisterInfo::reg_instr_iterator
       I = MRI->reg_instr_begin(li->reg), E = MRI->reg_instr_end();
       I != E; ) {
    MachineInstr *UseMI = &*(I++);
    if (UseMI->isDebugValue() || !UseMI->readsVirtualRegister(li->reg))
      continue;
    SlotIndex Idx = getInstructionIndex(*UseMI).getRegSlot();
    LiveQueryResult LRQ = li->Query(Idx);
    VNInfo *VNI = LRQ.valueIn();
    if (!VNI) {
      // This shouldn't happen: readsVirtualRegister returns true, but there is
      // no live value. It is likely caused by a target getting <undef> flags
      // wrong.
      DEBUG(dbgs() << Idx << '\t' << *UseMI
                   << "Warning: Instr claims to read non-existent value in "
                    << *li << '\n');
      continue;
    }
    // Special case: An early-clobber tied operand reads and writes the
    // register one slot early.
    if (VNInfo *DefVNI = LRQ.valueDefined())
      Idx = DefVNI->def;

    WorkList.push_back(std::make_pair(Idx, VNI));
  }

  // Create new live ranges with only minimal live segments per def.
  LiveRange NewLR;
  createSegmentsForValues(NewLR, make_range(li->vni_begin(), li->vni_end()));
  extendSegmentsToUses(NewLR, *Indexes, WorkList, *li);

  // Move the trimmed segments back.
  li->segments.swap(NewLR.segments);

  // Handle dead values.
  bool CanSeparate = computeDeadValues(*li, dead);
  DEBUG(dbgs() << "Shrunk: " << *li << '\n');
  return CanSeparate;
}

bool LiveIntervals::computeDeadValues(LiveInterval &LI,
                                      SmallVectorImpl<MachineInstr*> *dead) {
  bool MayHaveSplitComponents = false;
  for (auto VNI : LI.valnos) {
    if (VNI->isUnused())
      continue;
    SlotIndex Def = VNI->def;
    LiveRange::iterator I = LI.FindSegmentContaining(Def);
    assert(I != LI.end() && "Missing segment for VNI");

    // Is the register live before? Otherwise we may have to add a read-undef
    // flag for subregister defs.
    bool DeadBeforeDef = false;
    unsigned VReg = LI.reg;
    if (MRI->shouldTrackSubRegLiveness(VReg)) {
      if ((I == LI.begin() || std::prev(I)->end < Def) && !VNI->isPHIDef()) {
        MachineInstr *MI = getInstructionFromIndex(Def);
        MI->setRegisterDefReadUndef(VReg);
        DeadBeforeDef = true;
      }
    }

    if (I->end != Def.getDeadSlot())
      continue;
    if (VNI->isPHIDef()) {
      // This is a dead PHI. Remove it.
      VNI->markUnused();
      LI.removeSegment(I);
      DEBUG(dbgs() << "Dead PHI at " << Def << " may separate interval\n");
      MayHaveSplitComponents = true;
    } else {
      // This is a dead def. Make sure the instruction knows.
      MachineInstr *MI = getInstructionFromIndex(Def);
      assert(MI && "No instruction defining live value");
      MI->addRegisterDead(VReg, TRI);

      // If we have a dead def that is completely separate from the rest of
      // the liverange then we rewrite it to use a different VReg to not violate
      // the rule that the liveness of a virtual register forms a connected
      // component. This should only happen if subregister liveness is tracked.
      if (DeadBeforeDef)
        MayHaveSplitComponents = true;

      if (dead && MI->allDefsAreDead()) {
        DEBUG(dbgs() << "All defs dead: " << Def << '\t' << *MI);
        dead->push_back(MI);
      }
    }
  }
  return MayHaveSplitComponents;
}

void LiveIntervals::shrinkToUses(LiveInterval::SubRange &SR, unsigned Reg)
{
  DEBUG(dbgs() << "Shrink: " << SR << '\n');
  assert(TargetRegisterInfo::isVirtualRegister(Reg)
         && "Can only shrink virtual registers");
  // Find all the values used, including PHI kills.
  ShrinkToUsesWorkList WorkList;

  // Visit all instructions reading Reg.
  SlotIndex LastIdx;
  for (MachineOperand &MO : MRI->reg_operands(Reg)) {
    MachineInstr *UseMI = MO.getParent();
    if (UseMI->isDebugValue())
      continue;
    // Maybe the operand is for a subregister we don't care about.
    unsigned SubReg = MO.getSubReg();
    if (SubReg != 0) {
      LaneBitmask LaneMask = TRI->getSubRegIndexLaneMask(SubReg);
      if ((LaneMask & SR.LaneMask) == 0)
        continue;
    }
    // We only need to visit each instruction once.
    SlotIndex Idx = getInstructionIndex(*UseMI).getRegSlot();
    if (Idx == LastIdx)
      continue;
    LastIdx = Idx;

    LiveQueryResult LRQ = SR.Query(Idx);
    VNInfo *VNI = LRQ.valueIn();
    // For Subranges it is possible that only undef values are left in that
    // part of the subregister, so there is no real liverange at the use
    if (!VNI)
      continue;

    // Special case: An early-clobber tied operand reads and writes the
    // register one slot early.
    if (VNInfo *DefVNI = LRQ.valueDefined())
      Idx = DefVNI->def;

    WorkList.push_back(std::make_pair(Idx, VNI));
  }

  // Create a new live ranges with only minimal live segments per def.
  LiveRange NewLR;
  createSegmentsForValues(NewLR, make_range(SR.vni_begin(), SR.vni_end()));
  extendSegmentsToUses(NewLR, *Indexes, WorkList, SR);

  // Move the trimmed ranges back.
  SR.segments.swap(NewLR.segments);

  // Remove dead PHI value numbers
  for (auto VNI : SR.valnos) {
    if (VNI->isUnused())
      continue;
    const LiveRange::Segment *Segment = SR.getSegmentContaining(VNI->def);
    assert(Segment != nullptr && "Missing segment for VNI");
    if (Segment->end != VNI->def.getDeadSlot())
      continue;
    if (VNI->isPHIDef()) {
      // This is a dead PHI. Remove it.
      VNI->markUnused();
      SR.removeSegment(*Segment);
      DEBUG(dbgs() << "Dead PHI at " << VNI->def << " may separate interval\n");
    }
  }

  DEBUG(dbgs() << "Shrunk: " << SR << '\n');
}

void LiveIntervals::extendToIndices(LiveRange &LR,
                                    ArrayRef<SlotIndex> Indices) {
  assert(LRCalc && "LRCalc not initialized.");
  LRCalc->reset(MF, getSlotIndexes(), DomTree, &getVNInfoAllocator());
  for (unsigned i = 0, e = Indices.size(); i != e; ++i)
    LRCalc->extend(LR, Indices[i]);
}

void LiveIntervals::pruneValue(LiveRange &LR, SlotIndex Kill,
                               SmallVectorImpl<SlotIndex> *EndPoints) {
  LiveQueryResult LRQ = LR.Query(Kill);
  VNInfo *VNI = LRQ.valueOutOrDead();
  if (!VNI)
    return;

  MachineBasicBlock *KillMBB = Indexes->getMBBFromIndex(Kill);
  SlotIndex MBBEnd = Indexes->getMBBEndIdx(KillMBB);

  // If VNI isn't live out from KillMBB, the value is trivially pruned.
  if (LRQ.endPoint() < MBBEnd) {
    LR.removeSegment(Kill, LRQ.endPoint());
    if (EndPoints) EndPoints->push_back(LRQ.endPoint());
    return;
  }

  // VNI is live out of KillMBB.
  LR.removeSegment(Kill, MBBEnd);
  if (EndPoints) EndPoints->push_back(MBBEnd);

  // Find all blocks that are reachable from KillMBB without leaving VNI's live
  // range. It is possible that KillMBB itself is reachable, so start a DFS
  // from each successor.
  typedef SmallPtrSet<MachineBasicBlock*, 9> VisitedTy;
  VisitedTy Visited;
  for (MachineBasicBlock::succ_iterator
       SuccI = KillMBB->succ_begin(), SuccE = KillMBB->succ_end();
       SuccI != SuccE; ++SuccI) {
    for (df_ext_iterator<MachineBasicBlock*, VisitedTy>
         I = df_ext_begin(*SuccI, Visited), E = df_ext_end(*SuccI, Visited);
         I != E;) {
      MachineBasicBlock *MBB = *I;

      // Check if VNI is live in to MBB.
      SlotIndex MBBStart, MBBEnd;
      std::tie(MBBStart, MBBEnd) = Indexes->getMBBRange(MBB);
      LiveQueryResult LRQ = LR.Query(MBBStart);
      if (LRQ.valueIn() != VNI) {
        // This block isn't part of the VNI segment. Prune the search.
        I.skipChildren();
        continue;
      }

      // Prune the search if VNI is killed in MBB.
      if (LRQ.endPoint() < MBBEnd) {
        LR.removeSegment(MBBStart, LRQ.endPoint());
        if (EndPoints) EndPoints->push_back(LRQ.endPoint());
        I.skipChildren();
        continue;
      }

      // VNI is live through MBB.
      LR.removeSegment(MBBStart, MBBEnd);
      if (EndPoints) EndPoints->push_back(MBBEnd);
      ++I;
    }
  }
}

//===----------------------------------------------------------------------===//
// Register allocator hooks.
//

void LiveIntervals::addKillFlags(const VirtRegMap *VRM) {
  // Keep track of regunit ranges.
  SmallVector<std::pair<const LiveRange*, LiveRange::const_iterator>, 8> RU;
  // Keep track of subregister ranges.
  SmallVector<std::pair<const LiveInterval::SubRange*,
                        LiveRange::const_iterator>, 4> SRs;

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    const LiveInterval &LI = getInterval(Reg);
    if (LI.empty())
      continue;

    // Find the regunit intervals for the assigned register. They may overlap
    // the virtual register live range, cancelling any kills.
    RU.clear();
    for (MCRegUnitIterator Units(VRM->getPhys(Reg), TRI); Units.isValid();
         ++Units) {
      const LiveRange &RURange = getRegUnit(*Units);
      if (RURange.empty())
        continue;
      RU.push_back(std::make_pair(&RURange, RURange.find(LI.begin()->end)));
    }

    if (MRI->subRegLivenessEnabled()) {
      SRs.clear();
      for (const LiveInterval::SubRange &SR : LI.subranges()) {
        SRs.push_back(std::make_pair(&SR, SR.find(LI.begin()->end)));
      }
    }

    // Every instruction that kills Reg corresponds to a segment range end
    // point.
    for (LiveInterval::const_iterator RI = LI.begin(), RE = LI.end(); RI != RE;
         ++RI) {
      // A block index indicates an MBB edge.
      if (RI->end.isBlock())
        continue;
      MachineInstr *MI = getInstructionFromIndex(RI->end);
      if (!MI)
        continue;

      // Check if any of the regunits are live beyond the end of RI. That could
      // happen when a physreg is defined as a copy of a virtreg:
      //
      //   %EAX = COPY %vreg5
      //   FOO %vreg5         <--- MI, cancel kill because %EAX is live.
      //   BAR %EAX<kill>
      //
      // There should be no kill flag on FOO when %vreg5 is rewritten as %EAX.
      for (auto &RUP : RU) {
        const LiveRange &RURange = *RUP.first;
        LiveRange::const_iterator &I = RUP.second;
        if (I == RURange.end())
          continue;
        I = RURange.advanceTo(I, RI->end);
        if (I == RURange.end() || I->start >= RI->end)
          continue;
        // I is overlapping RI.
        goto CancelKill;
      }

      if (MRI->subRegLivenessEnabled()) {
        // When reading a partial undefined value we must not add a kill flag.
        // The regalloc might have used the undef lane for something else.
        // Example:
        //     %vreg1 = ...              ; R32: %vreg1
        //     %vreg2:high16 = ...       ; R64: %vreg2
        //        = read %vreg2<kill>    ; R64: %vreg2
        //        = read %vreg1          ; R32: %vreg1
        // The <kill> flag is correct for %vreg2, but the register allocator may
        // assign R0L to %vreg1, and R0 to %vreg2 because the low 32bits of R0
        // are actually never written by %vreg2. After assignment the <kill>
        // flag at the read instruction is invalid.
        LaneBitmask DefinedLanesMask;
        if (!SRs.empty()) {
          // Compute a mask of lanes that are defined.
          DefinedLanesMask = 0;
          for (auto &SRP : SRs) {
            const LiveInterval::SubRange &SR = *SRP.first;
            LiveRange::const_iterator &I = SRP.second;
            if (I == SR.end())
              continue;
            I = SR.advanceTo(I, RI->end);
            if (I == SR.end() || I->start >= RI->end)
              continue;
            // I is overlapping RI
            DefinedLanesMask |= SR.LaneMask;
          }
        } else
          DefinedLanesMask = ~0u;

        bool IsFullWrite = false;
        for (const MachineOperand &MO : MI->operands()) {
          if (!MO.isReg() || MO.getReg() != Reg)
            continue;
          if (MO.isUse()) {
            // Reading any undefined lanes?
            LaneBitmask UseMask = TRI->getSubRegIndexLaneMask(MO.getSubReg());
            if ((UseMask & ~DefinedLanesMask) != 0)
              goto CancelKill;
          } else if (MO.getSubReg() == 0) {
            // Writing to the full register?
            assert(MO.isDef());
            IsFullWrite = true;
          }
        }

        // If an instruction writes to a subregister, a new segment starts in
        // the LiveInterval. But as this is only overriding part of the register
        // adding kill-flags is not correct here after registers have been
        // assigned.
        if (!IsFullWrite) {
          // Next segment has to be adjacent in the subregister write case.
          LiveRange::const_iterator N = std::next(RI);
          if (N != LI.end() && N->start == RI->end)
            goto CancelKill;
        }
      }

      MI->addRegisterKilled(Reg, nullptr);
      continue;
CancelKill:
      MI->clearRegisterKills(Reg, nullptr);
    }
  }
}

MachineBasicBlock*
LiveIntervals::intervalIsInOneMBB(const LiveInterval &LI) const {
  // A local live range must be fully contained inside the block, meaning it is
  // defined and killed at instructions, not at block boundaries. It is not
  // live in or or out of any block.
  //
  // It is technically possible to have a PHI-defined live range identical to a
  // single block, but we are going to return false in that case.

  SlotIndex Start = LI.beginIndex();
  if (Start.isBlock())
    return nullptr;

  SlotIndex Stop = LI.endIndex();
  if (Stop.isBlock())
    return nullptr;

  // getMBBFromIndex doesn't need to search the MBB table when both indexes
  // belong to proper instructions.
  MachineBasicBlock *MBB1 = Indexes->getMBBFromIndex(Start);
  MachineBasicBlock *MBB2 = Indexes->getMBBFromIndex(Stop);
  return MBB1 == MBB2 ? MBB1 : nullptr;
}

bool
LiveIntervals::hasPHIKill(const LiveInterval &LI, const VNInfo *VNI) const {
  for (const VNInfo *PHI : LI.valnos) {
    if (PHI->isUnused() || !PHI->isPHIDef())
      continue;
    const MachineBasicBlock *PHIMBB = getMBBFromIndex(PHI->def);
    // Conservatively return true instead of scanning huge predecessor lists.
    if (PHIMBB->pred_size() > 100)
      return true;
    for (MachineBasicBlock::const_pred_iterator
         PI = PHIMBB->pred_begin(), PE = PHIMBB->pred_end(); PI != PE; ++PI)
      if (VNI == LI.getVNInfoBefore(Indexes->getMBBEndIdx(*PI)))
        return true;
  }
  return false;
}

float LiveIntervals::getSpillWeight(bool isDef, bool isUse,
                                    const MachineBlockFrequencyInfo *MBFI,
                                    const MachineInstr &MI) {
  BlockFrequency Freq = MBFI->getBlockFreq(MI.getParent());
  const float Scale = 1.0f / MBFI->getEntryFreq();
  return (isDef + isUse) * (Freq.getFrequency() * Scale);
}

LiveRange::Segment
LiveIntervals::addSegmentToEndOfBlock(unsigned reg, MachineInstr &startInst) {
  LiveInterval& Interval = createEmptyInterval(reg);
  VNInfo *VN = Interval.getNextValue(
      SlotIndex(getInstructionIndex(startInst).getRegSlot()),
      getVNInfoAllocator());
  LiveRange::Segment S(SlotIndex(getInstructionIndex(startInst).getRegSlot()),
                       getMBBEndIdx(startInst.getParent()), VN);
  Interval.addSegment(S);

  return S;
}


//===----------------------------------------------------------------------===//
//                          Register mask functions
//===----------------------------------------------------------------------===//

bool LiveIntervals::checkRegMaskInterference(LiveInterval &LI,
                                             BitVector &UsableRegs) {
  if (LI.empty())
    return false;
  LiveInterval::iterator LiveI = LI.begin(), LiveE = LI.end();

  // Use a smaller arrays for local live ranges.
  ArrayRef<SlotIndex> Slots;
  ArrayRef<const uint32_t*> Bits;
  if (MachineBasicBlock *MBB = intervalIsInOneMBB(LI)) {
    Slots = getRegMaskSlotsInBlock(MBB->getNumber());
    Bits = getRegMaskBitsInBlock(MBB->getNumber());
  } else {
    Slots = getRegMaskSlots();
    Bits = getRegMaskBits();
  }

  // We are going to enumerate all the register mask slots contained in LI.
  // Start with a binary search of RegMaskSlots to find a starting point.
  ArrayRef<SlotIndex>::iterator SlotI =
    std::lower_bound(Slots.begin(), Slots.end(), LiveI->start);
  ArrayRef<SlotIndex>::iterator SlotE = Slots.end();

  // No slots in range, LI begins after the last call.
  if (SlotI == SlotE)
    return false;

  bool Found = false;
  for (;;) {
    assert(*SlotI >= LiveI->start);
    // Loop over all slots overlapping this segment.
    while (*SlotI < LiveI->end) {
      // *SlotI overlaps LI. Collect mask bits.
      if (!Found) {
        // This is the first overlap. Initialize UsableRegs to all ones.
        UsableRegs.clear();
        UsableRegs.resize(TRI->getNumRegs(), true);
        Found = true;
      }
      // Remove usable registers clobbered by this mask.
      UsableRegs.clearBitsNotInMask(Bits[SlotI-Slots.begin()]);
      if (++SlotI == SlotE)
        return Found;
    }
    // *SlotI is beyond the current LI segment.
    LiveI = LI.advanceTo(LiveI, *SlotI);
    if (LiveI == LiveE)
      return Found;
    // Advance SlotI until it overlaps.
    while (*SlotI < LiveI->start)
      if (++SlotI == SlotE)
        return Found;
  }
}

//===----------------------------------------------------------------------===//
//                         IntervalUpdate class.
//===----------------------------------------------------------------------===//

// HMEditor is a toolkit used by handleMove to trim or extend live intervals.
class LiveIntervals::HMEditor {
private:
  LiveIntervals& LIS;
  const MachineRegisterInfo& MRI;
  const TargetRegisterInfo& TRI;
  SlotIndex OldIdx;
  SlotIndex NewIdx;
  SmallPtrSet<LiveRange*, 8> Updated;
  bool UpdateFlags;

public:
  HMEditor(LiveIntervals& LIS, const MachineRegisterInfo& MRI,
           const TargetRegisterInfo& TRI,
           SlotIndex OldIdx, SlotIndex NewIdx, bool UpdateFlags)
    : LIS(LIS), MRI(MRI), TRI(TRI), OldIdx(OldIdx), NewIdx(NewIdx),
      UpdateFlags(UpdateFlags) {}

  // FIXME: UpdateFlags is a workaround that creates live intervals for all
  // physregs, even those that aren't needed for regalloc, in order to update
  // kill flags. This is wasteful. Eventually, LiveVariables will strip all kill
  // flags, and postRA passes will use a live register utility instead.
  LiveRange *getRegUnitLI(unsigned Unit) {
    if (UpdateFlags)
      return &LIS.getRegUnit(Unit);
    return LIS.getCachedRegUnit(Unit);
  }

  /// Update all live ranges touched by MI, assuming a move from OldIdx to
  /// NewIdx.
  void updateAllRanges(MachineInstr *MI) {
    DEBUG(dbgs() << "handleMove " << OldIdx << " -> " << NewIdx << ": " << *MI);
    bool hasRegMask = false;
    for (MachineOperand &MO : MI->operands()) {
      if (MO.isRegMask())
        hasRegMask = true;
      if (!MO.isReg())
        continue;
      // Aggressively clear all kill flags.
      // They are reinserted by VirtRegRewriter.
      if (MO.isUse())
        MO.setIsKill(false);

      unsigned Reg = MO.getReg();
      if (!Reg)
        continue;
      if (TargetRegisterInfo::isVirtualRegister(Reg)) {
        LiveInterval &LI = LIS.getInterval(Reg);
        if (LI.hasSubRanges()) {
          unsigned SubReg = MO.getSubReg();
          LaneBitmask LaneMask = TRI.getSubRegIndexLaneMask(SubReg);
          for (LiveInterval::SubRange &S : LI.subranges()) {
            if ((S.LaneMask & LaneMask) == 0)
              continue;
            updateRange(S, Reg, S.LaneMask);
          }
        }
        updateRange(LI, Reg, 0);
        continue;
      }

      // For physregs, only update the regunits that actually have a
      // precomputed live range.
      for (MCRegUnitIterator Units(Reg, &TRI); Units.isValid(); ++Units)
        if (LiveRange *LR = getRegUnitLI(*Units))
          updateRange(*LR, *Units, 0);
    }
    if (hasRegMask)
      updateRegMaskSlots();
  }

private:
  /// Update a single live range, assuming an instruction has been moved from
  /// OldIdx to NewIdx.
  void updateRange(LiveRange &LR, unsigned Reg, LaneBitmask LaneMask) {
    if (!Updated.insert(&LR).second)
      return;
    DEBUG({
      dbgs() << "     ";
      if (TargetRegisterInfo::isVirtualRegister(Reg)) {
        dbgs() << PrintReg(Reg);
        if (LaneMask != 0)
          dbgs() << " L" << PrintLaneMask(LaneMask);
      } else {
        dbgs() << PrintRegUnit(Reg, &TRI);
      }
      dbgs() << ":\t" << LR << '\n';
    });
    if (SlotIndex::isEarlierInstr(OldIdx, NewIdx))
      handleMoveDown(LR);
    else
      handleMoveUp(LR, Reg, LaneMask);
    DEBUG(dbgs() << "        -->\t" << LR << '\n');
    LR.verify();
  }

  /// Update LR to reflect an instruction has been moved downwards from OldIdx
  /// to NewIdx (OldIdx < NewIdx).
  void handleMoveDown(LiveRange &LR) {
    LiveRange::iterator E = LR.end();
    // Segment going into OldIdx.
    LiveRange::iterator OldIdxIn = LR.find(OldIdx.getBaseIndex());

    // No value live before or after OldIdx? Nothing to do.
    if (OldIdxIn == E || SlotIndex::isEarlierInstr(OldIdx, OldIdxIn->start))
      return;

    LiveRange::iterator OldIdxOut;
    // Do we have a value live-in to OldIdx?
    if (SlotIndex::isEarlierInstr(OldIdxIn->start, OldIdx)) {
      // If the live-in value already extends to NewIdx, there is nothing to do.
      if (SlotIndex::isEarlierEqualInstr(NewIdx, OldIdxIn->end))
        return;
      // Aggressively remove all kill flags from the old kill point.
      // Kill flags shouldn't be used while live intervals exist, they will be
      // reinserted by VirtRegRewriter.
      if (MachineInstr *KillMI = LIS.getInstructionFromIndex(OldIdxIn->end))
        for (MIBundleOperands MO(*KillMI); MO.isValid(); ++MO)
          if (MO->isReg() && MO->isUse())
            MO->setIsKill(false);

      // Is there a def before NewIdx which is not OldIdx?
      LiveRange::iterator Next = std::next(OldIdxIn);
      if (Next != E && !SlotIndex::isSameInstr(OldIdx, Next->start) &&
          SlotIndex::isEarlierInstr(Next->start, NewIdx)) {
        // If we are here then OldIdx was just a use but not a def. We only have
        // to ensure liveness extends to NewIdx.
        LiveRange::iterator NewIdxIn =
          LR.advanceTo(Next, NewIdx.getBaseIndex());
        // Extend the segment before NewIdx if necessary.
        if (NewIdxIn == E ||
            !SlotIndex::isEarlierInstr(NewIdxIn->start, NewIdx)) {
          LiveRange::iterator Prev = std::prev(NewIdxIn);
          Prev->end = NewIdx.getRegSlot();
        }
        return;
      }

      // Adjust OldIdxIn->end to reach NewIdx. This may temporarily make LR
      // invalid by overlapping ranges.
      bool isKill = SlotIndex::isSameInstr(OldIdx, OldIdxIn->end);
      OldIdxIn->end = NewIdx.getRegSlot(OldIdxIn->end.isEarlyClobber());
      // If this was not a kill, then there was no def and we're done.
      if (!isKill)
        return;

      // Did we have a Def at OldIdx?
      OldIdxOut = Next;
      if (OldIdxOut == E || !SlotIndex::isSameInstr(OldIdx, OldIdxOut->start))
        return;
    } else {
      OldIdxOut = OldIdxIn;
    }

    // If we are here then there is a Definition at OldIdx. OldIdxOut points
    // to the segment starting there.
    assert(OldIdxOut != E && SlotIndex::isSameInstr(OldIdx, OldIdxOut->start) &&
           "No def?");
    VNInfo *OldIdxVNI = OldIdxOut->valno;
    assert(OldIdxVNI->def == OldIdxOut->start && "Inconsistent def");

    // If the defined value extends beyond NewIdx, just move the beginning
    // of the segment to NewIdx.
    SlotIndex NewIdxDef = NewIdx.getRegSlot(OldIdxOut->start.isEarlyClobber());
    if (SlotIndex::isEarlierInstr(NewIdxDef, OldIdxOut->end)) {
      OldIdxVNI->def = NewIdxDef;
      OldIdxOut->start = OldIdxVNI->def;
      return;
    }

    // If we are here then we have a Definition at OldIdx which ends before
    // NewIdx.

    // Is there an existing Def at NewIdx?
    LiveRange::iterator AfterNewIdx
      = LR.advanceTo(OldIdxOut, NewIdx.getRegSlot());
    bool OldIdxDefIsDead = OldIdxOut->end.isDead();
    if (!OldIdxDefIsDead &&
        SlotIndex::isEarlierInstr(OldIdxOut->end, NewIdxDef)) {
      // OldIdx is not a dead def, and NewIdxDef is inside a new interval.
      VNInfo *DefVNI;
      if (OldIdxOut != LR.begin() &&
          !SlotIndex::isEarlierInstr(std::prev(OldIdxOut)->end,
                                     OldIdxOut->start)) {
        // There is no gap between OldIdxOut and its predecessor anymore,
        // merge them.
        LiveRange::iterator IPrev = std::prev(OldIdxOut);
        DefVNI = OldIdxVNI;
        IPrev->end = OldIdxOut->end;
      } else {
        // The value is live in to OldIdx
        LiveRange::iterator INext = std::next(OldIdxOut);
        assert(INext != E && "Must have following segment");
        // We merge OldIdxOut and its successor. As we're dealing with subreg
        // reordering, there is always a successor to OldIdxOut in the same BB
        // We don't need INext->valno anymore and will reuse for the new segment
        // we create later.
        DefVNI = OldIdxVNI;
        INext->start = OldIdxOut->end;
        INext->valno->def = INext->start;
      }
      // If NewIdx is behind the last segment, extend that and append a new one.
      if (AfterNewIdx == E) {
        // OldIdxOut is undef at this point, Slide (OldIdxOut;AfterNewIdx] up
        // one position.
        //    |-  ?/OldIdxOut -| |- X0 -| ... |- Xn -| end
        // => |- X0/OldIdxOut -| ... |- Xn -| |- undef/NewS -| end
        std::copy(std::next(OldIdxOut), E, OldIdxOut);
        // The last segment is undefined now, reuse it for a dead def.
        LiveRange::iterator NewSegment = std::prev(E);
        *NewSegment = LiveRange::Segment(NewIdxDef, NewIdxDef.getDeadSlot(),
                                         DefVNI);
        DefVNI->def = NewIdxDef;

        LiveRange::iterator Prev = std::prev(NewSegment);
        Prev->end = NewIdxDef;
      } else {
        // OldIdxOut is undef at this point, Slide (OldIdxOut;AfterNewIdx] up
        // one position.
        //    |-  ?/OldIdxOut -| |- X0 -| ... |- Xn/AfterNewIdx -| |- Next -|
        // => |- X0/OldIdxOut -| ... |- Xn -| |- Xn/AfterNewIdx -| |- Next -|
        std::copy(std::next(OldIdxOut), std::next(AfterNewIdx), OldIdxOut);
        LiveRange::iterator Prev = std::prev(AfterNewIdx);
        // We have two cases:
        if (SlotIndex::isEarlierInstr(Prev->start, NewIdxDef)) {
          // Case 1: NewIdx is inside a liverange. Split this liverange at
          // NewIdxDef into the segment "Prev" followed by "NewSegment".
          LiveRange::iterator NewSegment = AfterNewIdx;
          *NewSegment = LiveRange::Segment(NewIdxDef, Prev->end, Prev->valno);
          Prev->valno->def = NewIdxDef;

          *Prev = LiveRange::Segment(Prev->start, NewIdxDef, DefVNI);
          DefVNI->def = Prev->start;
        } else {
          // Case 2: NewIdx is in a lifetime hole. Keep AfterNewIdx as is and
          // turn Prev into a segment from NewIdx to AfterNewIdx->start.
          *Prev = LiveRange::Segment(NewIdxDef, AfterNewIdx->start, DefVNI);
          DefVNI->def = NewIdxDef;
          assert(DefVNI != AfterNewIdx->valno);
        }
      }
      return;
    }

    if (AfterNewIdx != E &&
        SlotIndex::isSameInstr(AfterNewIdx->start, NewIdxDef)) {
      // There is an existing def at NewIdx. The def at OldIdx is coalesced into
      // that value.
      assert(AfterNewIdx->valno != OldIdxVNI && "Multiple defs of value?");
      LR.removeValNo(OldIdxVNI);
    } else {
      // There was no existing def at NewIdx. We need to create a dead def
      // at NewIdx. Shift segments over the old OldIdxOut segment, this frees
      // a new segment at the place where we want to construct the dead def.
      //    |- OldIdxOut -| |- X0 -| ... |- Xn -| |- AfterNewIdx -|
      // => |- X0/OldIdxOut -| ... |- Xn -| |- undef/NewS. -| |- AfterNewIdx -|
      assert(AfterNewIdx != OldIdxOut && "Inconsistent iterators");
      std::copy(std::next(OldIdxOut), AfterNewIdx, OldIdxOut);
      // We can reuse OldIdxVNI now.
      LiveRange::iterator NewSegment = std::prev(AfterNewIdx);
      VNInfo *NewSegmentVNI = OldIdxVNI;
      NewSegmentVNI->def = NewIdxDef;
      *NewSegment = LiveRange::Segment(NewIdxDef, NewIdxDef.getDeadSlot(),
                                       NewSegmentVNI);
    }
  }

  /// Update LR to reflect an instruction has been moved upwards from OldIdx
  /// to NewIdx (NewIdx < OldIdx).
  void handleMoveUp(LiveRange &LR, unsigned Reg, LaneBitmask LaneMask) {
    LiveRange::iterator E = LR.end();
    // Segment going into OldIdx.
    LiveRange::iterator OldIdxIn = LR.find(OldIdx.getBaseIndex());

    // No value live before or after OldIdx? Nothing to do.
    if (OldIdxIn == E || SlotIndex::isEarlierInstr(OldIdx, OldIdxIn->start))
      return;

    LiveRange::iterator OldIdxOut;
    // Do we have a value live-in to OldIdx?
    if (SlotIndex::isEarlierInstr(OldIdxIn->start, OldIdx)) {
      // If the live-in value isn't killed here, then we have no Def at
      // OldIdx, moreover the value must be live at NewIdx so there is nothing
      // to do.
      bool isKill = SlotIndex::isSameInstr(OldIdx, OldIdxIn->end);
      if (!isKill)
        return;

      // At this point we have to move OldIdxIn->end back to the nearest
      // previous use or (dead-)def but no further than NewIdx.
      SlotIndex DefBeforeOldIdx
        = std::max(OldIdxIn->start.getDeadSlot(),
                   NewIdx.getRegSlot(OldIdxIn->end.isEarlyClobber()));
      OldIdxIn->end = findLastUseBefore(DefBeforeOldIdx, Reg, LaneMask);

      // Did we have a Def at OldIdx? If not we are done now.
      OldIdxOut = std::next(OldIdxIn);
      if (OldIdxOut == E || !SlotIndex::isSameInstr(OldIdx, OldIdxOut->start))
        return;
    } else {
      OldIdxOut = OldIdxIn;
      OldIdxIn = OldIdxOut != LR.begin() ? std::prev(OldIdxOut) : E;
    }

    // If we are here then there is a Definition at OldIdx. OldIdxOut points
    // to the segment starting there.
    assert(OldIdxOut != E && SlotIndex::isSameInstr(OldIdx, OldIdxOut->start) &&
           "No def?");
    VNInfo *OldIdxVNI = OldIdxOut->valno;
    assert(OldIdxVNI->def == OldIdxOut->start && "Inconsistent def");
    bool OldIdxDefIsDead = OldIdxOut->end.isDead();

    // Is there an existing def at NewIdx?
    SlotIndex NewIdxDef = NewIdx.getRegSlot(OldIdxOut->start.isEarlyClobber());
    LiveRange::iterator NewIdxOut = LR.find(NewIdx.getRegSlot());
    if (SlotIndex::isSameInstr(NewIdxOut->start, NewIdx)) {
      assert(NewIdxOut->valno != OldIdxVNI &&
             "Same value defined more than once?");
      // If OldIdx was a dead def remove it.
      if (!OldIdxDefIsDead) {
        // Remove segment starting at NewIdx and move begin of OldIdxOut to
        // NewIdx so it can take its place.
        OldIdxVNI->def = NewIdxDef;
        OldIdxOut->start = NewIdxDef;
        LR.removeValNo(NewIdxOut->valno);
      } else {
        // Simply remove the dead def at OldIdx.
        LR.removeValNo(OldIdxVNI);
      }
    } else {
      // Previously nothing was live after NewIdx, so all we have to do now is
      // move the begin of OldIdxOut to NewIdx.
      if (!OldIdxDefIsDead) {
        // Do we have any intermediate Defs between OldIdx and NewIdx?
        if (OldIdxIn != E &&
            SlotIndex::isEarlierInstr(NewIdxDef, OldIdxIn->start)) {
          // OldIdx is not a dead def and NewIdx is before predecessor start.
          LiveRange::iterator NewIdxIn = NewIdxOut;
          assert(NewIdxIn == LR.find(NewIdx.getBaseIndex()));
          const SlotIndex SplitPos = NewIdxDef;

          // Merge the OldIdxIn and OldIdxOut segments into OldIdxOut.
          *OldIdxOut = LiveRange::Segment(OldIdxIn->start, OldIdxOut->end,
                                          OldIdxIn->valno);
          // OldIdxIn and OldIdxVNI are now undef and can be overridden.
          // We Slide [NewIdxIn, OldIdxIn) down one position.
          //    |- X0/NewIdxIn -| ... |- Xn-1 -||- Xn/OldIdxIn -||- OldIdxOut -|
          // => |- undef/NexIdxIn -| |- X0 -| ... |- Xn-1 -| |- Xn/OldIdxOut -|
          std::copy_backward(NewIdxIn, OldIdxIn, OldIdxOut);
          // NewIdxIn is now considered undef so we can reuse it for the moved
          // value.
          LiveRange::iterator NewSegment = NewIdxIn;
          LiveRange::iterator Next = std::next(NewSegment);
          NewSegment->valno = OldIdxVNI;
          if (SlotIndex::isEarlierInstr(Next->start, NewIdx)) {
            // There is no gap between NewSegment and its predecessor.
            *NewSegment = LiveRange::Segment(Next->start, SplitPos,
                                             NewSegment->valno);
            NewSegment->valno->def = Next->start;

            *Next = LiveRange::Segment(SplitPos, Next->end, Next->valno);
            Next->valno->def = SplitPos;
          } else {
            // There is a gap between NewSegment and its predecessor
            // Value becomes live in.
            *NewSegment = LiveRange::Segment(SplitPos, Next->start,
                                             NewSegment->valno);
            NewSegment->valno->def = SplitPos;
          }
        } else {
          // Leave the end point of a live def.
          OldIdxOut->start = NewIdxDef;
          OldIdxVNI->def = NewIdxDef;
          if (OldIdxIn != E && SlotIndex::isEarlierInstr(NewIdx, OldIdxIn->end))
            OldIdxIn->end = NewIdx.getRegSlot();
        }
      } else {
        // OldIdxVNI is a dead def. It may have been moved across other values
        // in LR, so move OldIdxOut up to NewIdxOut. Slide [NewIdxOut;OldIdxOut)
        // down one position.
        //    |- X0/NewIdxOut -| ... |- Xn-1 -| |- Xn/OldIdxOut -| |- next - |
        // => |- undef/NewIdxOut -| |- X0 -| ... |- Xn-1 -| |- next -|
        std::copy_backward(NewIdxOut, OldIdxOut, std::next(OldIdxOut));
        // OldIdxVNI can be reused now to build a new dead def segment.
        LiveRange::iterator NewSegment = NewIdxOut;
        VNInfo *NewSegmentVNI = OldIdxVNI;
        *NewSegment = LiveRange::Segment(NewIdxDef, NewIdxDef.getDeadSlot(),
                                         NewSegmentVNI);
        NewSegmentVNI->def = NewIdxDef;
      }
    }
  }

  void updateRegMaskSlots() {
    SmallVectorImpl<SlotIndex>::iterator RI =
      std::lower_bound(LIS.RegMaskSlots.begin(), LIS.RegMaskSlots.end(),
                       OldIdx);
    assert(RI != LIS.RegMaskSlots.end() && *RI == OldIdx.getRegSlot() &&
           "No RegMask at OldIdx.");
    *RI = NewIdx.getRegSlot();
    assert((RI == LIS.RegMaskSlots.begin() ||
            SlotIndex::isEarlierInstr(*std::prev(RI), *RI)) &&
           "Cannot move regmask instruction above another call");
    assert((std::next(RI) == LIS.RegMaskSlots.end() ||
            SlotIndex::isEarlierInstr(*RI, *std::next(RI))) &&
           "Cannot move regmask instruction below another call");
  }

  // Return the last use of reg between NewIdx and OldIdx.
  SlotIndex findLastUseBefore(SlotIndex Before, unsigned Reg,
                              LaneBitmask LaneMask) {
    if (TargetRegisterInfo::isVirtualRegister(Reg)) {
      SlotIndex LastUse = Before;
      for (MachineOperand &MO : MRI.use_nodbg_operands(Reg)) {
        unsigned SubReg = MO.getSubReg();
        if (SubReg != 0 && LaneMask != 0
            && (TRI.getSubRegIndexLaneMask(SubReg) & LaneMask) == 0)
          continue;

        const MachineInstr &MI = *MO.getParent();
        SlotIndex InstSlot = LIS.getSlotIndexes()->getInstructionIndex(MI);
        if (InstSlot > LastUse && InstSlot < OldIdx)
          LastUse = InstSlot.getRegSlot();
      }
      return LastUse;
    }

    // This is a regunit interval, so scanning the use list could be very
    // expensive. Scan upwards from OldIdx instead.
    assert(Before < OldIdx && "Expected upwards move");
    SlotIndexes *Indexes = LIS.getSlotIndexes();
    MachineBasicBlock *MBB = Indexes->getMBBFromIndex(Before);

    // OldIdx may not correspond to an instruction any longer, so set MII to
    // point to the next instruction after OldIdx, or MBB->end().
    MachineBasicBlock::iterator MII = MBB->end();
    if (MachineInstr *MI = Indexes->getInstructionFromIndex(
                           Indexes->getNextNonNullIndex(OldIdx)))
      if (MI->getParent() == MBB)
        MII = MI;

    MachineBasicBlock::iterator Begin = MBB->begin();
    while (MII != Begin) {
      if ((--MII)->isDebugValue())
        continue;
      SlotIndex Idx = Indexes->getInstructionIndex(*MII);

      // Stop searching when Before is reached.
      if (!SlotIndex::isEarlierInstr(Before, Idx))
        return Before;

      // Check if MII uses Reg.
      for (MIBundleOperands MO(*MII); MO.isValid(); ++MO)
        if (MO->isReg() &&
            TargetRegisterInfo::isPhysicalRegister(MO->getReg()) &&
            TRI.hasRegUnit(MO->getReg(), Reg))
          return Idx.getRegSlot();
    }
    // Didn't reach Before. It must be the first instruction in the block.
    return Before;
  }
};

void LiveIntervals::handleMove(MachineInstr &MI, bool UpdateFlags) {
  assert(!MI.isBundled() && "Can't handle bundled instructions yet.");
  SlotIndex OldIndex = Indexes->getInstructionIndex(MI);
  Indexes->removeMachineInstrFromMaps(MI);
  SlotIndex NewIndex = Indexes->insertMachineInstrInMaps(MI);
  assert(getMBBStartIdx(MI.getParent()) <= OldIndex &&
         OldIndex < getMBBEndIdx(MI.getParent()) &&
         "Cannot handle moves across basic block boundaries.");

  HMEditor HME(*this, *MRI, *TRI, OldIndex, NewIndex, UpdateFlags);
  HME.updateAllRanges(&MI);
}

void LiveIntervals::handleMoveIntoBundle(MachineInstr &MI,
                                         MachineInstr &BundleStart,
                                         bool UpdateFlags) {
  SlotIndex OldIndex = Indexes->getInstructionIndex(MI);
  SlotIndex NewIndex = Indexes->getInstructionIndex(BundleStart);
  HMEditor HME(*this, *MRI, *TRI, OldIndex, NewIndex, UpdateFlags);
  HME.updateAllRanges(&MI);
}

void LiveIntervals::repairOldRegInRange(const MachineBasicBlock::iterator Begin,
                                        const MachineBasicBlock::iterator End,
                                        const SlotIndex endIdx,
                                        LiveRange &LR, const unsigned Reg,
                                        LaneBitmask LaneMask) {
  LiveInterval::iterator LII = LR.find(endIdx);
  SlotIndex lastUseIdx;
  if (LII != LR.end() && LII->start < endIdx)
    lastUseIdx = LII->end;
  else
    --LII;

  for (MachineBasicBlock::iterator I = End; I != Begin;) {
    --I;
    MachineInstr &MI = *I;
    if (MI.isDebugValue())
      continue;

    SlotIndex instrIdx = getInstructionIndex(MI);
    bool isStartValid = getInstructionFromIndex(LII->start);
    bool isEndValid = getInstructionFromIndex(LII->end);

    // FIXME: This doesn't currently handle early-clobber or multiple removed
    // defs inside of the region to repair.
    for (MachineInstr::mop_iterator OI = MI.operands_begin(),
                                    OE = MI.operands_end();
         OI != OE; ++OI) {
      const MachineOperand &MO = *OI;
      if (!MO.isReg() || MO.getReg() != Reg)
        continue;

      unsigned SubReg = MO.getSubReg();
      LaneBitmask Mask = TRI->getSubRegIndexLaneMask(SubReg);
      if ((Mask & LaneMask) == 0)
        continue;

      if (MO.isDef()) {
        if (!isStartValid) {
          if (LII->end.isDead()) {
            SlotIndex prevStart;
            if (LII != LR.begin())
              prevStart = std::prev(LII)->start;

            // FIXME: This could be more efficient if there was a
            // removeSegment method that returned an iterator.
            LR.removeSegment(*LII, true);
            if (prevStart.isValid())
              LII = LR.find(prevStart);
            else
              LII = LR.begin();
          } else {
            LII->start = instrIdx.getRegSlot();
            LII->valno->def = instrIdx.getRegSlot();
            if (MO.getSubReg() && !MO.isUndef())
              lastUseIdx = instrIdx.getRegSlot();
            else
              lastUseIdx = SlotIndex();
            continue;
          }
        }

        if (!lastUseIdx.isValid()) {
          VNInfo *VNI = LR.getNextValue(instrIdx.getRegSlot(), VNInfoAllocator);
          LiveRange::Segment S(instrIdx.getRegSlot(),
                               instrIdx.getDeadSlot(), VNI);
          LII = LR.addSegment(S);
        } else if (LII->start != instrIdx.getRegSlot()) {
          VNInfo *VNI = LR.getNextValue(instrIdx.getRegSlot(), VNInfoAllocator);
          LiveRange::Segment S(instrIdx.getRegSlot(), lastUseIdx, VNI);
          LII = LR.addSegment(S);
        }

        if (MO.getSubReg() && !MO.isUndef())
          lastUseIdx = instrIdx.getRegSlot();
        else
          lastUseIdx = SlotIndex();
      } else if (MO.isUse()) {
        // FIXME: This should probably be handled outside of this branch,
        // either as part of the def case (for defs inside of the region) or
        // after the loop over the region.
        if (!isEndValid && !LII->end.isBlock())
          LII->end = instrIdx.getRegSlot();
        if (!lastUseIdx.isValid())
          lastUseIdx = instrIdx.getRegSlot();
      }
    }
  }
}

void
LiveIntervals::repairIntervalsInRange(MachineBasicBlock *MBB,
                                      MachineBasicBlock::iterator Begin,
                                      MachineBasicBlock::iterator End,
                                      ArrayRef<unsigned> OrigRegs) {
  // Find anchor points, which are at the beginning/end of blocks or at
  // instructions that already have indexes.
  while (Begin != MBB->begin() && !Indexes->hasIndex(*Begin))
    --Begin;
  while (End != MBB->end() && !Indexes->hasIndex(*End))
    ++End;

  SlotIndex endIdx;
  if (End == MBB->end())
    endIdx = getMBBEndIdx(MBB).getPrevSlot();
  else
    endIdx = getInstructionIndex(*End);

  Indexes->repairIndexesInRange(MBB, Begin, End);

  for (MachineBasicBlock::iterator I = End; I != Begin;) {
    --I;
    MachineInstr &MI = *I;
    if (MI.isDebugValue())
      continue;
    for (MachineInstr::const_mop_iterator MOI = MI.operands_begin(),
                                          MOE = MI.operands_end();
         MOI != MOE; ++MOI) {
      if (MOI->isReg() &&
          TargetRegisterInfo::isVirtualRegister(MOI->getReg()) &&
          !hasInterval(MOI->getReg())) {
        createAndComputeVirtRegInterval(MOI->getReg());
      }
    }
  }

  for (unsigned i = 0, e = OrigRegs.size(); i != e; ++i) {
    unsigned Reg = OrigRegs[i];
    if (!TargetRegisterInfo::isVirtualRegister(Reg))
      continue;

    LiveInterval &LI = getInterval(Reg);
    // FIXME: Should we support undefs that gain defs?
    if (!LI.hasAtLeastOneValue())
      continue;

    for (LiveInterval::SubRange &S : LI.subranges()) {
      repairOldRegInRange(Begin, End, endIdx, S, Reg, S.LaneMask);
    }
    repairOldRegInRange(Begin, End, endIdx, LI, Reg);
  }
}

void LiveIntervals::removePhysRegDefAt(unsigned Reg, SlotIndex Pos) {
  for (MCRegUnitIterator Units(Reg, TRI); Units.isValid(); ++Units) {
    if (LiveRange *LR = getCachedRegUnit(*Units))
      if (VNInfo *VNI = LR->getVNInfoAt(Pos))
        LR->removeValNo(VNI);
  }
}

void LiveIntervals::removeVRegDefAt(LiveInterval &LI, SlotIndex Pos) {
  VNInfo *VNI = LI.getVNInfoAt(Pos);
  if (VNI == nullptr)
    return;
  LI.removeValNo(VNI);

  // Also remove the value in subranges.
  for (LiveInterval::SubRange &S : LI.subranges()) {
    if (VNInfo *SVNI = S.getVNInfoAt(Pos))
      S.removeValNo(SVNI);
  }
  LI.removeEmptySubRanges();
}

void LiveIntervals::splitSeparateComponents(LiveInterval &LI,
    SmallVectorImpl<LiveInterval*> &SplitLIs) {
  ConnectedVNInfoEqClasses ConEQ(*this);
  unsigned NumComp = ConEQ.Classify(LI);
  if (NumComp <= 1)
    return;
  DEBUG(dbgs() << "  Split " << NumComp << " components: " << LI << '\n');
  unsigned Reg = LI.reg;
  const TargetRegisterClass *RegClass = MRI->getRegClass(Reg);
  for (unsigned I = 1; I < NumComp; ++I) {
    unsigned NewVReg = MRI->createVirtualRegister(RegClass);
    LiveInterval &NewLI = createEmptyInterval(NewVReg);
    SplitLIs.push_back(&NewLI);
  }
  ConEQ.Distribute(LI, SplitLIs.data(), *MRI);
}

void LiveIntervals::renameDisconnectedComponents() {
  ConnectedSubRegClasses SubRegClasses(*this, *MRI);

  // Iterate over all vregs. Note that we query getNumVirtRegs() the newly
  // created vregs end up with higher numbers but do not need to be visited as
  // there can't be any further splitting.
  for (size_t I = 0, E = MRI->getNumVirtRegs(); I < E; ++I) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(I);
    LiveInterval *LI = VirtRegIntervals[Reg];
    if (LI == nullptr || !LI->hasSubRanges())
      continue;

    SubRegClasses.renameComponents(*LI);
  }
}
