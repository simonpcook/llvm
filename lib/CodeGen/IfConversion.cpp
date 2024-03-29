//===-- IfConversion.cpp - Machine code if conversion pass. ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the machine instruction level if-conversion pass, which
// tries to convert conditional branches into predicated instructions.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "BranchFolding.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <algorithm>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "ifcvt"

// Hidden options for help debugging.
static cl::opt<int> IfCvtFnStart("ifcvt-fn-start", cl::init(-1), cl::Hidden);
static cl::opt<int> IfCvtFnStop("ifcvt-fn-stop", cl::init(-1), cl::Hidden);
static cl::opt<int> IfCvtLimit("ifcvt-limit", cl::init(-1), cl::Hidden);
static cl::opt<bool> DisableSimple("disable-ifcvt-simple",
                                   cl::init(false), cl::Hidden);
static cl::opt<bool> DisableSimpleF("disable-ifcvt-simple-false",
                                    cl::init(false), cl::Hidden);
static cl::opt<bool> DisableTriangle("disable-ifcvt-triangle",
                                     cl::init(false), cl::Hidden);
static cl::opt<bool> DisableTriangleR("disable-ifcvt-triangle-rev",
                                      cl::init(false), cl::Hidden);
static cl::opt<bool> DisableTriangleF("disable-ifcvt-triangle-false",
                                      cl::init(false), cl::Hidden);
static cl::opt<bool> DisableTriangleFR("disable-ifcvt-triangle-false-rev",
                                       cl::init(false), cl::Hidden);
static cl::opt<bool> DisableDiamond("disable-ifcvt-diamond",
                                    cl::init(false), cl::Hidden);
static cl::opt<bool> DisableForkedDiamond("disable-ifcvt-forked-diamond",
                                        cl::init(false), cl::Hidden);
static cl::opt<bool> IfCvtBranchFold("ifcvt-branch-fold",
                                     cl::init(true), cl::Hidden);

STATISTIC(NumSimple,       "Number of simple if-conversions performed");
STATISTIC(NumSimpleFalse,  "Number of simple (F) if-conversions performed");
STATISTIC(NumTriangle,     "Number of triangle if-conversions performed");
STATISTIC(NumTriangleRev,  "Number of triangle (R) if-conversions performed");
STATISTIC(NumTriangleFalse,"Number of triangle (F) if-conversions performed");
STATISTIC(NumTriangleFRev, "Number of triangle (F/R) if-conversions performed");
STATISTIC(NumDiamonds,     "Number of diamond if-conversions performed");
STATISTIC(NumForkedDiamonds, "Number of forked-diamond if-conversions performed");
STATISTIC(NumIfConvBBs,    "Number of if-converted blocks");
STATISTIC(NumDupBBs,       "Number of duplicated blocks");
STATISTIC(NumUnpred,       "Number of true blocks of diamonds unpredicated");

namespace {
  class IfConverter : public MachineFunctionPass {
    enum IfcvtKind {
      ICNotClassfied,  // BB data valid, but not classified.
      ICSimpleFalse,   // Same as ICSimple, but on the false path.
      ICSimple,        // BB is entry of an one split, no rejoin sub-CFG.
      ICTriangleFRev,  // Same as ICTriangleFalse, but false path rev condition.
      ICTriangleRev,   // Same as ICTriangle, but true path rev condition.
      ICTriangleFalse, // Same as ICTriangle, but on the false path.
      ICTriangle,      // BB is entry of a triangle sub-CFG.
      ICDiamond,       // BB is entry of a diamond sub-CFG.
      ICForkedDiamond  // BB is entry of an almost diamond sub-CFG, with a
                       // common tail that can be shared.
    };

    /// One per MachineBasicBlock, this is used to cache the result
    /// if-conversion feasibility analysis. This includes results from
    /// TargetInstrInfo::analyzeBranch() (i.e. TBB, FBB, and Cond), and its
    /// classification, and common tail block of its successors (if it's a
    /// diamond shape), its size, whether it's predicable, and whether any
    /// instruction can clobber the 'would-be' predicate.
    ///
    /// IsDone          - True if BB is not to be considered for ifcvt.
    /// IsBeingAnalyzed - True if BB is currently being analyzed.
    /// IsAnalyzed      - True if BB has been analyzed (info is still valid).
    /// IsEnqueued      - True if BB has been enqueued to be ifcvt'ed.
    /// IsBrAnalyzable  - True if analyzeBranch() returns false.
    /// HasFallThrough  - True if BB may fallthrough to the following BB.
    /// IsUnpredicable  - True if BB is known to be unpredicable.
    /// ClobbersPred    - True if BB could modify predicates (e.g. has
    ///                   cmp, call, etc.)
    /// NonPredSize     - Number of non-predicated instructions.
    /// ExtraCost       - Extra cost for multi-cycle instructions.
    /// ExtraCost2      - Some instructions are slower when predicated
    /// BB              - Corresponding MachineBasicBlock.
    /// TrueBB / FalseBB- See analyzeBranch().
    /// BrCond          - Conditions for end of block conditional branches.
    /// Predicate       - Predicate used in the BB.
    struct BBInfo {
      bool IsDone          : 1;
      bool IsBeingAnalyzed : 1;
      bool IsAnalyzed      : 1;
      bool IsEnqueued      : 1;
      bool IsBrAnalyzable  : 1;
      bool IsBrReversible  : 1;
      bool HasFallThrough  : 1;
      bool IsUnpredicable  : 1;
      bool CannotBeCopied  : 1;
      bool ClobbersPred    : 1;
      unsigned NonPredSize;
      unsigned ExtraCost;
      unsigned ExtraCost2;
      MachineBasicBlock *BB;
      MachineBasicBlock *TrueBB;
      MachineBasicBlock *FalseBB;
      SmallVector<MachineOperand, 4> BrCond;
      SmallVector<MachineOperand, 4> Predicate;
      BBInfo() : IsDone(false), IsBeingAnalyzed(false),
                 IsAnalyzed(false), IsEnqueued(false), IsBrAnalyzable(false),
                 IsBrReversible(false), HasFallThrough(false),
                 IsUnpredicable(false), CannotBeCopied(false),
                 ClobbersPred(false), NonPredSize(0), ExtraCost(0),
                 ExtraCost2(0), BB(nullptr), TrueBB(nullptr),
                 FalseBB(nullptr) {}
    };

    /// Record information about pending if-conversions to attempt:
    /// BBI             - Corresponding BBInfo.
    /// Kind            - Type of block. See IfcvtKind.
    /// NeedSubsumption - True if the to-be-predicated BB has already been
    ///                   predicated.
    /// NumDups      - Number of instructions that would be duplicated due
    ///                   to this if-conversion. (For diamonds, the number of
    ///                   identical instructions at the beginnings of both
    ///                   paths).
    /// NumDups2     - For diamonds, the number of identical instructions
    ///                   at the ends of both paths.
    struct IfcvtToken {
      BBInfo &BBI;
      IfcvtKind Kind;
      unsigned NumDups;
      unsigned NumDups2;
      bool NeedSubsumption : 1;
      bool TClobbersPred : 1;
      bool FClobbersPred : 1;
      IfcvtToken(BBInfo &b, IfcvtKind k, bool s, unsigned d, unsigned d2 = 0,
                 bool tc = false, bool fc = false)
        : BBI(b), Kind(k), NumDups(d), NumDups2(d2), NeedSubsumption(s),
          TClobbersPred(tc), FClobbersPred(fc) {}
    };

    /// Results of if-conversion feasibility analysis indexed by basic block
    /// number.
    std::vector<BBInfo> BBAnalysis;
    TargetSchedModel SchedModel;

    const TargetLoweringBase *TLI;
    const TargetInstrInfo *TII;
    const TargetRegisterInfo *TRI;
    const MachineBranchProbabilityInfo *MBPI;
    MachineRegisterInfo *MRI;

    LivePhysRegs Redefs;
    LivePhysRegs DontKill;

    bool PreRegAlloc;
    bool MadeChange;
    int FnNum;
    std::function<bool(const Function &)> PredicateFtor;

  public:
    static char ID;
    IfConverter(std::function<bool(const Function &)> Ftor = nullptr)
        : MachineFunctionPass(ID), FnNum(-1), PredicateFtor(std::move(Ftor)) {
      initializeIfConverterPass(*PassRegistry::getPassRegistry());
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<MachineBlockFrequencyInfo>();
      AU.addRequired<MachineBranchProbabilityInfo>();
      MachineFunctionPass::getAnalysisUsage(AU);
    }

    bool runOnMachineFunction(MachineFunction &MF) override;

    MachineFunctionProperties getRequiredProperties() const override {
      return MachineFunctionProperties().set(
          MachineFunctionProperties::Property::NoVRegs);
    }

  private:
    bool ReverseBranchCondition(BBInfo &BBI) const;
    bool ValidSimple(BBInfo &TrueBBI, unsigned &Dups,
                     BranchProbability Prediction) const;
    bool ValidTriangle(BBInfo &TrueBBI, BBInfo &FalseBBI,
                       bool FalseBranch, unsigned &Dups,
                       BranchProbability Prediction) const;
    bool CountDuplicatedInstructions(
        MachineBasicBlock::iterator &TIB, MachineBasicBlock::iterator &FIB,
        MachineBasicBlock::iterator &TIE, MachineBasicBlock::iterator &FIE,
        unsigned &Dups1, unsigned &Dups2,
        MachineBasicBlock &TBB, MachineBasicBlock &FBB,
        bool SkipUnconditionalBranches) const;
    bool ValidDiamond(BBInfo &TrueBBI, BBInfo &FalseBBI,
                      unsigned &Dups1, unsigned &Dups2,
                      BBInfo &TrueBBICalc, BBInfo &FalseBBICalc) const;
    bool ValidForkedDiamond(BBInfo &TrueBBI, BBInfo &FalseBBI,
                            unsigned &Dups1, unsigned &Dups2,
                            BBInfo &TrueBBICalc, BBInfo &FalseBBICalc) const;
    void AnalyzeBranches(BBInfo &BBI);
    void ScanInstructions(BBInfo &BBI,
                          MachineBasicBlock::iterator &Begin,
                          MachineBasicBlock::iterator &End,
                          bool BranchUnpredicable = false) const;
    bool RescanInstructions(
        MachineBasicBlock::iterator &TIB, MachineBasicBlock::iterator &FIB,
        MachineBasicBlock::iterator &TIE, MachineBasicBlock::iterator &FIE,
        BBInfo &TrueBBI, BBInfo &FalseBBI) const;
    void AnalyzeBlock(MachineBasicBlock &MBB,
                      std::vector<std::unique_ptr<IfcvtToken>> &Tokens);
    bool FeasibilityAnalysis(BBInfo &BBI, SmallVectorImpl<MachineOperand> &Cond,
                             bool isTriangle = false, bool RevBranch = false,
                             bool hasCommonTail = false);
    void AnalyzeBlocks(MachineFunction &MF,
                       std::vector<std::unique_ptr<IfcvtToken>> &Tokens);
    void InvalidatePreds(MachineBasicBlock &MBB);
    void RemoveExtraEdges(BBInfo &BBI);
    bool IfConvertSimple(BBInfo &BBI, IfcvtKind Kind);
    bool IfConvertTriangle(BBInfo &BBI, IfcvtKind Kind);
    bool IfConvertDiamondCommon(BBInfo &BBI, BBInfo &TrueBBI, BBInfo &FalseBBI,
                                unsigned NumDups1, unsigned NumDups2,
                                bool TClobbersPred, bool FClobbersPred,
                                bool RemoveBranch, bool MergeAddEdges);
    bool IfConvertDiamond(BBInfo &BBI, IfcvtKind Kind,
                          unsigned NumDups1, unsigned NumDups2,
                          bool TClobbers, bool FClobbers);
    bool IfConvertForkedDiamond(BBInfo &BBI, IfcvtKind Kind,
                              unsigned NumDups1, unsigned NumDups2,
                              bool TClobbers, bool FClobbers);
    void PredicateBlock(BBInfo &BBI,
                        MachineBasicBlock::iterator E,
                        SmallVectorImpl<MachineOperand> &Cond,
                        SmallSet<unsigned, 4> *LaterRedefs = nullptr);
    void CopyAndPredicateBlock(BBInfo &ToBBI, BBInfo &FromBBI,
                               SmallVectorImpl<MachineOperand> &Cond,
                               bool IgnoreBr = false);
    void MergeBlocks(BBInfo &ToBBI, BBInfo &FromBBI, bool AddEdges = true);

    bool MeetIfcvtSizeLimit(MachineBasicBlock &BB,
                            unsigned Cycle, unsigned Extra,
                            BranchProbability Prediction) const {
      return Cycle > 0 && TII->isProfitableToIfCvt(BB, Cycle, Extra,
                                                   Prediction);
    }

    bool MeetIfcvtSizeLimit(MachineBasicBlock &TBB,
                            unsigned TCycle, unsigned TExtra,
                            MachineBasicBlock &FBB,
                            unsigned FCycle, unsigned FExtra,
                            BranchProbability Prediction) const {
      return TCycle > 0 && FCycle > 0 &&
        TII->isProfitableToIfCvt(TBB, TCycle, TExtra, FBB, FCycle, FExtra,
                                 Prediction);
    }

    /// Returns true if Block ends without a terminator.
    bool blockAlwaysFallThrough(BBInfo &BBI) const {
      return BBI.IsBrAnalyzable && BBI.TrueBB == nullptr;
    }

    /// Used to sort if-conversion candidates.
    static bool IfcvtTokenCmp(const std::unique_ptr<IfcvtToken> &C1,
                              const std::unique_ptr<IfcvtToken> &C2) {
      int Incr1 = (C1->Kind == ICDiamond)
        ? -(int)(C1->NumDups + C1->NumDups2) : (int)C1->NumDups;
      int Incr2 = (C2->Kind == ICDiamond)
        ? -(int)(C2->NumDups + C2->NumDups2) : (int)C2->NumDups;
      if (Incr1 > Incr2)
        return true;
      else if (Incr1 == Incr2) {
        // Favors subsumption.
        if (!C1->NeedSubsumption && C2->NeedSubsumption)
          return true;
        else if (C1->NeedSubsumption == C2->NeedSubsumption) {
          // Favors diamond over triangle, etc.
          if ((unsigned)C1->Kind < (unsigned)C2->Kind)
            return true;
          else if (C1->Kind == C2->Kind)
            return C1->BBI.BB->getNumber() < C2->BBI.BB->getNumber();
        }
      }
      return false;
    }
  };

  char IfConverter::ID = 0;
}

char &llvm::IfConverterID = IfConverter::ID;

INITIALIZE_PASS_BEGIN(IfConverter, "if-converter", "If Converter", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineBranchProbabilityInfo)
INITIALIZE_PASS_END(IfConverter, "if-converter", "If Converter", false, false)

bool IfConverter::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(*MF.getFunction()) ||
      (PredicateFtor && !PredicateFtor(*MF.getFunction())))
    return false;

  const TargetSubtargetInfo &ST = MF.getSubtarget();
  TLI = ST.getTargetLowering();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();
  BranchFolder::MBFIWrapper MBFI(getAnalysis<MachineBlockFrequencyInfo>());
  MBPI = &getAnalysis<MachineBranchProbabilityInfo>();
  MRI = &MF.getRegInfo();
  SchedModel.init(ST.getSchedModel(), &ST, TII);

  if (!TII) return false;

  PreRegAlloc = MRI->isSSA();

  bool BFChange = false;
  if (!PreRegAlloc) {
    // Tail merge tend to expose more if-conversion opportunities.
    BranchFolder BF(true, false, MBFI, *MBPI);
    BFChange = BF.OptimizeFunction(MF, TII, ST.getRegisterInfo(),
                                   getAnalysisIfAvailable<MachineModuleInfo>());
  }

  DEBUG(dbgs() << "\nIfcvt: function (" << ++FnNum <<  ") \'"
               << MF.getName() << "\'");

  if (FnNum < IfCvtFnStart || (IfCvtFnStop != -1 && FnNum > IfCvtFnStop)) {
    DEBUG(dbgs() << " skipped\n");
    return false;
  }
  DEBUG(dbgs() << "\n");

  MF.RenumberBlocks();
  BBAnalysis.resize(MF.getNumBlockIDs());

  std::vector<std::unique_ptr<IfcvtToken>> Tokens;
  MadeChange = false;
  unsigned NumIfCvts = NumSimple + NumSimpleFalse + NumTriangle +
    NumTriangleRev + NumTriangleFalse + NumTriangleFRev + NumDiamonds;
  while (IfCvtLimit == -1 || (int)NumIfCvts < IfCvtLimit) {
    // Do an initial analysis for each basic block and find all the potential
    // candidates to perform if-conversion.
    bool Change = false;
    AnalyzeBlocks(MF, Tokens);
    while (!Tokens.empty()) {
      std::unique_ptr<IfcvtToken> Token = std::move(Tokens.back());
      Tokens.pop_back();
      BBInfo &BBI = Token->BBI;
      IfcvtKind Kind = Token->Kind;
      unsigned NumDups = Token->NumDups;
      unsigned NumDups2 = Token->NumDups2;

      // If the block has been evicted out of the queue or it has already been
      // marked dead (due to it being predicated), then skip it.
      if (BBI.IsDone)
        BBI.IsEnqueued = false;
      if (!BBI.IsEnqueued)
        continue;

      BBI.IsEnqueued = false;

      bool RetVal = false;
      switch (Kind) {
      default: llvm_unreachable("Unexpected!");
      case ICSimple:
      case ICSimpleFalse: {
        bool isFalse = Kind == ICSimpleFalse;
        if ((isFalse && DisableSimpleF) || (!isFalse && DisableSimple)) break;
        DEBUG(dbgs() << "Ifcvt (Simple" << (Kind == ICSimpleFalse ?
                                            " false" : "")
                     << "): BB#" << BBI.BB->getNumber() << " ("
                     << ((Kind == ICSimpleFalse)
                         ? BBI.FalseBB->getNumber()
                         : BBI.TrueBB->getNumber()) << ") ");
        RetVal = IfConvertSimple(BBI, Kind);
        DEBUG(dbgs() << (RetVal ? "succeeded!" : "failed!") << "\n");
        if (RetVal) {
          if (isFalse) ++NumSimpleFalse;
          else         ++NumSimple;
        }
       break;
      }
      case ICTriangle:
      case ICTriangleRev:
      case ICTriangleFalse:
      case ICTriangleFRev: {
        bool isFalse = Kind == ICTriangleFalse;
        bool isRev   = (Kind == ICTriangleRev || Kind == ICTriangleFRev);
        if (DisableTriangle && !isFalse && !isRev) break;
        if (DisableTriangleR && !isFalse && isRev) break;
        if (DisableTriangleF && isFalse && !isRev) break;
        if (DisableTriangleFR && isFalse && isRev) break;
        DEBUG(dbgs() << "Ifcvt (Triangle");
        if (isFalse)
          DEBUG(dbgs() << " false");
        if (isRev)
          DEBUG(dbgs() << " rev");
        DEBUG(dbgs() << "): BB#" << BBI.BB->getNumber() << " (T:"
                     << BBI.TrueBB->getNumber() << ",F:"
                     << BBI.FalseBB->getNumber() << ") ");
        RetVal = IfConvertTriangle(BBI, Kind);
        DEBUG(dbgs() << (RetVal ? "succeeded!" : "failed!") << "\n");
        if (RetVal) {
          if (isFalse) {
            if (isRev) ++NumTriangleFRev;
            else       ++NumTriangleFalse;
          } else {
            if (isRev) ++NumTriangleRev;
            else       ++NumTriangle;
          }
        }
        break;
      }
      case ICDiamond: {
        if (DisableDiamond) break;
        DEBUG(dbgs() << "Ifcvt (Diamond): BB#" << BBI.BB->getNumber() << " (T:"
                     << BBI.TrueBB->getNumber() << ",F:"
                     << BBI.FalseBB->getNumber() << ") ");
        RetVal = IfConvertDiamond(BBI, Kind, NumDups, NumDups2,
                                  Token->TClobbersPred,
                                  Token->FClobbersPred);
        DEBUG(dbgs() << (RetVal ? "succeeded!" : "failed!") << "\n");
        if (RetVal) ++NumDiamonds;
        break;
      }
      case ICForkedDiamond: {
        if (DisableForkedDiamond) break;
        DEBUG(dbgs() << "Ifcvt (Forked Diamond): BB#"
                     << BBI.BB->getNumber() << " (T:"
                     << BBI.TrueBB->getNumber() << ",F:"
                     << BBI.FalseBB->getNumber() << ") ");
        RetVal = IfConvertForkedDiamond(BBI, Kind, NumDups, NumDups2,
                                      Token->TClobbersPred,
                                      Token->FClobbersPred);
        DEBUG(dbgs() << (RetVal ? "succeeded!" : "failed!") << "\n");
        if (RetVal) ++NumForkedDiamonds;
        break;
      }
      }

      Change |= RetVal;

      NumIfCvts = NumSimple + NumSimpleFalse + NumTriangle + NumTriangleRev +
        NumTriangleFalse + NumTriangleFRev + NumDiamonds;
      if (IfCvtLimit != -1 && (int)NumIfCvts >= IfCvtLimit)
        break;
    }

    if (!Change)
      break;
    MadeChange |= Change;
  }

  Tokens.clear();
  BBAnalysis.clear();

  if (MadeChange && IfCvtBranchFold) {
    BranchFolder BF(false, false, MBFI, *MBPI);
    BF.OptimizeFunction(MF, TII, MF.getSubtarget().getRegisterInfo(),
                        getAnalysisIfAvailable<MachineModuleInfo>());
  }

  MadeChange |= BFChange;
  return MadeChange;
}

/// BB has a fallthrough. Find its 'false' successor given its 'true' successor.
static MachineBasicBlock *findFalseBlock(MachineBasicBlock *BB,
                                         MachineBasicBlock *TrueBB) {
  for (MachineBasicBlock *SuccBB : BB->successors()) {
    if (SuccBB != TrueBB)
      return SuccBB;
  }
  return nullptr;
}

/// Reverse the condition of the end of the block branch. Swap block's 'true'
/// and 'false' successors.
bool IfConverter::ReverseBranchCondition(BBInfo &BBI) const {
  DebugLoc dl;  // FIXME: this is nowhere
  if (!TII->ReverseBranchCondition(BBI.BrCond)) {
    TII->RemoveBranch(*BBI.BB);
    TII->InsertBranch(*BBI.BB, BBI.FalseBB, BBI.TrueBB, BBI.BrCond, dl);
    std::swap(BBI.TrueBB, BBI.FalseBB);
    return true;
  }
  return false;
}

/// Returns the next block in the function blocks ordering. If it is the end,
/// returns NULL.
static inline MachineBasicBlock *getNextBlock(MachineBasicBlock &MBB) {
  MachineFunction::iterator I = MBB.getIterator();
  MachineFunction::iterator E = MBB.getParent()->end();
  if (++I == E)
    return nullptr;
  return &*I;
}

/// Returns true if the 'true' block (along with its predecessor) forms a valid
/// simple shape for ifcvt. It also returns the number of instructions that the
/// ifcvt would need to duplicate if performed in Dups.
bool IfConverter::ValidSimple(BBInfo &TrueBBI, unsigned &Dups,
                              BranchProbability Prediction) const {
  Dups = 0;
  if (TrueBBI.IsBeingAnalyzed || TrueBBI.IsDone)
    return false;

  if (TrueBBI.IsBrAnalyzable)
    return false;

  if (TrueBBI.BB->pred_size() > 1) {
    if (TrueBBI.CannotBeCopied ||
        !TII->isProfitableToDupForIfCvt(*TrueBBI.BB, TrueBBI.NonPredSize,
                                        Prediction))
      return false;
    Dups = TrueBBI.NonPredSize;
  }

  return true;
}

/// Returns true if the 'true' and 'false' blocks (along with their common
/// predecessor) forms a valid triangle shape for ifcvt. If 'FalseBranch' is
/// true, it checks if 'true' block's false branch branches to the 'false' block
/// rather than the other way around. It also returns the number of instructions
/// that the ifcvt would need to duplicate if performed in 'Dups'.
bool IfConverter::ValidTriangle(BBInfo &TrueBBI, BBInfo &FalseBBI,
                                bool FalseBranch, unsigned &Dups,
                                BranchProbability Prediction) const {
  Dups = 0;
  if (TrueBBI.IsBeingAnalyzed || TrueBBI.IsDone)
    return false;

  if (TrueBBI.BB->pred_size() > 1) {
    if (TrueBBI.CannotBeCopied)
      return false;

    unsigned Size = TrueBBI.NonPredSize;
    if (TrueBBI.IsBrAnalyzable) {
      if (TrueBBI.TrueBB && TrueBBI.BrCond.empty())
        // Ends with an unconditional branch. It will be removed.
        --Size;
      else {
        MachineBasicBlock *FExit = FalseBranch
          ? TrueBBI.TrueBB : TrueBBI.FalseBB;
        if (FExit)
          // Require a conditional branch
          ++Size;
      }
    }
    if (!TII->isProfitableToDupForIfCvt(*TrueBBI.BB, Size, Prediction))
      return false;
    Dups = Size;
  }

  MachineBasicBlock *TExit = FalseBranch ? TrueBBI.FalseBB : TrueBBI.TrueBB;
  if (!TExit && blockAlwaysFallThrough(TrueBBI)) {
    MachineFunction::iterator I = TrueBBI.BB->getIterator();
    if (++I == TrueBBI.BB->getParent()->end())
      return false;
    TExit = &*I;
  }
  return TExit && TExit == FalseBBI.BB;
}

/// Increment \p It until it points to a non-debug instruction or to \p End.
/// @param It Iterator to increment
/// @param End Iterator that points to end. Will be compared to It
/// @returns true if It == End, false otherwise.
static inline bool skipDebugInstructionsForward(
    MachineBasicBlock::iterator &It,
    MachineBasicBlock::iterator &End) {
  while (It != End && It->isDebugValue())
    It++;
  return It == End;
}

/// Shrink the provided inclusive range by one instruction.
/// If the range was one instruction (\p It == \p Begin), It is not modified,
/// but \p Empty is set to true.
static inline void shrinkInclusiveRange(
    MachineBasicBlock::iterator &Begin,
    MachineBasicBlock::iterator &It,
    bool &Empty) {
  if (It == Begin)
    Empty = true;
  else
    It--;
}

/// Decrement \p It until it points to a non-debug instruction or the range is
/// empty.
/// @param It Iterator to decrement.
/// @param Begin Iterator that points to beginning. Will be compared to It
/// @param Empty Set to true if the resulting range is Empty
/// @returns the value of Empty as a convenience.
static inline bool skipDebugInstructionsBackward(
    MachineBasicBlock::iterator &Begin,
    MachineBasicBlock::iterator &It,
    bool &Empty) {
  while (!Empty && It->isDebugValue())
    shrinkInclusiveRange(Begin, It, Empty);
  return Empty;
}

/// Count duplicated instructions and move the iterators to show where they
/// are.
/// @param TIB True Iterator Begin
/// @param FIB False Iterator Begin
/// These two iterators initially point to the first instruction of the two
/// blocks, and finally point to the first non-shared instruction.
/// @param TIE True Iterator End
/// @param FIE False Iterator End
/// These two iterators initially point to End() for the two blocks() and
/// finally point to the first shared instruction in the tail.
/// Upon return [TIB, TIE), and [FIB, FIE) mark the un-duplicated portions of
/// two blocks.
/// @param Dups1 count of duplicated instructions at the beginning of the 2
/// blocks.
/// @param Dups2 count of duplicated instructions at the end of the 2 blocks.
/// @param SkipUnconditionalBranches if true, Don't make sure that
/// unconditional branches at the end of the blocks are the same. True is
/// passed when the blocks are analyzable to allow for fallthrough to be
/// handled.
/// @return false if the shared portion prevents if conversion.
bool IfConverter::CountDuplicatedInstructions(
    MachineBasicBlock::iterator &TIB,
    MachineBasicBlock::iterator &FIB,
    MachineBasicBlock::iterator &TIE,
    MachineBasicBlock::iterator &FIE,
    unsigned &Dups1, unsigned &Dups2,
    MachineBasicBlock &TBB, MachineBasicBlock &FBB,
    bool SkipUnconditionalBranches) const {

  while (TIB != TIE && FIB != FIE) {
    // Skip dbg_value instructions. These do not count.
    if(skipDebugInstructionsForward(TIB, TIE))
      break;
    if(skipDebugInstructionsForward(FIB, FIE))
      break;
    if (!TIB->isIdenticalTo(*FIB))
      break;
    // A pred-clobbering instruction in the shared portion prevents
    // if-conversion.
    std::vector<MachineOperand> PredDefs;
    if (TII->DefinesPredicate(*TIB, PredDefs))
      return false;
    // If we get all the way to the branch instructions, don't count them.
    if (!TIB->isBranch())
      ++Dups1;
    ++TIB;
    ++FIB;
  }

  // Check for already containing all of the block.
  if (TIB == TIE || FIB == FIE)
    return true;
  // Now, in preparation for counting duplicate instructions at the ends of the
  // blocks, move the end iterators up past any branch instructions.
  --TIE;
  --FIE;

  // After this point TIB and TIE define an inclusive range, which means that
  // TIB == TIE is true when there is one more instruction to consider, not at
  // the end. Because we may not be able to go before TIB, we need a flag to
  // indicate a completely empty range.
  bool TEmpty = false, FEmpty = false;

  // Upon exit TIE and FIE will both point at the last non-shared instruction.
  // They need to be moved forward to point past the last non-shared
  // instruction if the range they delimit is non-empty.
  auto IncrementEndIteratorsOnExit = make_scope_exit([&]() {
    if (!TEmpty)
      ++TIE;
    if (!FEmpty)
      ++FIE;
  });

  if (!TBB.succ_empty() || !FBB.succ_empty()) {
    if (SkipUnconditionalBranches) {
      while (!TEmpty && TIE->isUnconditionalBranch())
        shrinkInclusiveRange(TIB, TIE, TEmpty);
      while (!FEmpty && FIE->isUnconditionalBranch())
        shrinkInclusiveRange(FIB, FIE, FEmpty);
    }
  }

  // If Dups1 includes all of a block, then don't count duplicate
  // instructions at the end of the blocks.
  if (TEmpty || FEmpty)
    return true;

  // Count duplicate instructions at the ends of the blocks.
  while (!TEmpty && !FEmpty) {
    // Skip dbg_value instructions. These do not count.
    if (skipDebugInstructionsBackward(TIB, TIE, TEmpty))
      break;
    if (skipDebugInstructionsBackward(FIB, FIE, FEmpty))
      break;
    if (!TIE->isIdenticalTo(*FIE))
      break;
    // We have to verify that any branch instructions are the same, and then we
    // don't count them toward the # of duplicate instructions.
    if (!TIE->isBranch())
      ++Dups2;
    shrinkInclusiveRange(TIB, TIE, TEmpty);
    shrinkInclusiveRange(FIB, FIE, FEmpty);
  }
  return true;
}

/// RescanInstructions - Run ScanInstructions on a pair of blocks.
/// @param TIB - True Iterator Begin, points to first non-shared instruction
/// @param FIB - False Iterator Begin, points to first non-shared instruction
/// @param TIE - True Iterator End, points past last non-shared instruction
/// @param FIE - False Iterator End, points past last non-shared instruction
/// @param TrueBBI  - BBInfo to update for the true block.
/// @param FalseBBI - BBInfo to update for the false block.
/// @returns - false if either block cannot be predicated or if both blocks end
///   with a predicate-clobbering instruction.
bool IfConverter::RescanInstructions(
    MachineBasicBlock::iterator &TIB, MachineBasicBlock::iterator &FIB,
    MachineBasicBlock::iterator &TIE, MachineBasicBlock::iterator &FIE,
    BBInfo &TrueBBI, BBInfo &FalseBBI) const {
  bool BranchUnpredicable = true;
  TrueBBI.IsUnpredicable = FalseBBI.IsUnpredicable = false;
  ScanInstructions(TrueBBI, TIB, TIE, BranchUnpredicable);
  if (TrueBBI.IsUnpredicable)
    return false;
  ScanInstructions(FalseBBI, FIB, FIE, BranchUnpredicable);
  if (FalseBBI.IsUnpredicable)
    return false;
  if (TrueBBI.ClobbersPred && FalseBBI.ClobbersPred)
    return false;
  return true;
}

#ifndef NDEBUG
static void verifySameBranchInstructions(
    MachineBasicBlock *MBB1,
    MachineBasicBlock *MBB2) {
  MachineBasicBlock::iterator B1 = MBB1->begin();
  MachineBasicBlock::iterator B2 = MBB2->begin();
  MachineBasicBlock::iterator E1 = std::prev(MBB1->end());
  MachineBasicBlock::iterator E2 = std::prev(MBB2->end());
  bool Empty1 = false, Empty2 = false;
  while (!Empty1 && !Empty2) {
    skipDebugInstructionsBackward(B1, E1, Empty1);
    skipDebugInstructionsBackward(B2, E2, Empty2);
    if (Empty1 && Empty2)
      break;

    if (Empty1) {
      assert(!E2->isBranch() && "Branch mis-match, one block is empty.");
      break;
    }
    if (Empty2) {
      assert(!E1->isBranch() && "Branch mis-match, one block is empty.");
      break;
    }

    if (E1->isBranch() || E2->isBranch())
      assert(E1->isIdenticalTo(*E2) &&
             "Branch mis-match, branch instructions don't match.");
    else
      break;
    shrinkInclusiveRange(B1, E1, Empty1);
    shrinkInclusiveRange(B2, E2, Empty2);
  }
}
#endif

/// ValidForkedDiamond - Returns true if the 'true' and 'false' blocks (along
/// with their common predecessor) form a diamond if a common tail block is
/// extracted.
/// While not strictly a diamond, this pattern would form a diamond if
/// tail-merging had merged the shared tails.
///           EBB
///         _/   \_
///         |     |
///        TBB   FBB
///        /  \ /   \
///  FalseBB TrueBB FalseBB
/// Currently only handles analyzable branches.
/// Specifically excludes actual diamonds to avoid overlap.
bool IfConverter::ValidForkedDiamond(
    BBInfo &TrueBBI, BBInfo &FalseBBI,
    unsigned &Dups1, unsigned &Dups2,
    BBInfo &TrueBBICalc, BBInfo &FalseBBICalc) const {
  Dups1 = Dups2 = 0;
  if (TrueBBI.IsBeingAnalyzed || TrueBBI.IsDone ||
      FalseBBI.IsBeingAnalyzed || FalseBBI.IsDone)
    return false;

  if (!TrueBBI.IsBrAnalyzable || !FalseBBI.IsBrAnalyzable)
    return false;
  // Don't IfConvert blocks that can't be folded into their predecessor.
  if  (TrueBBI.BB->pred_size() > 1 || FalseBBI.BB->pred_size() > 1)
    return false;

  // This function is specifically looking for conditional tails, as
  // unconditional tails are already handled by the standard diamond case.
  if (TrueBBI.BrCond.size() == 0 ||
      FalseBBI.BrCond.size() == 0)
    return false;

  MachineBasicBlock *TT = TrueBBI.TrueBB;
  MachineBasicBlock *TF = TrueBBI.FalseBB;
  MachineBasicBlock *FT = FalseBBI.TrueBB;
  MachineBasicBlock *FF = FalseBBI.FalseBB;

  if (!TT)
    TT = getNextBlock(*TrueBBI.BB);
  if (!TF)
    TF = getNextBlock(*TrueBBI.BB);
  if (!FT)
    FT = getNextBlock(*FalseBBI.BB);
  if (!FF)
    FF = getNextBlock(*FalseBBI.BB);

  if (!TT || !TF)
    return false;

  // Check successors. If they don't match, bail.
  if (!((TT == FT && TF == FF) || (TF == FT && TT == FF)))
    return false;

  bool FalseReversed = false;
  if (TF == FT && TT == FF) {
    // If the branches are opposing, but we can't reverse, don't do it.
    if (!FalseBBI.IsBrReversible)
      return false;
    FalseReversed = true;
    ReverseBranchCondition(FalseBBI);
  }
  auto UnReverseOnExit = make_scope_exit([&]() {
    if (FalseReversed)
      ReverseBranchCondition(FalseBBI);
  });

  // Count duplicate instructions at the beginning of the true and false blocks.
  MachineBasicBlock::iterator TIB = TrueBBI.BB->begin();
  MachineBasicBlock::iterator FIB = FalseBBI.BB->begin();
  MachineBasicBlock::iterator TIE = TrueBBI.BB->end();
  MachineBasicBlock::iterator FIE = FalseBBI.BB->end();
  if(!CountDuplicatedInstructions(TIB, FIB, TIE, FIE, Dups1, Dups2,
                                  *TrueBBI.BB, *FalseBBI.BB,
                                  /* SkipUnconditionalBranches */ true))
    return false;

  TrueBBICalc.BB = TrueBBI.BB;
  FalseBBICalc.BB = FalseBBI.BB;
  if (!RescanInstructions(TIB, FIB, TIE, FIE, TrueBBICalc, FalseBBICalc))
    return false;

  // The size is used to decide whether to if-convert, and the shared portions
  // are subtracted off. Because of the subtraction, we just use the size that
  // was calculated by the original ScanInstructions, as it is correct.
  TrueBBICalc.NonPredSize = TrueBBI.NonPredSize;
  FalseBBICalc.NonPredSize = FalseBBI.NonPredSize;
  return true;
}

/// ValidDiamond - Returns true if the 'true' and 'false' blocks (along
/// with their common predecessor) forms a valid diamond shape for ifcvt.
bool IfConverter::ValidDiamond(
    BBInfo &TrueBBI, BBInfo &FalseBBI,
    unsigned &Dups1, unsigned &Dups2,
    BBInfo &TrueBBICalc, BBInfo &FalseBBICalc) const {
  Dups1 = Dups2 = 0;
  if (TrueBBI.IsBeingAnalyzed || TrueBBI.IsDone ||
      FalseBBI.IsBeingAnalyzed || FalseBBI.IsDone)
    return false;

  MachineBasicBlock *TT = TrueBBI.TrueBB;
  MachineBasicBlock *FT = FalseBBI.TrueBB;

  if (!TT && blockAlwaysFallThrough(TrueBBI))
    TT = getNextBlock(*TrueBBI.BB);
  if (!FT && blockAlwaysFallThrough(FalseBBI))
    FT = getNextBlock(*FalseBBI.BB);
  if (TT != FT)
    return false;
  if (!TT && (TrueBBI.IsBrAnalyzable || FalseBBI.IsBrAnalyzable))
    return false;
  if  (TrueBBI.BB->pred_size() > 1 || FalseBBI.BB->pred_size() > 1)
    return false;

  // FIXME: Allow true block to have an early exit?
  if (TrueBBI.FalseBB || FalseBBI.FalseBB)
    return false;

  // Count duplicate instructions at the beginning and end of the true and
  // false blocks.
  // Skip unconditional branches only if we are considering an analyzable
  // diamond. Otherwise the branches must be the same.
  bool SkipUnconditionalBranches =
      TrueBBI.IsBrAnalyzable && FalseBBI.IsBrAnalyzable;
  MachineBasicBlock::iterator TIB = TrueBBI.BB->begin();
  MachineBasicBlock::iterator FIB = FalseBBI.BB->begin();
  MachineBasicBlock::iterator TIE = TrueBBI.BB->end();
  MachineBasicBlock::iterator FIE = FalseBBI.BB->end();
  if(!CountDuplicatedInstructions(TIB, FIB, TIE, FIE, Dups1, Dups2,
                                  *TrueBBI.BB, *FalseBBI.BB,
                                  SkipUnconditionalBranches))
    return false;

  TrueBBICalc.BB = TrueBBI.BB;
  FalseBBICalc.BB = FalseBBI.BB;
  if (!RescanInstructions(TIB, FIB, TIE, FIE, TrueBBICalc, FalseBBICalc))
    return false;
  // The size is used to decide whether to if-convert, and the shared portions
  // are subtracted off. Because of the subtraction, we just use the size that
  // was calculated by the original ScanInstructions, as it is correct.
  TrueBBICalc.NonPredSize = TrueBBI.NonPredSize;
  FalseBBICalc.NonPredSize = FalseBBI.NonPredSize;
  return true;
}

/// AnalyzeBranches - Look at the branches at the end of a block to determine if
/// the block is predicable.
void IfConverter::AnalyzeBranches(BBInfo &BBI) {
  if (BBI.IsDone)
    return;

  BBI.TrueBB = BBI.FalseBB = nullptr;
  BBI.BrCond.clear();
  BBI.IsBrAnalyzable =
      !TII->analyzeBranch(*BBI.BB, BBI.TrueBB, BBI.FalseBB, BBI.BrCond);
  SmallVector<MachineOperand, 4> RevCond(BBI.BrCond.begin(), BBI.BrCond.end());
  BBI.IsBrReversible = (RevCond.size() == 0) ||
      !TII->ReverseBranchCondition(RevCond);
  BBI.HasFallThrough = BBI.IsBrAnalyzable && BBI.FalseBB == nullptr;

  if (BBI.BrCond.size()) {
    // No false branch. This BB must end with a conditional branch and a
    // fallthrough.
    if (!BBI.FalseBB)
      BBI.FalseBB = findFalseBlock(BBI.BB, BBI.TrueBB);
    if (!BBI.FalseBB) {
      // Malformed bcc? True and false blocks are the same?
      BBI.IsUnpredicable = true;
    }
  }
}

/// ScanInstructions - Scan all the instructions in the block to determine if
/// the block is predicable. In most cases, that means all the instructions
/// in the block are isPredicable(). Also checks if the block contains any
/// instruction which can clobber a predicate (e.g. condition code register).
/// If so, the block is not predicable unless it's the last instruction.
void IfConverter::ScanInstructions(BBInfo &BBI,
                                   MachineBasicBlock::iterator &Begin,
                                   MachineBasicBlock::iterator &End,
                                   bool BranchUnpredicable) const {
  if (BBI.IsDone || BBI.IsUnpredicable)
    return;

  bool AlreadyPredicated = !BBI.Predicate.empty();

  BBI.NonPredSize = 0;
  BBI.ExtraCost = 0;
  BBI.ExtraCost2 = 0;
  BBI.ClobbersPred = false;
  for (MachineInstr &MI : make_range(Begin, End)) {
    if (MI.isDebugValue())
      continue;

    // It's unsafe to duplicate convergent instructions in this context, so set
    // BBI.CannotBeCopied to true if MI is convergent.  To see why, consider the
    // following CFG, which is subject to our "simple" transformation.
    //
    //    BB0     // if (c1) goto BB1; else goto BB2;
    //   /   \
    //  BB1   |
    //   |   BB2  // if (c2) goto TBB; else goto FBB;
    //   |   / |
    //   |  /  |
    //   TBB   |
    //    |    |
    //    |   FBB
    //    |
    //    exit
    //
    // Suppose we want to move TBB's contents up into BB1 and BB2 (in BB1 they'd
    // be unconditional, and in BB2, they'd be predicated upon c2), and suppose
    // TBB contains a convergent instruction.  This is safe iff doing so does
    // not add a control-flow dependency to the convergent instruction -- i.e.,
    // it's safe iff the set of control flows that leads us to the convergent
    // instruction does not get smaller after the transformation.
    //
    // Originally we executed TBB if c1 || c2.  After the transformation, there
    // are two copies of TBB's instructions.  We get to the first if c1, and we
    // get to the second if !c1 && c2.
    //
    // There are clearly fewer ways to satisfy the condition "c1" than
    // "c1 || c2".  Since we've shrunk the set of control flows which lead to
    // our convergent instruction, the transformation is unsafe.
    if (MI.isNotDuplicable() || MI.isConvergent())
      BBI.CannotBeCopied = true;

    bool isPredicated = TII->isPredicated(MI);
    bool isCondBr = BBI.IsBrAnalyzable && MI.isConditionalBranch();

    if (BranchUnpredicable && MI.isBranch()) {
      BBI.IsUnpredicable = true;
      return;
    }

    // A conditional branch is not predicable, but it may be eliminated.
    if (isCondBr)
      continue;

    if (!isPredicated) {
      BBI.NonPredSize++;
      unsigned ExtraPredCost = TII->getPredicationCost(MI);
      unsigned NumCycles = SchedModel.computeInstrLatency(&MI, false);
      if (NumCycles > 1)
        BBI.ExtraCost += NumCycles-1;
      BBI.ExtraCost2 += ExtraPredCost;
    } else if (!AlreadyPredicated) {
      // FIXME: This instruction is already predicated before the
      // if-conversion pass. It's probably something like a conditional move.
      // Mark this block unpredicable for now.
      BBI.IsUnpredicable = true;
      return;
    }

    if (BBI.ClobbersPred && !isPredicated) {
      // Predicate modification instruction should end the block (except for
      // already predicated instructions and end of block branches).
      // Predicate may have been modified, the subsequent (currently)
      // unpredicated instructions cannot be correctly predicated.
      BBI.IsUnpredicable = true;
      return;
    }

    // FIXME: Make use of PredDefs? e.g. ADDC, SUBC sets predicates but are
    // still potentially predicable.
    std::vector<MachineOperand> PredDefs;
    if (TII->DefinesPredicate(MI, PredDefs))
      BBI.ClobbersPred = true;

    if (!TII->isPredicable(MI)) {
      BBI.IsUnpredicable = true;
      return;
    }
  }
}

/// Determine if the block is a suitable candidate to be predicated by the
/// specified predicate.
/// @param BBI BBInfo for the block to check
/// @param Pred Predicate array for the branch that leads to BBI
/// @param isTriangle true if the Analysis is for a triangle
/// @param RevBranch true if Reverse(Pred) leads to BBI (e.g. BBI is the false
///        case
/// @param hasCommonTail true if BBI shares a tail with a sibling block that
///        contains any instruction that would make the block unpredicable.
bool IfConverter::FeasibilityAnalysis(BBInfo &BBI,
                                      SmallVectorImpl<MachineOperand> &Pred,
                                      bool isTriangle, bool RevBranch,
                                      bool hasCommonTail) {
  // If the block is dead or unpredicable, then it cannot be predicated.
  // Two blocks may share a common unpredicable tail, but this doesn't prevent
  // them from being if-converted. The non-shared portion is assumed to have
  // been checked
  if (BBI.IsDone || (BBI.IsUnpredicable && !hasCommonTail))
    return false;

  // If it is already predicated but we couldn't analyze its terminator, the
  // latter might fallthrough, but we can't determine where to.
  // Conservatively avoid if-converting again.
  if (BBI.Predicate.size() && !BBI.IsBrAnalyzable)
    return false;

  // If it is already predicated, check if the new predicate subsumes
  // its predicate.
  if (BBI.Predicate.size() && !TII->SubsumesPredicate(Pred, BBI.Predicate))
    return false;

  if (!hasCommonTail && BBI.BrCond.size()) {
    if (!isTriangle)
      return false;

    // Test predicate subsumption.
    SmallVector<MachineOperand, 4> RevPred(Pred.begin(), Pred.end());
    SmallVector<MachineOperand, 4> Cond(BBI.BrCond.begin(), BBI.BrCond.end());
    if (RevBranch) {
      if (TII->ReverseBranchCondition(Cond))
        return false;
    }
    if (TII->ReverseBranchCondition(RevPred) ||
        !TII->SubsumesPredicate(Cond, RevPred))
      return false;
  }

  return true;
}

/// Analyze the structure of the sub-CFG starting from the specified block.
/// Record its successors and whether it looks like an if-conversion candidate.
void IfConverter::AnalyzeBlock(
    MachineBasicBlock &MBB, std::vector<std::unique_ptr<IfcvtToken>> &Tokens) {
  struct BBState {
    BBState(MachineBasicBlock &MBB) : MBB(&MBB), SuccsAnalyzed(false) {}
    MachineBasicBlock *MBB;

    /// This flag is true if MBB's successors have been analyzed.
    bool SuccsAnalyzed;
  };

  // Push MBB to the stack.
  SmallVector<BBState, 16> BBStack(1, MBB);

  while (!BBStack.empty()) {
    BBState &State = BBStack.back();
    MachineBasicBlock *BB = State.MBB;
    BBInfo &BBI = BBAnalysis[BB->getNumber()];

    if (!State.SuccsAnalyzed) {
      if (BBI.IsAnalyzed || BBI.IsBeingAnalyzed) {
        BBStack.pop_back();
        continue;
      }

      BBI.BB = BB;
      BBI.IsBeingAnalyzed = true;

      AnalyzeBranches(BBI);
      MachineBasicBlock::iterator Begin = BBI.BB->begin();
      MachineBasicBlock::iterator End = BBI.BB->end();
      ScanInstructions(BBI, Begin, End);

      // Unanalyzable or ends with fallthrough or unconditional branch, or if is
      // not considered for ifcvt anymore.
      if (!BBI.IsBrAnalyzable || BBI.BrCond.empty() || BBI.IsDone) {
        BBI.IsBeingAnalyzed = false;
        BBI.IsAnalyzed = true;
        BBStack.pop_back();
        continue;
      }

      // Do not ifcvt if either path is a back edge to the entry block.
      if (BBI.TrueBB == BB || BBI.FalseBB == BB) {
        BBI.IsBeingAnalyzed = false;
        BBI.IsAnalyzed = true;
        BBStack.pop_back();
        continue;
      }

      // Do not ifcvt if true and false fallthrough blocks are the same.
      if (!BBI.FalseBB) {
        BBI.IsBeingAnalyzed = false;
        BBI.IsAnalyzed = true;
        BBStack.pop_back();
        continue;
      }

      // Push the False and True blocks to the stack.
      State.SuccsAnalyzed = true;
      BBStack.push_back(*BBI.FalseBB);
      BBStack.push_back(*BBI.TrueBB);
      continue;
    }

    BBInfo &TrueBBI = BBAnalysis[BBI.TrueBB->getNumber()];
    BBInfo &FalseBBI = BBAnalysis[BBI.FalseBB->getNumber()];

    if (TrueBBI.IsDone && FalseBBI.IsDone) {
      BBI.IsBeingAnalyzed = false;
      BBI.IsAnalyzed = true;
      BBStack.pop_back();
      continue;
    }

    SmallVector<MachineOperand, 4>
        RevCond(BBI.BrCond.begin(), BBI.BrCond.end());
    bool CanRevCond = !TII->ReverseBranchCondition(RevCond);

    unsigned Dups = 0;
    unsigned Dups2 = 0;
    bool TNeedSub = !TrueBBI.Predicate.empty();
    bool FNeedSub = !FalseBBI.Predicate.empty();
    bool Enqueued = false;

    BranchProbability Prediction = MBPI->getEdgeProbability(BB, TrueBBI.BB);

    if (CanRevCond) {
      BBInfo TrueBBICalc, FalseBBICalc;
      auto feasibleDiamond = [&]() {
        bool MeetsSize = MeetIfcvtSizeLimit(
            *TrueBBI.BB, (TrueBBICalc.NonPredSize - (Dups + Dups2) +
                          TrueBBICalc.ExtraCost), TrueBBICalc.ExtraCost2,
            *FalseBBI.BB, (FalseBBICalc.NonPredSize - (Dups + Dups2) +
                           FalseBBICalc.ExtraCost), FalseBBICalc.ExtraCost2,
            Prediction);
        bool TrueFeasible = FeasibilityAnalysis(TrueBBI, BBI.BrCond,
                                                /* IsTriangle */ false, /* RevCond */ false,
                                                /* hasCommonTail */ true);
        bool FalseFeasible = FeasibilityAnalysis(FalseBBI, RevCond,
                                                 /* IsTriangle */ false, /* RevCond */ false,
                                                 /* hasCommonTail */ true);
        return MeetsSize && TrueFeasible && FalseFeasible;
      };

      if (ValidDiamond(TrueBBI, FalseBBI, Dups, Dups2,
                       TrueBBICalc, FalseBBICalc)) {
        if (feasibleDiamond()) {
          // Diamond:
          //   EBB
          //   / \_
          //  |   |
          // TBB FBB
          //   \ /
          //  TailBB
          // Note TailBB can be empty.
          Tokens.push_back(llvm::make_unique<IfcvtToken>(
              BBI, ICDiamond, TNeedSub | FNeedSub, Dups, Dups2,
              (bool) TrueBBICalc.ClobbersPred, (bool) FalseBBICalc.ClobbersPred));
          Enqueued = true;
        }
      } else if (ValidForkedDiamond(TrueBBI, FalseBBI, Dups, Dups2,
                                    TrueBBICalc, FalseBBICalc)) {
        if (feasibleDiamond()) {
          // ForkedDiamond:
          // if TBB and FBB have a common tail that includes their conditional
          // branch instructions, then we can If Convert this pattern.
          //          EBB
          //         _/ \_
          //         |   |
          //        TBB  FBB
          //        / \ /   \
          //  FalseBB TrueBB FalseBB
          //
          Tokens.push_back(llvm::make_unique<IfcvtToken>(
              BBI, ICForkedDiamond, TNeedSub | FNeedSub, Dups, Dups2,
              (bool) TrueBBICalc.ClobbersPred, (bool) FalseBBICalc.ClobbersPred));
          Enqueued = true;
        }
      }
    }

    if (ValidTriangle(TrueBBI, FalseBBI, false, Dups, Prediction) &&
        MeetIfcvtSizeLimit(*TrueBBI.BB, TrueBBI.NonPredSize + TrueBBI.ExtraCost,
                           TrueBBI.ExtraCost2, Prediction) &&
        FeasibilityAnalysis(TrueBBI, BBI.BrCond, true)) {
      // Triangle:
      //   EBB
      //   | \_
      //   |  |
      //   | TBB
      //   |  /
      //   FBB
      Tokens.push_back(
          llvm::make_unique<IfcvtToken>(BBI, ICTriangle, TNeedSub, Dups));
      Enqueued = true;
    }

    if (ValidTriangle(TrueBBI, FalseBBI, true, Dups, Prediction) &&
        MeetIfcvtSizeLimit(*TrueBBI.BB, TrueBBI.NonPredSize + TrueBBI.ExtraCost,
                           TrueBBI.ExtraCost2, Prediction) &&
        FeasibilityAnalysis(TrueBBI, BBI.BrCond, true, true)) {
      Tokens.push_back(
          llvm::make_unique<IfcvtToken>(BBI, ICTriangleRev, TNeedSub, Dups));
      Enqueued = true;
    }

    if (ValidSimple(TrueBBI, Dups, Prediction) &&
        MeetIfcvtSizeLimit(*TrueBBI.BB, TrueBBI.NonPredSize + TrueBBI.ExtraCost,
                           TrueBBI.ExtraCost2, Prediction) &&
        FeasibilityAnalysis(TrueBBI, BBI.BrCond)) {
      // Simple (split, no rejoin):
      //   EBB
      //   | \_
      //   |  |
      //   | TBB---> exit
      //   |
      //   FBB
      Tokens.push_back(
          llvm::make_unique<IfcvtToken>(BBI, ICSimple, TNeedSub, Dups));
      Enqueued = true;
    }

    if (CanRevCond) {
      // Try the other path...
      if (ValidTriangle(FalseBBI, TrueBBI, false, Dups,
                        Prediction.getCompl()) &&
          MeetIfcvtSizeLimit(*FalseBBI.BB,
                             FalseBBI.NonPredSize + FalseBBI.ExtraCost,
                             FalseBBI.ExtraCost2, Prediction.getCompl()) &&
          FeasibilityAnalysis(FalseBBI, RevCond, true)) {
        Tokens.push_back(llvm::make_unique<IfcvtToken>(BBI, ICTriangleFalse,
                                                       FNeedSub, Dups));
        Enqueued = true;
      }

      if (ValidTriangle(FalseBBI, TrueBBI, true, Dups,
                        Prediction.getCompl()) &&
          MeetIfcvtSizeLimit(*FalseBBI.BB,
                             FalseBBI.NonPredSize + FalseBBI.ExtraCost,
                           FalseBBI.ExtraCost2, Prediction.getCompl()) &&
        FeasibilityAnalysis(FalseBBI, RevCond, true, true)) {
        Tokens.push_back(
            llvm::make_unique<IfcvtToken>(BBI, ICTriangleFRev, FNeedSub, Dups));
        Enqueued = true;
      }

      if (ValidSimple(FalseBBI, Dups, Prediction.getCompl()) &&
          MeetIfcvtSizeLimit(*FalseBBI.BB,
                             FalseBBI.NonPredSize + FalseBBI.ExtraCost,
                             FalseBBI.ExtraCost2, Prediction.getCompl()) &&
          FeasibilityAnalysis(FalseBBI, RevCond)) {
        Tokens.push_back(
            llvm::make_unique<IfcvtToken>(BBI, ICSimpleFalse, FNeedSub, Dups));
        Enqueued = true;
      }
    }

    BBI.IsEnqueued = Enqueued;
    BBI.IsBeingAnalyzed = false;
    BBI.IsAnalyzed = true;
    BBStack.pop_back();
  }
}

/// Analyze all blocks and find entries for all if-conversion candidates.
void IfConverter::AnalyzeBlocks(
    MachineFunction &MF, std::vector<std::unique_ptr<IfcvtToken>> &Tokens) {
  for (MachineBasicBlock &MBB : MF)
    AnalyzeBlock(MBB, Tokens);

  // Sort to favor more complex ifcvt scheme.
  std::stable_sort(Tokens.begin(), Tokens.end(), IfcvtTokenCmp);
}

/// Returns true either if ToMBB is the next block after MBB or that all the
/// intervening blocks are empty (given MBB can fall through to its next block).
static bool canFallThroughTo(MachineBasicBlock &MBB, MachineBasicBlock &ToMBB) {
  MachineFunction::iterator PI = MBB.getIterator();
  MachineFunction::iterator I = std::next(PI);
  MachineFunction::iterator TI = ToMBB.getIterator();
  MachineFunction::iterator E = MBB.getParent()->end();
  while (I != TI) {
    // Check isSuccessor to avoid case where the next block is empty, but
    // it's not a successor.
    if (I == E || !I->empty() || !PI->isSuccessor(&*I))
      return false;
    PI = I++;
  }
  return true;
}

/// Invalidate predecessor BB info so it would be re-analyzed to determine if it
/// can be if-converted. If predecessor is already enqueued, dequeue it!
void IfConverter::InvalidatePreds(MachineBasicBlock &MBB) {
  for (const MachineBasicBlock *Predecessor : MBB.predecessors()) {
    BBInfo &PBBI = BBAnalysis[Predecessor->getNumber()];
    if (PBBI.IsDone || PBBI.BB == &MBB)
      continue;
    PBBI.IsAnalyzed = false;
    PBBI.IsEnqueued = false;
  }
}

/// Inserts an unconditional branch from \p MBB to \p ToMBB.
static void InsertUncondBranch(MachineBasicBlock &MBB, MachineBasicBlock &ToMBB,
                               const TargetInstrInfo *TII) {
  DebugLoc dl;  // FIXME: this is nowhere
  SmallVector<MachineOperand, 0> NoCond;
  TII->InsertBranch(MBB, &ToMBB, nullptr, NoCond, dl);
}

/// Remove true / false edges if either / both are no longer successors.
void IfConverter::RemoveExtraEdges(BBInfo &BBI) {
  MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
  SmallVector<MachineOperand, 4> Cond;
  if (!TII->analyzeBranch(*BBI.BB, TBB, FBB, Cond))
    BBI.BB->CorrectExtraCFGEdges(TBB, FBB, !Cond.empty());
}

/// Behaves like LiveRegUnits::StepForward() but also adds implicit uses to all
/// values defined in MI which are also live/used by MI.
static void UpdatePredRedefs(MachineInstr &MI, LivePhysRegs &Redefs) {
  const TargetRegisterInfo *TRI = MI.getParent()->getParent()
    ->getSubtarget().getRegisterInfo();

  // Before stepping forward past MI, remember which regs were live
  // before MI. This is needed to set the Undef flag only when reg is
  // dead.
  SparseSet<unsigned> LiveBeforeMI;
  LiveBeforeMI.setUniverse(TRI->getNumRegs());
  for (unsigned Reg : Redefs)
    LiveBeforeMI.insert(Reg);

  SmallVector<std::pair<unsigned, const MachineOperand*>, 4> Clobbers;
  Redefs.stepForward(MI, Clobbers);

  // Now add the implicit uses for each of the clobbered values.
  for (auto Clobber : Clobbers) {
    // FIXME: Const cast here is nasty, but better than making StepForward
    // take a mutable instruction instead of const.
    unsigned Reg = Clobber.first;
    MachineOperand &Op = const_cast<MachineOperand&>(*Clobber.second);
    MachineInstr *OpMI = Op.getParent();
    MachineInstrBuilder MIB(*OpMI->getParent()->getParent(), OpMI);
    if (Op.isRegMask()) {
      // First handle regmasks.  They clobber any entries in the mask which
      // means that we need a def for those registers.
      if (LiveBeforeMI.count(Reg))
        MIB.addReg(Reg, RegState::Implicit);

      // We also need to add an implicit def of this register for the later
      // use to read from.
      // For the register allocator to have allocated a register clobbered
      // by the call which is used later, it must be the case that
      // the call doesn't return.
      MIB.addReg(Reg, RegState::Implicit | RegState::Define);
      continue;
    }
    assert(Op.isReg() && "Register operand required");
    if (Op.isDead()) {
      // If we found a dead def, but it needs to be live, then remove the dead
      // flag.
      if (Redefs.contains(Op.getReg()))
        Op.setIsDead(false);
    }
    if (LiveBeforeMI.count(Reg))
      MIB.addReg(Reg, RegState::Implicit);
  }
}

/// Remove kill flags from operands with a registers in the \p DontKill set.
static void RemoveKills(MachineInstr &MI, const LivePhysRegs &DontKill) {
  for (MIBundleOperands O(MI); O.isValid(); ++O) {
    if (!O->isReg() || !O->isKill())
      continue;
    if (DontKill.contains(O->getReg()))
      O->setIsKill(false);
  }
}

/// Walks a range of machine instructions and removes kill flags for registers
/// in the \p DontKill set.
static void RemoveKills(MachineBasicBlock::iterator I,
                        MachineBasicBlock::iterator E,
                        const LivePhysRegs &DontKill,
                        const MCRegisterInfo &MCRI) {
  for (MachineInstr &MI : make_range(I, E))
    RemoveKills(MI, DontKill);
}

/// If convert a simple (split, no rejoin) sub-CFG.
bool IfConverter::IfConvertSimple(BBInfo &BBI, IfcvtKind Kind) {
  BBInfo &TrueBBI  = BBAnalysis[BBI.TrueBB->getNumber()];
  BBInfo &FalseBBI = BBAnalysis[BBI.FalseBB->getNumber()];
  BBInfo *CvtBBI = &TrueBBI;
  BBInfo *NextBBI = &FalseBBI;

  SmallVector<MachineOperand, 4> Cond(BBI.BrCond.begin(), BBI.BrCond.end());
  if (Kind == ICSimpleFalse)
    std::swap(CvtBBI, NextBBI);

  MachineBasicBlock &CvtMBB = *CvtBBI->BB;
  MachineBasicBlock &NextMBB = *NextBBI->BB;
  if (CvtBBI->IsDone ||
      (CvtBBI->CannotBeCopied && CvtMBB.pred_size() > 1)) {
    // Something has changed. It's no longer safe to predicate this block.
    BBI.IsAnalyzed = false;
    CvtBBI->IsAnalyzed = false;
    return false;
  }

  if (CvtMBB.hasAddressTaken())
    // Conservatively abort if-conversion if BB's address is taken.
    return false;

  if (Kind == ICSimpleFalse)
    if (TII->ReverseBranchCondition(Cond))
      llvm_unreachable("Unable to reverse branch condition!");

  // Initialize liveins to the first BB. These are potentiall redefined by
  // predicated instructions.
  Redefs.init(TRI);
  Redefs.addLiveIns(CvtMBB);
  Redefs.addLiveIns(NextMBB);

  // Compute a set of registers which must not be killed by instructions in
  // BB1: This is everything live-in to BB2.
  DontKill.init(TRI);
  DontKill.addLiveIns(NextMBB);

  if (CvtMBB.pred_size() > 1) {
    BBI.NonPredSize -= TII->RemoveBranch(*BBI.BB);
    // Copy instructions in the true block, predicate them, and add them to
    // the entry block.
    CopyAndPredicateBlock(BBI, *CvtBBI, Cond);

    // RemoveExtraEdges won't work if the block has an unanalyzable branch, so
    // explicitly remove CvtBBI as a successor.
    BBI.BB->removeSuccessor(&CvtMBB, true);
  } else {
    RemoveKills(CvtMBB.begin(), CvtMBB.end(), DontKill, *TRI);
    PredicateBlock(*CvtBBI, CvtMBB.end(), Cond);

    // Merge converted block into entry block.
    BBI.NonPredSize -= TII->RemoveBranch(*BBI.BB);
    MergeBlocks(BBI, *CvtBBI);
  }

  bool IterIfcvt = true;
  if (!canFallThroughTo(*BBI.BB, NextMBB)) {
    InsertUncondBranch(*BBI.BB, NextMBB, TII);
    BBI.HasFallThrough = false;
    // Now ifcvt'd block will look like this:
    // BB:
    // ...
    // t, f = cmp
    // if t op
    // b BBf
    //
    // We cannot further ifcvt this block because the unconditional branch
    // will have to be predicated on the new condition, that will not be
    // available if cmp executes.
    IterIfcvt = false;
  }

  RemoveExtraEdges(BBI);

  // Update block info. BB can be iteratively if-converted.
  if (!IterIfcvt)
    BBI.IsDone = true;
  InvalidatePreds(*BBI.BB);
  CvtBBI->IsDone = true;

  // FIXME: Must maintain LiveIns.
  return true;
}

/// If convert a triangle sub-CFG.
bool IfConverter::IfConvertTriangle(BBInfo &BBI, IfcvtKind Kind) {
  BBInfo &TrueBBI = BBAnalysis[BBI.TrueBB->getNumber()];
  BBInfo &FalseBBI = BBAnalysis[BBI.FalseBB->getNumber()];
  BBInfo *CvtBBI = &TrueBBI;
  BBInfo *NextBBI = &FalseBBI;
  DebugLoc dl;  // FIXME: this is nowhere

  SmallVector<MachineOperand, 4> Cond(BBI.BrCond.begin(), BBI.BrCond.end());
  if (Kind == ICTriangleFalse || Kind == ICTriangleFRev)
    std::swap(CvtBBI, NextBBI);

  MachineBasicBlock &CvtMBB = *CvtBBI->BB;
  MachineBasicBlock &NextMBB = *NextBBI->BB;
  if (CvtBBI->IsDone ||
      (CvtBBI->CannotBeCopied && CvtMBB.pred_size() > 1)) {
    // Something has changed. It's no longer safe to predicate this block.
    BBI.IsAnalyzed = false;
    CvtBBI->IsAnalyzed = false;
    return false;
  }

  if (CvtMBB.hasAddressTaken())
    // Conservatively abort if-conversion if BB's address is taken.
    return false;

  if (Kind == ICTriangleFalse || Kind == ICTriangleFRev)
    if (TII->ReverseBranchCondition(Cond))
      llvm_unreachable("Unable to reverse branch condition!");

  if (Kind == ICTriangleRev || Kind == ICTriangleFRev) {
    if (ReverseBranchCondition(*CvtBBI)) {
      // BB has been changed, modify its predecessors (except for this
      // one) so they don't get ifcvt'ed based on bad intel.
      for (MachineBasicBlock *PBB : CvtMBB.predecessors()) {
        if (PBB == BBI.BB)
          continue;
        BBInfo &PBBI = BBAnalysis[PBB->getNumber()];
        if (PBBI.IsEnqueued) {
          PBBI.IsAnalyzed = false;
          PBBI.IsEnqueued = false;
        }
      }
    }
  }

  // Initialize liveins to the first BB. These are potentially redefined by
  // predicated instructions.
  Redefs.init(TRI);
  Redefs.addLiveIns(CvtMBB);
  Redefs.addLiveIns(NextMBB);

  DontKill.clear();

  bool HasEarlyExit = CvtBBI->FalseBB != nullptr;
  BranchProbability CvtNext, CvtFalse, BBNext, BBCvt;

  if (HasEarlyExit) {
    // Get probabilities before modifying CvtMBB and BBI.BB.
    CvtNext = MBPI->getEdgeProbability(&CvtMBB, &NextMBB);
    CvtFalse = MBPI->getEdgeProbability(&CvtMBB, CvtBBI->FalseBB);
    BBNext = MBPI->getEdgeProbability(BBI.BB, &NextMBB);
    BBCvt = MBPI->getEdgeProbability(BBI.BB, &CvtMBB);
  }

  if (CvtMBB.pred_size() > 1) {
    BBI.NonPredSize -= TII->RemoveBranch(*BBI.BB);
    // Copy instructions in the true block, predicate them, and add them to
    // the entry block.
    CopyAndPredicateBlock(BBI, *CvtBBI, Cond, true);

    // RemoveExtraEdges won't work if the block has an unanalyzable branch, so
    // explicitly remove CvtBBI as a successor.
    BBI.BB->removeSuccessor(&CvtMBB, true);
  } else {
    // Predicate the 'true' block after removing its branch.
    CvtBBI->NonPredSize -= TII->RemoveBranch(CvtMBB);
    PredicateBlock(*CvtBBI, CvtMBB.end(), Cond);

    // Now merge the entry of the triangle with the true block.
    BBI.NonPredSize -= TII->RemoveBranch(*BBI.BB);
    MergeBlocks(BBI, *CvtBBI, false);
  }

  // If 'true' block has a 'false' successor, add an exit branch to it.
  if (HasEarlyExit) {
    SmallVector<MachineOperand, 4> RevCond(CvtBBI->BrCond.begin(),
                                           CvtBBI->BrCond.end());
    if (TII->ReverseBranchCondition(RevCond))
      llvm_unreachable("Unable to reverse branch condition!");

    // Update the edge probability for both CvtBBI->FalseBB and NextBBI.
    // NewNext = New_Prob(BBI.BB, NextMBB) =
    //   Prob(BBI.BB, NextMBB) +
    //   Prob(BBI.BB, CvtMBB) * Prob(CvtMBB, NextMBB)
    // NewFalse = New_Prob(BBI.BB, CvtBBI->FalseBB) =
    //   Prob(BBI.BB, CvtMBB) * Prob(CvtMBB, CvtBBI->FalseBB)
    auto NewTrueBB = getNextBlock(*BBI.BB);
    auto NewNext = BBNext + BBCvt * CvtNext;
    auto NewTrueBBIter = find(BBI.BB->successors(), NewTrueBB);
    if (NewTrueBBIter != BBI.BB->succ_end())
      BBI.BB->setSuccProbability(NewTrueBBIter, NewNext);

    auto NewFalse = BBCvt * CvtFalse;
    TII->InsertBranch(*BBI.BB, CvtBBI->FalseBB, nullptr, RevCond, dl);
    BBI.BB->addSuccessor(CvtBBI->FalseBB, NewFalse);
  }

  // Merge in the 'false' block if the 'false' block has no other
  // predecessors. Otherwise, add an unconditional branch to 'false'.
  bool FalseBBDead = false;
  bool IterIfcvt = true;
  bool isFallThrough = canFallThroughTo(*BBI.BB, NextMBB);
  if (!isFallThrough) {
    // Only merge them if the true block does not fallthrough to the false
    // block. By not merging them, we make it possible to iteratively
    // ifcvt the blocks.
    if (!HasEarlyExit &&
        NextMBB.pred_size() == 1 && !NextBBI->HasFallThrough &&
        !NextMBB.hasAddressTaken()) {
      MergeBlocks(BBI, *NextBBI);
      FalseBBDead = true;
    } else {
      InsertUncondBranch(*BBI.BB, NextMBB, TII);
      BBI.HasFallThrough = false;
    }
    // Mixed predicated and unpredicated code. This cannot be iteratively
    // predicated.
    IterIfcvt = false;
  }

  RemoveExtraEdges(BBI);

  // Update block info. BB can be iteratively if-converted.
  if (!IterIfcvt)
    BBI.IsDone = true;
  InvalidatePreds(*BBI.BB);
  CvtBBI->IsDone = true;
  if (FalseBBDead)
    NextBBI->IsDone = true;

  // FIXME: Must maintain LiveIns.
  return true;
}

/// Common code shared between diamond conversions.
/// \p BBI, \p TrueBBI, and \p FalseBBI form the diamond shape.
/// \p NumDups1 - number of shared instructions at the beginning of \p TrueBBI
///               and FalseBBI
/// \p NumDups2 - number of shared instructions at the end of \p TrueBBI
///               and \p FalseBBI
/// \p RemoveBranch - Remove the common branch of the two blocks before
///                   predicating. Only false for unanalyzable fallthrough
///                   cases. The caller will replace the branch if necessary.
/// \p MergeAddEdges - Add successor edges when merging blocks. Only false for
///                    unanalyzable fallthrough
bool IfConverter::IfConvertDiamondCommon(
    BBInfo &BBI, BBInfo &TrueBBI, BBInfo &FalseBBI,
    unsigned NumDups1, unsigned NumDups2,
    bool TClobbersPred, bool FClobbersPred,
    bool RemoveBranch, bool MergeAddEdges) {

  if (TrueBBI.IsDone || FalseBBI.IsDone ||
      TrueBBI.BB->pred_size() > 1 || FalseBBI.BB->pred_size() > 1) {
    // Something has changed. It's no longer safe to predicate these blocks.
    BBI.IsAnalyzed = false;
    TrueBBI.IsAnalyzed = false;
    FalseBBI.IsAnalyzed = false;
    return false;
  }

  if (TrueBBI.BB->hasAddressTaken() || FalseBBI.BB->hasAddressTaken())
    // Conservatively abort if-conversion if either BB has its address taken.
    return false;

  // Put the predicated instructions from the 'true' block before the
  // instructions from the 'false' block, unless the true block would clobber
  // the predicate, in which case, do the opposite.
  BBInfo *BBI1 = &TrueBBI;
  BBInfo *BBI2 = &FalseBBI;
  SmallVector<MachineOperand, 4> RevCond(BBI.BrCond.begin(), BBI.BrCond.end());
  if (TII->ReverseBranchCondition(RevCond))
    llvm_unreachable("Unable to reverse branch condition!");
  SmallVector<MachineOperand, 4> *Cond1 = &BBI.BrCond;
  SmallVector<MachineOperand, 4> *Cond2 = &RevCond;

  // Figure out the more profitable ordering.
  bool DoSwap = false;
  if (TClobbersPred && !FClobbersPred)
    DoSwap = true;
  else if (TClobbersPred == FClobbersPred) {
    if (TrueBBI.NonPredSize > FalseBBI.NonPredSize)
      DoSwap = true;
  }
  if (DoSwap) {
    std::swap(BBI1, BBI2);
    std::swap(Cond1, Cond2);
  }

  // Remove the conditional branch from entry to the blocks.
  BBI.NonPredSize -= TII->RemoveBranch(*BBI.BB);

  MachineBasicBlock &MBB1 = *BBI1->BB;
  MachineBasicBlock &MBB2 = *BBI2->BB;

  // Initialize the Redefs:
  // - BB2 live-in regs need implicit uses before being redefined by BB1
  //   instructions.
  // - BB1 live-out regs need implicit uses before being redefined by BB2
  //   instructions. We start with BB1 live-ins so we have the live-out regs
  //   after tracking the BB1 instructions.
  Redefs.init(TRI);
  Redefs.addLiveIns(MBB1);
  Redefs.addLiveIns(MBB2);

  // Remove the duplicated instructions at the beginnings of both paths.
  // Skip dbg_value instructions
  MachineBasicBlock::iterator DI1 = MBB1.getFirstNonDebugInstr();
  MachineBasicBlock::iterator DI2 = MBB2.getFirstNonDebugInstr();
  BBI1->NonPredSize -= NumDups1;
  BBI2->NonPredSize -= NumDups1;

  // Skip past the dups on each side separately since there may be
  // differing dbg_value entries.
  for (unsigned i = 0; i < NumDups1; ++DI1) {
    if (!DI1->isDebugValue())
      ++i;
  }
  while (NumDups1 != 0) {
    ++DI2;
    if (!DI2->isDebugValue())
      --NumDups1;
  }

  // Compute a set of registers which must not be killed by instructions in BB1:
  // This is everything used+live in BB2 after the duplicated instructions. We
  // can compute this set by simulating liveness backwards from the end of BB2.
  DontKill.init(TRI);
  for (const MachineInstr &MI :
       make_range(MBB2.rbegin(), MachineBasicBlock::reverse_iterator(DI2)))
    DontKill.stepBackward(MI);

  for (const MachineInstr &MI : make_range(MBB1.begin(), DI1)) {
    SmallVector<std::pair<unsigned, const MachineOperand*>, 4> IgnoredClobbers;
    Redefs.stepForward(MI, IgnoredClobbers);
  }
  BBI.BB->splice(BBI.BB->end(), &MBB1, MBB1.begin(), DI1);
  MBB2.erase(MBB2.begin(), DI2);

  // The branches have been checked to match, so it is safe to remove the branch
  // in BB1 and rely on the copy in BB2
#ifndef NDEBUG
  // Unanalyzable branches must match exactly. Check that now.
  if (!BBI1->IsBrAnalyzable)
    verifySameBranchInstructions(&MBB1, &MBB2);
#endif
  BBI1->NonPredSize -= TII->RemoveBranch(*BBI1->BB);
  // Remove duplicated instructions.
  DI1 = MBB1.end();
  for (unsigned i = 0; i != NumDups2; ) {
    // NumDups2 only counted non-dbg_value instructions, so this won't
    // run off the head of the list.
    assert(DI1 != MBB1.begin());
    --DI1;
    // skip dbg_value instructions
    if (!DI1->isDebugValue())
      ++i;
  }
  MBB1.erase(DI1, MBB1.end());

  // Kill flags in the true block for registers living into the false block
  // must be removed.
  RemoveKills(MBB1.begin(), MBB1.end(), DontKill, *TRI);

  DI2 = BBI2->BB->end();
  // The branches have been checked to match. Skip over the branch in the false
  // block so that we don't try to predicate it.
  if (RemoveBranch)
    BBI2->NonPredSize -= TII->RemoveBranch(*BBI2->BB);
  else {
    do {
      assert(DI2 != MBB2.begin());
      DI2--;
    } while (DI2->isBranch() || DI2->isDebugValue());
    DI2++;
  }
  while (NumDups2 != 0) {
    // NumDups2 only counted non-dbg_value instructions, so this won't
    // run off the head of the list.
    assert(DI2 != MBB2.begin());
    --DI2;
    // skip dbg_value instructions
    if (!DI2->isDebugValue())
      --NumDups2;
  }

  // Remember which registers would later be defined by the false block.
  // This allows us not to predicate instructions in the true block that would
  // later be re-defined. That is, rather than
  //   subeq  r0, r1, #1
  //   addne  r0, r1, #1
  // generate:
  //   sub    r0, r1, #1
  //   addne  r0, r1, #1
  SmallSet<unsigned, 4> RedefsByFalse;
  SmallSet<unsigned, 4> ExtUses;
  if (TII->isProfitableToUnpredicate(MBB1, MBB2)) {
    for (const MachineInstr &FI : make_range(MBB2.begin(), DI2)) {
      if (FI.isDebugValue())
        continue;
      SmallVector<unsigned, 4> Defs;
      for (const MachineOperand &MO : FI.operands()) {
        if (!MO.isReg())
          continue;
        unsigned Reg = MO.getReg();
        if (!Reg)
          continue;
        if (MO.isDef()) {
          Defs.push_back(Reg);
        } else if (!RedefsByFalse.count(Reg)) {
          // These are defined before ctrl flow reach the 'false' instructions.
          // They cannot be modified by the 'true' instructions.
          for (MCSubRegIterator SubRegs(Reg, TRI, /*IncludeSelf=*/true);
               SubRegs.isValid(); ++SubRegs)
            ExtUses.insert(*SubRegs);
        }
      }

      for (unsigned Reg : Defs) {
        if (!ExtUses.count(Reg)) {
          for (MCSubRegIterator SubRegs(Reg, TRI, /*IncludeSelf=*/true);
               SubRegs.isValid(); ++SubRegs)
            RedefsByFalse.insert(*SubRegs);
        }
      }
    }
  }

  // Predicate the 'true' block.
  PredicateBlock(*BBI1, MBB1.end(), *Cond1, &RedefsByFalse);

  // After predicating BBI1, if there is a predicated terminator in BBI1 and
  // a non-predicated in BBI2, then we don't want to predicate the one from
  // BBI2. The reason is that if we merged these blocks, we would end up with
  // two predicated terminators in the same block.
  if (!MBB2.empty() && (DI2 == MBB2.end())) {
    MachineBasicBlock::iterator BBI1T = MBB1.getFirstTerminator();
    MachineBasicBlock::iterator BBI2T = MBB2.getFirstTerminator();
    if (BBI1T != MBB1.end() && TII->isPredicated(*BBI1T) &&
        BBI2T != MBB2.end() && !TII->isPredicated(*BBI2T))
      --DI2;
  }

  // Predicate the 'false' block.
  PredicateBlock(*BBI2, DI2, *Cond2);

  // Merge the true block into the entry of the diamond.
  MergeBlocks(BBI, *BBI1, MergeAddEdges);
  MergeBlocks(BBI, *BBI2, MergeAddEdges);
  return true;
}

/// If convert an almost-diamond sub-CFG where the true
/// and false blocks share a common tail.
bool IfConverter::IfConvertForkedDiamond(
    BBInfo &BBI, IfcvtKind Kind,
    unsigned NumDups1, unsigned NumDups2,
    bool TClobbersPred, bool FClobbersPred) {
  BBInfo &TrueBBI  = BBAnalysis[BBI.TrueBB->getNumber()];
  BBInfo &FalseBBI = BBAnalysis[BBI.FalseBB->getNumber()];

  // Save the debug location for later.
  DebugLoc dl;
  MachineBasicBlock::iterator TIE = TrueBBI.BB->getFirstTerminator();
  if (TIE != TrueBBI.BB->end())
    dl = TIE->getDebugLoc();
  // Removing branches from both blocks is safe, because we have already
  // determined that both blocks have the same branch instructions. The branch
  // will be added back at the end, unpredicated.
  if (!IfConvertDiamondCommon(
      BBI, TrueBBI, FalseBBI,
      NumDups1, NumDups2,
      TClobbersPred, FClobbersPred,
      /* RemoveBranch */ true, /* MergeAddEdges */ true))
    return false;

  // Add back the branch.
  // Debug location saved above when removing the branch from BBI2
  TII->InsertBranch(*BBI.BB, TrueBBI.TrueBB, TrueBBI.FalseBB,
                    TrueBBI.BrCond, dl);

  RemoveExtraEdges(BBI);

  // Update block info.
  BBI.IsDone = TrueBBI.IsDone = FalseBBI.IsDone = true;
  InvalidatePreds(*BBI.BB);

  // FIXME: Must maintain LiveIns.
  return true;
}

/// If convert a diamond sub-CFG.
bool IfConverter::IfConvertDiamond(BBInfo &BBI, IfcvtKind Kind,
                                   unsigned NumDups1, unsigned NumDups2,
                                   bool TClobbersPred, bool FClobbersPred) {
  BBInfo &TrueBBI  = BBAnalysis[BBI.TrueBB->getNumber()];
  BBInfo &FalseBBI = BBAnalysis[BBI.FalseBB->getNumber()];
  MachineBasicBlock *TailBB = TrueBBI.TrueBB;

  // True block must fall through or end with an unanalyzable terminator.
  if (!TailBB) {
    if (blockAlwaysFallThrough(TrueBBI))
      TailBB = FalseBBI.TrueBB;
    assert((TailBB || !TrueBBI.IsBrAnalyzable) && "Unexpected!");
  }

  if (!IfConvertDiamondCommon(
      BBI, TrueBBI, FalseBBI,
      NumDups1, NumDups2,
      TrueBBI.ClobbersPred, FalseBBI.ClobbersPred,
      /* RemoveBranch */ TrueBBI.IsBrAnalyzable,
      /* MergeAddEdges */ TailBB == nullptr))
    return false;

  // If the if-converted block falls through or unconditionally branches into
  // the tail block, and the tail block does not have other predecessors, then
  // fold the tail block in as well. Otherwise, unless it falls through to the
  // tail, add a unconditional branch to it.
  if (TailBB) {
    BBInfo &TailBBI = BBAnalysis[TailBB->getNumber()];
    bool CanMergeTail = !TailBBI.HasFallThrough &&
      !TailBBI.BB->hasAddressTaken();
    // The if-converted block can still have a predicated terminator
    // (e.g. a predicated return). If that is the case, we cannot merge
    // it with the tail block.
    MachineBasicBlock::const_iterator TI = BBI.BB->getFirstTerminator();
    if (TI != BBI.BB->end() && TII->isPredicated(*TI))
      CanMergeTail = false;
    // There may still be a fall-through edge from BBI1 or BBI2 to TailBB;
    // check if there are any other predecessors besides those.
    unsigned NumPreds = TailBB->pred_size();
    if (NumPreds > 1)
      CanMergeTail = false;
    else if (NumPreds == 1 && CanMergeTail) {
      MachineBasicBlock::pred_iterator PI = TailBB->pred_begin();
      if (*PI != TrueBBI.BB && *PI != FalseBBI.BB)
        CanMergeTail = false;
    }
    if (CanMergeTail) {
      MergeBlocks(BBI, TailBBI);
      TailBBI.IsDone = true;
    } else {
      BBI.BB->addSuccessor(TailBB, BranchProbability::getOne());
      InsertUncondBranch(*BBI.BB, *TailBB, TII);
      BBI.HasFallThrough = false;
    }
  }

  // RemoveExtraEdges won't work if the block has an unanalyzable branch,
  // which can happen here if TailBB is unanalyzable and is merged, so
  // explicitly remove BBI1 and BBI2 as successors.
  BBI.BB->removeSuccessor(TrueBBI.BB);
  BBI.BB->removeSuccessor(FalseBBI.BB, /* NormalizeSuccessProbs */ true);
  RemoveExtraEdges(BBI);

  // Update block info.
  BBI.IsDone = TrueBBI.IsDone = FalseBBI.IsDone = true;
  InvalidatePreds(*BBI.BB);

  // FIXME: Must maintain LiveIns.
  return true;
}

static bool MaySpeculate(const MachineInstr &MI,
                         SmallSet<unsigned, 4> &LaterRedefs) {
  bool SawStore = true;
  if (!MI.isSafeToMove(nullptr, SawStore))
    return false;

  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg())
      continue;
    unsigned Reg = MO.getReg();
    if (!Reg)
      continue;
    if (MO.isDef() && !LaterRedefs.count(Reg))
      return false;
  }

  return true;
}

/// Predicate instructions from the start of the block to the specified end with
/// the specified condition.
void IfConverter::PredicateBlock(BBInfo &BBI,
                                 MachineBasicBlock::iterator E,
                                 SmallVectorImpl<MachineOperand> &Cond,
                                 SmallSet<unsigned, 4> *LaterRedefs) {
  bool AnyUnpred = false;
  bool MaySpec = LaterRedefs != nullptr;
  for (MachineInstr &I : make_range(BBI.BB->begin(), E)) {
    if (I.isDebugValue() || TII->isPredicated(I))
      continue;
    // It may be possible not to predicate an instruction if it's the 'true'
    // side of a diamond and the 'false' side may re-define the instruction's
    // defs.
    if (MaySpec && MaySpeculate(I, *LaterRedefs)) {
      AnyUnpred = true;
      continue;
    }
    // If any instruction is predicated, then every instruction after it must
    // be predicated.
    MaySpec = false;
    if (!TII->PredicateInstruction(I, Cond)) {
#ifndef NDEBUG
      dbgs() << "Unable to predicate " << I << "!\n";
#endif
      llvm_unreachable(nullptr);
    }

    // If the predicated instruction now redefines a register as the result of
    // if-conversion, add an implicit kill.
    UpdatePredRedefs(I, Redefs);
  }

  BBI.Predicate.append(Cond.begin(), Cond.end());

  BBI.IsAnalyzed = false;
  BBI.NonPredSize = 0;

  ++NumIfConvBBs;
  if (AnyUnpred)
    ++NumUnpred;
}

/// Copy and predicate instructions from source BB to the destination block.
/// Skip end of block branches if IgnoreBr is true.
void IfConverter::CopyAndPredicateBlock(BBInfo &ToBBI, BBInfo &FromBBI,
                                        SmallVectorImpl<MachineOperand> &Cond,
                                        bool IgnoreBr) {
  MachineFunction &MF = *ToBBI.BB->getParent();

  MachineBasicBlock &FromMBB = *FromBBI.BB;
  for (MachineInstr &I : FromMBB) {
    // Do not copy the end of the block branches.
    if (IgnoreBr && I.isBranch())
      break;

    MachineInstr *MI = MF.CloneMachineInstr(&I);
    ToBBI.BB->insert(ToBBI.BB->end(), MI);
    ToBBI.NonPredSize++;
    unsigned ExtraPredCost = TII->getPredicationCost(I);
    unsigned NumCycles = SchedModel.computeInstrLatency(&I, false);
    if (NumCycles > 1)
      ToBBI.ExtraCost += NumCycles-1;
    ToBBI.ExtraCost2 += ExtraPredCost;

    if (!TII->isPredicated(I) && !MI->isDebugValue()) {
      if (!TII->PredicateInstruction(*MI, Cond)) {
#ifndef NDEBUG
        dbgs() << "Unable to predicate " << I << "!\n";
#endif
        llvm_unreachable(nullptr);
      }
    }

    // If the predicated instruction now redefines a register as the result of
    // if-conversion, add an implicit kill.
    UpdatePredRedefs(*MI, Redefs);

    // Some kill flags may not be correct anymore.
    if (!DontKill.empty())
      RemoveKills(*MI, DontKill);
  }

  if (!IgnoreBr) {
    std::vector<MachineBasicBlock *> Succs(FromMBB.succ_begin(),
                                           FromMBB.succ_end());
    MachineBasicBlock *NBB = getNextBlock(FromMBB);
    MachineBasicBlock *FallThrough = FromBBI.HasFallThrough ? NBB : nullptr;

    for (MachineBasicBlock *Succ : Succs) {
      // Fallthrough edge can't be transferred.
      if (Succ == FallThrough)
        continue;
      ToBBI.BB->addSuccessor(Succ);
    }
  }

  ToBBI.Predicate.append(FromBBI.Predicate.begin(), FromBBI.Predicate.end());
  ToBBI.Predicate.append(Cond.begin(), Cond.end());

  ToBBI.ClobbersPred |= FromBBI.ClobbersPred;
  ToBBI.IsAnalyzed = false;

  ++NumDupBBs;
}

/// Move all instructions from FromBB to the end of ToBB.  This will leave
/// FromBB as an empty block, so remove all of its successor edges except for
/// the fall-through edge.  If AddEdges is true, i.e., when FromBBI's branch is
/// being moved, add those successor edges to ToBBI.
void IfConverter::MergeBlocks(BBInfo &ToBBI, BBInfo &FromBBI, bool AddEdges) {
  MachineBasicBlock &FromMBB = *FromBBI.BB;
  assert(!FromMBB.hasAddressTaken() &&
         "Removing a BB whose address is taken!");

  // In case FromMBB contains terminators (e.g. return instruction),
  // first move the non-terminator instructions, then the terminators.
  MachineBasicBlock::iterator FromTI = FromMBB.getFirstTerminator();
  MachineBasicBlock::iterator ToTI = ToBBI.BB->getFirstTerminator();
  ToBBI.BB->splice(ToTI, &FromMBB, FromMBB.begin(), FromTI);

  // If FromBB has non-predicated terminator we should copy it at the end.
  if (FromTI != FromMBB.end() && !TII->isPredicated(*FromTI))
    ToTI = ToBBI.BB->end();
  ToBBI.BB->splice(ToTI, &FromMBB, FromTI, FromMBB.end());

  // Force normalizing the successors' probabilities of ToBBI.BB to convert all
  // unknown probabilities into known ones.
  // FIXME: This usage is too tricky and in the future we would like to
  // eliminate all unknown probabilities in MBB.
  ToBBI.BB->normalizeSuccProbs();

  SmallVector<MachineBasicBlock *, 4> FromSuccs(FromMBB.succ_begin(),
                                                FromMBB.succ_end());
  MachineBasicBlock *NBB = getNextBlock(FromMBB);
  MachineBasicBlock *FallThrough = FromBBI.HasFallThrough ? NBB : nullptr;
  // The edge probability from ToBBI.BB to FromMBB, which is only needed when
  // AddEdges is true and FromMBB is a successor of ToBBI.BB.
  auto To2FromProb = BranchProbability::getZero();
  if (AddEdges && ToBBI.BB->isSuccessor(&FromMBB)) {
    To2FromProb = MBPI->getEdgeProbability(ToBBI.BB, &FromMBB);
    // Set the edge probability from ToBBI.BB to FromMBB to zero to avoid the
    // edge probability being merged to other edges when this edge is removed
    // later.
    ToBBI.BB->setSuccProbability(find(ToBBI.BB->successors(), &FromMBB),
                                 BranchProbability::getZero());
  }

  for (MachineBasicBlock *Succ : FromSuccs) {
    // Fallthrough edge can't be transferred.
    if (Succ == FallThrough)
      continue;

    auto NewProb = BranchProbability::getZero();
    if (AddEdges) {
      // Calculate the edge probability for the edge from ToBBI.BB to Succ,
      // which is a portion of the edge probability from FromMBB to Succ. The
      // portion ratio is the edge probability from ToBBI.BB to FromMBB (if
      // FromBBI is a successor of ToBBI.BB. See comment below for excepion).
      NewProb = MBPI->getEdgeProbability(&FromMBB, Succ);

      // To2FromProb is 0 when FromMBB is not a successor of ToBBI.BB. This
      // only happens when if-converting a diamond CFG and FromMBB is the
      // tail BB.  In this case FromMBB post-dominates ToBBI.BB and hence we
      // could just use the probabilities on FromMBB's out-edges when adding
      // new successors.
      if (!To2FromProb.isZero())
        NewProb *= To2FromProb;
    }

    FromMBB.removeSuccessor(Succ);

    if (AddEdges) {
      // If the edge from ToBBI.BB to Succ already exists, update the
      // probability of this edge by adding NewProb to it. An example is shown
      // below, in which A is ToBBI.BB and B is FromMBB. In this case we
      // don't have to set C as A's successor as it already is. We only need to
      // update the edge probability on A->C. Note that B will not be
      // immediately removed from A's successors. It is possible that B->D is
      // not removed either if D is a fallthrough of B. Later the edge A->D
      // (generated here) and B->D will be combined into one edge. To maintain
      // correct edge probability of this combined edge, we need to set the edge
      // probability of A->B to zero, which is already done above. The edge
      // probability on A->D is calculated by scaling the original probability
      // on A->B by the probability of B->D.
      //
      // Before ifcvt:      After ifcvt (assume B->D is kept):
      //
      //       A                A
      //      /|               /|\
      //     / B              / B|
      //    | /|             |  ||
      //    |/ |             |  |/
      //    C  D             C  D
      //
      if (ToBBI.BB->isSuccessor(Succ))
        ToBBI.BB->setSuccProbability(
            find(ToBBI.BB->successors(), Succ),
            MBPI->getEdgeProbability(ToBBI.BB, Succ) + NewProb);
      else
        ToBBI.BB->addSuccessor(Succ, NewProb);
    }
  }

  // Now FromBBI always falls through to the next block!
  if (NBB && !FromMBB.isSuccessor(NBB))
    FromMBB.addSuccessor(NBB);

  // Normalize the probabilities of ToBBI.BB's successors with all adjustment
  // we've done above.
  ToBBI.BB->normalizeSuccProbs();

  ToBBI.Predicate.append(FromBBI.Predicate.begin(), FromBBI.Predicate.end());
  FromBBI.Predicate.clear();

  ToBBI.NonPredSize += FromBBI.NonPredSize;
  ToBBI.ExtraCost += FromBBI.ExtraCost;
  ToBBI.ExtraCost2 += FromBBI.ExtraCost2;
  FromBBI.NonPredSize = 0;
  FromBBI.ExtraCost = 0;
  FromBBI.ExtraCost2 = 0;

  ToBBI.ClobbersPred |= FromBBI.ClobbersPred;
  ToBBI.HasFallThrough = FromBBI.HasFallThrough;
  ToBBI.IsAnalyzed = false;
  FromBBI.IsAnalyzed = false;
}

FunctionPass *
llvm::createIfConverter(std::function<bool(const Function &)> Ftor) {
  return new IfConverter(std::move(Ftor));
}
