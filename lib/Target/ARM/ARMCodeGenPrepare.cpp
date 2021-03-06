//===----- ARMCodeGenPrepare.cpp ------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This pass inserts intrinsics to handle small types that would otherwise be
/// promoted during legalization. Here we can manually promote types or insert
/// intrinsics which can handle narrow types that aren't supported by the
/// register classes.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMSubtarget.h"
#include "ARMTargetMachine.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "arm-codegenprepare"

using namespace llvm;

static cl::opt<bool>
DisableCGP("arm-disable-cgp", cl::Hidden, cl::init(false),
           cl::desc("Disable ARM specific CodeGenPrepare pass"));

static cl::opt<bool>
EnableDSP("arm-enable-scalar-dsp", cl::Hidden, cl::init(false),
          cl::desc("Use DSP instructions for scalar operations"));

static cl::opt<bool>
EnableDSPWithImms("arm-enable-scalar-dsp-imms", cl::Hidden, cl::init(false),
                   cl::desc("Use DSP instructions for scalar operations\
                            with immediate operands"));

// The goal of this pass is to enable more efficient code generation for
// operations on narrow types (i.e. types with < 32-bits) and this is a
// motivating IR code example:
//
//   define hidden i32 @cmp(i8 zeroext) {
//     %2 = add i8 %0, -49
//     %3 = icmp ult i8 %2, 3
//     ..
//   }
//
// The issue here is that i8 is type-legalized to i32 because i8 is not a
// legal type. Thus, arithmetic is done in integer-precision, but then the
// byte value is masked out as follows:
//
//   t19: i32 = add t4, Constant:i32<-49>
//     t24: i32 = and t19, Constant:i32<255>
//
// Consequently, we generate code like this:
//
//   subs  r0, #49
//   uxtb  r1, r0
//   cmp r1, #3
//
// This shows that masking out the byte value results in generation of
// the UXTB instruction. This is not optimal as r0 already contains the byte
// value we need, and so instead we can just generate:
//
//   sub.w r1, r0, #49
//   cmp r1, #3
//
// We achieve this by type promoting the IR to i32 like so for this example:
//
//   define i32 @cmp(i8 zeroext %c) {
//     %0 = zext i8 %c to i32
//     %c.off = add i32 %0, -49
//     %1 = icmp ult i32 %c.off, 3
//     ..
//   }
//
// For this to be valid and legal, we need to prove that the i32 add is
// producing the same value as the i8 addition, and that e.g. no overflow
// happens.
//
// A brief sketch of the algorithm and some terminology.
// We pattern match interesting IR patterns:
// - which have "sources": instructions producing narrow values (i8, i16), and
// - they have "sinks": instructions consuming these narrow values.
//
// We collect all instruction connecting sources and sinks in a worklist, so
// that we can mutate these instruction and perform type promotion when it is
// legal to do so.

namespace {
class IRPromoter {
  SmallPtrSet<Value*, 8> NewInsts;
  SmallVector<Instruction*, 4> InstsToRemove;
  Module *M = nullptr;
  LLVMContext &Ctx;

public:
  IRPromoter(Module *M) : M(M), Ctx(M->getContext()) { }

  void Cleanup() {
    for (auto *I : InstsToRemove) {
      LLVM_DEBUG(dbgs() << "ARM CGP: Removing " << *I << "\n");
      I->dropAllReferences();
      I->eraseFromParent();
    }
    InstsToRemove.clear();
    NewInsts.clear();
  }

  void Mutate(Type *OrigTy,
              SmallPtrSetImpl<Value*> &Visited,
              SmallPtrSetImpl<Value*> &Sources,
              SmallPtrSetImpl<Instruction*> &Sinks);
};

class ARMCodeGenPrepare : public FunctionPass {
  const ARMSubtarget *ST = nullptr;
  IRPromoter *Promoter = nullptr;
  std::set<Value*> AllVisited;

  bool isSupportedValue(Value *V);
  bool isLegalToPromote(Value *V);
  bool TryToPromote(Value *V);

public:
  static char ID;
  static unsigned TypeSize;
  Type *OrigTy = nullptr;

  ARMCodeGenPrepare() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetPassConfig>();
  }

  StringRef getPassName() const override { return "ARM IR optimizations"; }

  bool doInitialization(Module &M) override;
  bool runOnFunction(Function &F) override;
  bool doFinalization(Module &M) override;
};

}

static bool generateSignBits(Value *V) {
  if (!isa<Instruction>(V))
    return false;

  unsigned Opc = cast<Instruction>(V)->getOpcode();
  return Opc == Instruction::AShr || Opc == Instruction::SDiv ||
         Opc == Instruction::SRem;
}

/// Some instructions can use 8- and 16-bit operands, and we don't need to
/// promote anything larger. We disallow booleans to make life easier when
/// dealing with icmps but allow any other integer that is <= 16 bits. Void
/// types are accepted so we can handle switches.
static bool isSupportedType(Value *V) {
  Type *Ty = V->getType();

  // Allow voids and pointers, these won't be promoted.
  if (Ty->isVoidTy() || Ty->isPointerTy())
    return true;

  if (auto *Ld = dyn_cast<LoadInst>(V))
    Ty = cast<PointerType>(Ld->getPointerOperandType())->getElementType();

  const IntegerType *IntTy = dyn_cast<IntegerType>(Ty);
  if (!IntTy)
    return false;

  return IntTy->getBitWidth() == ARMCodeGenPrepare::TypeSize;
}

/// Return true if the given value is a source in the use-def chain, producing
/// a narrow (i8, i16) value. These values will be zext to start the promotion
/// of the tree to i32. We guarantee that these won't populate the upper bits
/// of the register. ZExt on the loads will be free, and the same for call
/// return values because we only accept ones that guarantee a zeroext ret val.
/// Many arguments will have the zeroext attribute too, so those would be free
/// too.
static bool isSource(Value *V) {
  if (!isa<IntegerType>(V->getType()))
    return false;
  // TODO Allow zext to be sources.
  if (isa<Argument>(V))
    return true;
  else if (isa<LoadInst>(V))
    return true;
  else if (isa<BitCastInst>(V))
    return true;
  else if (auto *Call = dyn_cast<CallInst>(V))
    return Call->hasRetAttr(Attribute::AttrKind::ZExt);
  else if (auto *Trunc = dyn_cast<TruncInst>(V))
    return isSupportedType(Trunc);
  return false;
}

/// Return true if V will require any promoted values to be truncated for the
/// the IR to remain valid. We can't mutate the value type of these
/// instructions.
static bool isSink(Value *V) {
  // TODO The truncate also isn't actually necessary because we would already
  // proved that the data value is kept within the range of the original data
  // type.
  auto UsesNarrowValue = [](Value *V) {
    return V->getType()->getScalarSizeInBits() == ARMCodeGenPrepare::TypeSize;
  };

  if (auto *Store = dyn_cast<StoreInst>(V))
    return UsesNarrowValue(Store->getValueOperand());
  if (auto *Return = dyn_cast<ReturnInst>(V))
    return UsesNarrowValue(Return->getReturnValue());
  if (auto *Trunc = dyn_cast<TruncInst>(V))
    return UsesNarrowValue(Trunc->getOperand(0));
  if (auto *ZExt = dyn_cast<ZExtInst>(V))
    return UsesNarrowValue(ZExt->getOperand(0));
  if (auto *ICmp = dyn_cast<ICmpInst>(V))
    return ICmp->isSigned();

  return isa<CallInst>(V);
}

/// Return whether the instruction can be promoted within any modifications to
/// it's operands or result.
static bool isSafeOverflow(Instruction *I) {
  // FIXME Do we need NSW too?
  if (isa<OverflowingBinaryOperator>(I) && I->hasNoUnsignedWrap())
    return true;

  unsigned Opc = I->getOpcode();
  if (Opc == Instruction::Add || Opc == Instruction::Sub) {
    // We don't care if the add or sub could wrap if the value is decreasing
    // and is only being used by an unsigned compare.
    if (!I->hasOneUse() ||
        !isa<ICmpInst>(*I->user_begin()) ||
        !isa<ConstantInt>(I->getOperand(1)))
      return false;

    auto *CI = cast<ICmpInst>(*I->user_begin());
    
    // Don't support an icmp that deals with sign bits, including negative
    // immediates
    if (CI->isSigned())
      return false;

    if (auto *Const = dyn_cast<ConstantInt>(CI->getOperand(0)))
      if (Const->isNegative())
        return false;

    if (auto *Const = dyn_cast<ConstantInt>(CI->getOperand(1)))
      if (Const->isNegative())
        return false;

    bool NegImm = cast<ConstantInt>(I->getOperand(1))->isNegative();
    bool IsDecreasing = ((Opc == Instruction::Sub) && !NegImm) ||
                        ((Opc == Instruction::Add) && NegImm);
    if (!IsDecreasing)
      return false;

    LLVM_DEBUG(dbgs() << "ARM CGP: Allowing safe overflow for " << *I << "\n");
    return true;
  }

  return false;
}

static bool shouldPromote(Value *V) {
  if (!isa<IntegerType>(V->getType()) || isSink(V))
    return false;

  if (isSource(V))
    return true;

  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;

  if (isa<ICmpInst>(I))
    return false;

  return true;
}

/// Return whether we can safely mutate V's type to ExtTy without having to be
/// concerned with zero extending or truncation.
static bool isPromotedResultSafe(Value *V) {
  if (!isa<Instruction>(V))
    return true;

  if (generateSignBits(V))
    return false;

  // If I is only being used by something that will require its value to be
  // truncated, then we don't care about the promoted result.
  auto *I = cast<Instruction>(V);
  if (I->hasOneUse() && isSink(*I->use_begin()))
    return true;

  if (isa<OverflowingBinaryOperator>(I))
    return isSafeOverflow(I);
  return true;
}

/// Return the intrinsic for the instruction that can perform the same
/// operation but on a narrow type. This is using the parallel dsp intrinsics
/// on scalar values.
static Intrinsic::ID getNarrowIntrinsic(Instruction *I) {
  // Whether we use the signed or unsigned versions of these intrinsics
  // doesn't matter because we're not using the GE bits that they set in
  // the APSR.
  switch(I->getOpcode()) {
  default:
    break;
  case Instruction::Add:
    return ARMCodeGenPrepare::TypeSize == 16 ? Intrinsic::arm_uadd16 :
      Intrinsic::arm_uadd8;
  case Instruction::Sub:
    return ARMCodeGenPrepare::TypeSize == 16 ? Intrinsic::arm_usub16 :
      Intrinsic::arm_usub8;
  }
  llvm_unreachable("unhandled opcode for narrow intrinsic");
}

void IRPromoter::Mutate(Type *OrigTy,
                        SmallPtrSetImpl<Value*> &Visited,
                        SmallPtrSetImpl<Value*> &Sources,
                        SmallPtrSetImpl<Instruction*> &Sinks) {
  IRBuilder<> Builder{Ctx};
  Type *ExtTy = Type::getInt32Ty(M->getContext());
  SmallPtrSet<Value*, 8> Promoted;
  LLVM_DEBUG(dbgs() << "ARM CGP: Promoting use-def chains to from "
             << ARMCodeGenPrepare::TypeSize << " to 32-bits\n");

  // Cache original types.
  DenseMap<Value*, Type*> TruncTysMap;
  for (auto *V : Visited)
    TruncTysMap[V] = V->getType();

  auto ReplaceAllUsersOfWith = [&](Value *From, Value *To) {
    SmallVector<Instruction*, 4> Users;
    Instruction *InstTo = dyn_cast<Instruction>(To);
    for (Use &U : From->uses()) {
      auto *User = cast<Instruction>(U.getUser());
      if (InstTo && User->isIdenticalTo(InstTo))
        continue;
      Users.push_back(User);
    }

    for (auto *U : Users)
      U->replaceUsesOfWith(From, To);
  };

  auto FixConst = [&](ConstantInt *Const, Instruction *I) {
    Constant *NewConst = isSafeOverflow(I) && Const->isNegative() ?
      ConstantExpr::getSExt(Const, ExtTy) :
      ConstantExpr::getZExt(Const, ExtTy);
    I->replaceUsesOfWith(Const, NewConst);
  };

  auto InsertDSPIntrinsic = [&](Instruction *I) {
    LLVM_DEBUG(dbgs() << "ARM CGP: Inserting DSP intrinsic for "
               << *I << "\n");
    Function *DSPInst =
      Intrinsic::getDeclaration(M, getNarrowIntrinsic(I));
    Builder.SetInsertPoint(I);
    Builder.SetCurrentDebugLocation(I->getDebugLoc());
    Value *Args[] = { I->getOperand(0), I->getOperand(1) };
    CallInst *Call = Builder.CreateCall(DSPInst, Args);
    ReplaceAllUsersOfWith(I, Call);
    InstsToRemove.push_back(I);
    NewInsts.insert(Call);
    TruncTysMap[Call] = OrigTy;
  };

  auto InsertZExt = [&](Value *V, Instruction *InsertPt) {
    LLVM_DEBUG(dbgs() << "ARM CGP: Inserting ZExt for " << *V << "\n");
    Builder.SetInsertPoint(InsertPt);
    if (auto *I = dyn_cast<Instruction>(V))
      Builder.SetCurrentDebugLocation(I->getDebugLoc());
    auto *ZExt = cast<Instruction>(Builder.CreateZExt(V, ExtTy));
    if (isa<Argument>(V))
      ZExt->moveBefore(InsertPt);
    else
      ZExt->moveAfter(InsertPt);
    ReplaceAllUsersOfWith(V, ZExt);
    NewInsts.insert(ZExt);
    TruncTysMap[ZExt] = TruncTysMap[V];
  };

  // First, insert extending instructions between the sources and their users.
  LLVM_DEBUG(dbgs() << "ARM CGP: Promoting sources:\n");
  for (auto V : Sources) {
    LLVM_DEBUG(dbgs() << " - " << *V << "\n");
    if (auto *I = dyn_cast<Instruction>(V))
      InsertZExt(I, I);
    else if (auto *Arg = dyn_cast<Argument>(V)) {
      BasicBlock &BB = Arg->getParent()->front();
      InsertZExt(Arg, &*BB.getFirstInsertionPt());
    } else {
      llvm_unreachable("unhandled source that needs extending");
    }
    Promoted.insert(V);
  }

  LLVM_DEBUG(dbgs() << "ARM CGP: Mutating the tree..\n");
  // Then mutate the types of the instructions within the tree. Here we handle
  // constant operands.
  for (auto *V : Visited) {
    if (Sources.count(V))
      continue;

    auto *I = cast<Instruction>(V);
    if (Sinks.count(I))
      continue;

    for (unsigned i = 0, e = I->getNumOperands(); i < e; ++i) {
      Value *Op = I->getOperand(i);
      if ((Op->getType() == ExtTy) || !isa<IntegerType>(Op->getType()))
        continue;

      if (auto *Const = dyn_cast<ConstantInt>(Op))
        FixConst(Const, I);
      else if (isa<UndefValue>(Op))
        I->setOperand(i, UndefValue::get(ExtTy));
    }

    if (shouldPromote(I)) {
      I->mutateType(ExtTy);
      Promoted.insert(I);
    }
  }

  // Now we need to remove any zexts that have become unnecessary, as well
  // as insert any intrinsics.
  for (auto *V : Visited) {
    if (Sources.count(V))
      continue;

    if (!shouldPromote(V) || isPromotedResultSafe(V))
      continue;

    // Replace unsafe instructions with appropriate intrinsic calls.
    InsertDSPIntrinsic(cast<Instruction>(V));
  }

  auto InsertTrunc = [&](Value *V) -> Instruction* {
    if (!isa<Instruction>(V) || !isa<IntegerType>(V->getType()))
      return nullptr;

    if ((!Promoted.count(V) && !NewInsts.count(V)) || !TruncTysMap.count(V) ||
        Sources.count(V))
      return nullptr;

    Type *TruncTy = TruncTysMap[V];
    if (TruncTy == ExtTy)
      return nullptr;

    LLVM_DEBUG(dbgs() << "ARM CGP: Creating " << *TruncTy << " Trunc for "
               << *V << "\n");
    Builder.SetInsertPoint(cast<Instruction>(V));
    auto *Trunc = cast<Instruction>(Builder.CreateTrunc(V, TruncTy));
    NewInsts.insert(Trunc);
    return Trunc;
  };

  LLVM_DEBUG(dbgs() << "ARM CGP: Fixing up the sinks:\n");
  // Fix up any stores or returns that use the results of the promoted
  // chain.
  for (auto I : Sinks) {
    LLVM_DEBUG(dbgs() << " - " << *I << "\n");

    // Handle calls separately as we need to iterate over arg operands.
    if (auto *Call = dyn_cast<CallInst>(I)) {
      for (unsigned i = 0; i < Call->getNumArgOperands(); ++i) {
        Value *Arg = Call->getArgOperand(i);
        if (Instruction *Trunc = InsertTrunc(Arg)) {
          Trunc->moveBefore(Call);
          Call->setArgOperand(i, Trunc);
        }
      }
      continue;
    }

    // Now handle the others.
    for (unsigned i = 0; i < I->getNumOperands(); ++i) {
      if (Instruction *Trunc = InsertTrunc(I->getOperand(i))) {
        Trunc->moveBefore(I);
        I->setOperand(i, Trunc);
      }
    }
  }
  LLVM_DEBUG(dbgs() << "ARM CGP: Mutation complete:\n");
  LLVM_DEBUG(dbgs();
             for (auto *V : Sources)
               V->dump();
             for (auto *I : NewInsts)
               I->dump();
             for (auto *V : Visited) {
               if (!Sources.count(V))
                 V->dump();
              });
}

/// We accept most instructions, as well as Arguments and ConstantInsts. We
/// Disallow casts other than zext and truncs and only allow calls if their
/// return value is zeroext. We don't allow opcodes that can introduce sign
/// bits.
bool ARMCodeGenPrepare::isSupportedValue(Value *V) {
  if (isa<ICmpInst>(V))
    return true;

  // Memory instructions
  if (isa<StoreInst>(V) || isa<GetElementPtrInst>(V))
    return true;

  // Branches and targets.
  if( isa<BranchInst>(V) || isa<SwitchInst>(V) || isa<BasicBlock>(V))
    return true;

  // Non-instruction values that we can handle.
  if ((isa<Constant>(V) && !isa<ConstantExpr>(V)) || isa<Argument>(V))
    return isSupportedType(V);

  if (isa<PHINode>(V) || isa<SelectInst>(V) || isa<ReturnInst>(V) ||
      isa<LoadInst>(V))
    return isSupportedType(V);

  // Truncs can be either sources or sinks.
  if (auto *Trunc = dyn_cast<TruncInst>(V))
    return isSupportedType(Trunc) || isSupportedType(Trunc->getOperand(0));

  if (isa<CastInst>(V) && !isa<SExtInst>(V))
    return isSupportedType(cast<CastInst>(V)->getOperand(0));

  // Special cases for calls as we need to check for zeroext
  // TODO We should accept calls even if they don't have zeroext, as they can
  // still be sinks.
  if (auto *Call = dyn_cast<CallInst>(V))
    return isSupportedType(Call) &&
           Call->hasRetAttr(Attribute::AttrKind::ZExt);

  if (!isa<BinaryOperator>(V))
    return false;

  if (!isSupportedType(V))
    return false;

  if (generateSignBits(V)) {
    LLVM_DEBUG(dbgs() << "ARM CGP: No, instruction can generate sign bits.\n");
    return false;
  }
  return true;
}

/// Check that the type of V would be promoted and that the original type is
/// smaller than the targeted promoted type. Check that we're not trying to
/// promote something larger than our base 'TypeSize' type.
bool ARMCodeGenPrepare::isLegalToPromote(Value *V) {
  if (isPromotedResultSafe(V))
    return true;

  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;

  // If promotion is not safe, can we use a DSP instruction to natively
  // handle the narrow type?
  if (!ST->hasDSP() || !EnableDSP || !isSupportedType(I))
    return false;

  if (ST->isThumb() && !ST->hasThumb2())
    return false;

  if (I->getOpcode() != Instruction::Add && I->getOpcode() != Instruction::Sub)
    return false;

  // TODO
  // Would it be profitable? For Thumb code, these parallel DSP instructions
  // are only Thumb-2, so we wouldn't be able to dual issue on Cortex-M33. For
  // Cortex-A, specifically Cortex-A72, the latency is double and throughput is
  // halved. They also do not take immediates as operands.
  for (auto &Op : I->operands()) {
    if (isa<Constant>(Op)) {
      if (!EnableDSPWithImms)
        return false;
    }
  }
  return true;
}

bool ARMCodeGenPrepare::TryToPromote(Value *V) {
  OrigTy = V->getType();
  TypeSize = OrigTy->getPrimitiveSizeInBits();
  if (TypeSize > 16 || TypeSize < 8)
    return false;

  if (!isSupportedValue(V) || !shouldPromote(V) || !isLegalToPromote(V))
    return false;

  LLVM_DEBUG(dbgs() << "ARM CGP: TryToPromote: " << *V << ", TypeSize = "
             << TypeSize << "\n");

  SetVector<Value*> WorkList;
  SmallPtrSet<Value*, 8> Sources;
  SmallPtrSet<Instruction*, 4> Sinks;
  WorkList.insert(V);
  SmallPtrSet<Value*, 16> CurrentVisited;
  CurrentVisited.clear();

  // Return true if V was added to the worklist as a supported instruction,
  // if it was already visited, or if we don't need to explore it (e.g.
  // pointer values and GEPs), and false otherwise.
  auto AddLegalInst = [&](Value *V) {
    if (CurrentVisited.count(V))
      return true;

    // Ignore GEPs because they don't need promoting and the constant indices
    // will prevent the transformation.
    if (isa<GetElementPtrInst>(V))
      return true;

    if (!isSupportedValue(V) || (shouldPromote(V) && !isLegalToPromote(V))) {
      LLVM_DEBUG(dbgs() << "ARM CGP: Can't handle: " << *V << "\n");
      return false;
    }

    WorkList.insert(V);
    return true;
  };

  // Iterate through, and add to, a tree of operands and users in the use-def.
  while (!WorkList.empty()) {
    Value *V = WorkList.back();
    WorkList.pop_back();
    if (CurrentVisited.count(V))
      continue;

    // Ignore non-instructions, other than arguments.
    if (!isa<Instruction>(V) && !isSource(V))
      continue;

    // If we've already visited this value from somewhere, bail now because
    // the tree has already been explored.
    // TODO: This could limit the transform, ie if we try to promote something
    // from an i8 and fail first, before trying an i16.
    if (AllVisited.count(V))
      return false;

    CurrentVisited.insert(V);
    AllVisited.insert(V);

    // Calls can be both sources and sinks.
    if (isSink(V))
      Sinks.insert(cast<Instruction>(V));
    if (isSource(V))
      Sources.insert(V);
    else if (auto *I = dyn_cast<Instruction>(V)) {
      // Visit operands of any instruction visited.
      for (auto &U : I->operands()) {
        if (!AddLegalInst(U))
          return false;
      }
    }

    // Don't visit users of a node which isn't going to be mutated unless its a
    // source.
    if (isSource(V) || shouldPromote(V)) {
      for (Use &U : V->uses()) {
        if (!AddLegalInst(U.getUser()))
          return false;
      }
    }
  }

  LLVM_DEBUG(dbgs() << "ARM CGP: Visited nodes:\n";
             for (auto *I : CurrentVisited)
               I->dump();
             );
  unsigned ToPromote = 0;
  for (auto *V : CurrentVisited) {
    if (Sources.count(V))
      continue;
    if (Sinks.count(cast<Instruction>(V)))
      continue;
    ++ToPromote;
  }

  if (ToPromote < 2)
    return false;

  Promoter->Mutate(OrigTy, CurrentVisited, Sources, Sinks);
  return true;
}

bool ARMCodeGenPrepare::doInitialization(Module &M) {
  Promoter = new IRPromoter(&M);
  return false;
}

bool ARMCodeGenPrepare::runOnFunction(Function &F) {
  if (skipFunction(F) || DisableCGP)
    return false;

  auto *TPC = &getAnalysis<TargetPassConfig>();
  if (!TPC)
    return false;

  const TargetMachine &TM = TPC->getTM<TargetMachine>();
  ST = &TM.getSubtarget<ARMSubtarget>(F);
  bool MadeChange = false;
  LLVM_DEBUG(dbgs() << "ARM CGP: Running on " << F.getName() << "\n");

  // Search up from icmps to try to promote their operands.
  for (BasicBlock &BB : F) {
    auto &Insts = BB.getInstList();
    for (auto &I : Insts) {
      if (AllVisited.count(&I))
        continue;

      if (isa<ICmpInst>(I)) {
        auto &CI = cast<ICmpInst>(I);

        // Skip signed or pointer compares
        if (CI.isSigned() || !isa<IntegerType>(CI.getOperand(0)->getType()))
          continue;

        for (auto &Op : CI.operands()) {
          if (auto *I = dyn_cast<Instruction>(Op))
            MadeChange |= TryToPromote(I);
        }
      }
    }
    Promoter->Cleanup();
    LLVM_DEBUG(if (verifyFunction(F, &dbgs())) {
                dbgs();
                report_fatal_error("Broken function after type promotion");
               });
  }
  if (MadeChange)
    LLVM_DEBUG(dbgs() << "After ARMCodeGenPrepare: " << F << "\n");

  return MadeChange;
}

bool ARMCodeGenPrepare::doFinalization(Module &M) {
  delete Promoter;
  return false;
}

INITIALIZE_PASS_BEGIN(ARMCodeGenPrepare, DEBUG_TYPE,
                      "ARM IR optimizations", false, false)
INITIALIZE_PASS_END(ARMCodeGenPrepare, DEBUG_TYPE, "ARM IR optimizations",
                    false, false)

char ARMCodeGenPrepare::ID = 0;
unsigned ARMCodeGenPrepare::TypeSize = 0;

FunctionPass *llvm::createARMCodeGenPreparePass() {
  return new ARMCodeGenPrepare();
}
