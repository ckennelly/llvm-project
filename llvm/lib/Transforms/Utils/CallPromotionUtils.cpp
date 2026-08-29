//===- CallPromotionUtils.cpp - Utilities for call promotion ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements utilities useful for promoting indirect call sites to
// direct call sites.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/CallPromotionUtils.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/CtxProfAnalysis.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/TypeMetadataUtils.h"
#include "llvm/IR/AttributeMask.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/ProfileData/PGOCtxProfReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;

#define DEBUG_TYPE "call-promotion-utils"

static cl::opt<bool> HoistPromotionCondAboveTypeTest(
    "icp-hoist-cond-above-type-test", cl::init(true), cl::Hidden,
    cl::desc("When promoting an indirect call whose callee is guarded by a "
             "CFI type test, evaluate the promotion condition before the "
             "type test so that the direct call does not pay for the check"));

/// Fix-up phi nodes in an invoke instruction's normal destination.
///
/// After versioning an invoke instruction, values coming from the original
/// block will now be coming from the "merge" block. For example, in the code
/// below:
///
///   then_bb:
///     %t0 = invoke i32 %ptr() to label %merge_bb unwind label %unwind_dst
///
///   else_bb:
///     %t1 = invoke i32 %ptr() to label %merge_bb unwind label %unwind_dst
///
///   merge_bb:
///     %t2 = phi i32 [ %t0, %then_bb ], [ %t1, %else_bb ]
///     br %normal_dst
///
///   normal_dst:
///     %t3 = phi i32 [ %x, %orig_bb ], ...
///
/// "orig_bb" is no longer a predecessor of "normal_dst", so the phi nodes in
/// "normal_dst" must be fixed to refer to "merge_bb":
///
///    normal_dst:
///      %t3 = phi i32 [ %x, %merge_bb ], ...
///
static void fixupPHINodeForNormalDest(InvokeInst *Invoke, BasicBlock *OrigBlock,
                                      BasicBlock *MergeBlock) {
  for (PHINode &Phi : Invoke->getNormalDest()->phis()) {
    int Idx = Phi.getBasicBlockIndex(OrigBlock);
    if (Idx == -1)
      continue;
    Phi.setIncomingBlock(Idx, MergeBlock);
  }
}

/// Fix-up phi nodes in an invoke instruction's unwind destination.
///
/// After versioning an invoke instruction, values coming from the original
/// block will now be coming from either the "then" block or the "else" block.
/// For example, in the code below:
///
///   then_bb:
///     %t0 = invoke i32 %ptr() to label %merge_bb unwind label %unwind_dst
///
///   else_bb:
///     %t1 = invoke i32 %ptr() to label %merge_bb unwind label %unwind_dst
///
///   unwind_dst:
///     %t3 = phi i32 [ %x, %orig_bb ], ...
///
/// "orig_bb" is no longer a predecessor of "unwind_dst", so the phi nodes in
/// "unwind_dst" must be fixed to refer to "then_bb" and "else_bb":
///
///   unwind_dst:
///     %t3 = phi i32 [ %x, %then_bb ], [ %x, %else_bb ], ...
///
static void fixupPHINodeForUnwindDest(InvokeInst *Invoke, BasicBlock *OrigBlock,
                                      BasicBlock *ThenBlock,
                                      BasicBlock *ElseBlock) {
  for (PHINode &Phi : Invoke->getUnwindDest()->phis()) {
    int Idx = Phi.getBasicBlockIndex(OrigBlock);
    if (Idx == -1)
      continue;
    auto *V = Phi.getIncomingValue(Idx);
    Phi.setIncomingBlock(Idx, ThenBlock);
    Phi.addIncoming(V, ElseBlock);
  }
}

/// Create a phi node for the returned value of a call or invoke instruction.
///
/// After versioning a call or invoke instruction that returns a value, we have
/// to merge the value of the original and new instructions. We do this by
/// creating a phi node and replacing uses of the original instruction with this
/// phi node.
///
/// For example, if \p OrigInst is defined in "else_bb" and \p NewInst is
/// defined in "then_bb", we create the following phi node:
///
///   ; Uses of the original instruction are replaced by uses of the phi node.
///   %t0 = phi i32 [ %orig_inst, %else_bb ], [ %new_inst, %then_bb ],
///
static void createRetPHINode(Instruction *OrigInst, Instruction *NewInst,
                             BasicBlock *MergeBlock, IRBuilder<> &Builder) {

  if (OrigInst->getType()->isVoidTy() || OrigInst->use_empty())
    return;

  Builder.SetInsertPoint(MergeBlock, MergeBlock->begin());
  PHINode *Phi = Builder.CreatePHI(OrigInst->getType(), 0);
  SmallVector<User *, 16> UsersToUpdate(OrigInst->users());
  for (User *U : UsersToUpdate)
    U->replaceUsesOfWith(OrigInst, Phi);
  Phi->addIncoming(OrigInst, OrigInst->getParent());
  Phi->addIncoming(NewInst, NewInst->getParent());
}

/// Cast a call or invoke instruction to the given type.
///
/// When promoting a call site, the return type of the call site might not match
/// that of the callee. If this is the case, we have to cast the returned value
/// to the correct type. The location of the cast depends on if we have a call
/// or invoke instruction.
///
/// For example, if the call instruction below requires a bitcast after
/// promotion:
///
///   orig_bb:
///     %t0 = call i32 @func()
///     ...
///
/// The bitcast is placed after the call instruction:
///
///   orig_bb:
///     ; Uses of the original return value are replaced by uses of the bitcast.
///     %t0 = call i32 @func()
///     %t1 = bitcast i32 %t0 to ...
///     ...
///
/// A similar transformation is performed for invoke instructions. However,
/// since invokes are terminating, a new block is created for the bitcast. For
/// example, if the invoke instruction below requires a bitcast after promotion:
///
///   orig_bb:
///     %t0 = invoke i32 @func() to label %normal_dst unwind label %unwind_dst
///
/// The edge between the original block and the invoke's normal destination is
/// split, and the bitcast is placed there:
///
///   orig_bb:
///     %t0 = invoke i32 @func() to label %split_bb unwind label %unwind_dst
///
///   split_bb:
///     ; Uses of the original return value are replaced by uses of the bitcast.
///     %t1 = bitcast i32 %t0 to ...
///     br label %normal_dst
///
static void createRetBitCast(CallBase &CB, Type *RetTy, CastInst **RetBitCast) {

  // Save the users of the calling instruction. These uses will be changed to
  // use the bitcast after we create it.
  SmallVector<User *, 16> UsersToUpdate(CB.users());

  // Determine an appropriate location to create the bitcast for the return
  // value. The location depends on if we have a call or invoke instruction.
  BasicBlock::iterator InsertBefore;
  if (auto *Invoke = dyn_cast<InvokeInst>(&CB))
    InsertBefore =
        SplitEdge(Invoke->getParent(), Invoke->getNormalDest())->begin();
  else
    InsertBefore = std::next(CB.getIterator());

  // Bitcast the return value to the correct type.
  auto *Cast = CastInst::CreateBitOrPointerCast(&CB, RetTy, "", InsertBefore);
  if (RetBitCast)
    *RetBitCast = Cast;

  // Replace all the original uses of the calling instruction with the bitcast.
  for (User *U : UsersToUpdate)
    U->replaceUsesOfWith(&CB, Cast);
}

/// Predicate and clone the given call site.
///
/// This function creates an if-then-else structure at the location of the call
/// site. The "if" condition is specified by `Cond`.
/// The original call site is moved into the "else" block, and a clone of the
/// call site is placed in the "then" block. The cloned instruction is returned.
///
/// For example, the call instruction below:
///
///   orig_bb:
///     %t0 = call i32 %ptr()
///     ...
///
/// Is replace by the following:
///
///   orig_bb:
///     %cond = Cond
///     br i1 %cond, %then_bb, %else_bb
///
///   then_bb:
///     ; The clone of the original call instruction is placed in the "then"
///     ; block. It is not yet promoted.
///     %t1 = call i32 %ptr()
///     br merge_bb
///
///   else_bb:
///     ; The original call instruction is moved to the "else" block.
///     %t0 = call i32 %ptr()
///     br merge_bb
///
///   merge_bb:
///     ; Uses of the original call instruction are replaced by uses of the phi
///     ; node.
///     %t2 = phi i32 [ %t0, %else_bb ], [ %t1, %then_bb ]
///     ...
///
/// A similar transformation is performed for invoke instructions. However,
/// since invokes are terminating, more work is required. For example, the
/// invoke instruction below:
///
///   orig_bb:
///     %t0 = invoke %ptr() to label %normal_dst unwind label %unwind_dst
///
/// Is replace by the following:
///
///   orig_bb:
///     %cond = Cond
///     br i1 %cond, %then_bb, %else_bb
///
///   then_bb:
///     ; The clone of the original invoke instruction is placed in the "then"
///     ; block, and its normal destination is set to the "merge" block. It is
///     ; not yet promoted.
///     %t1 = invoke i32 %ptr() to label %merge_bb unwind label %unwind_dst
///
///   else_bb:
///     ; The original invoke instruction is moved into the "else" block, and
///     ; its normal destination is set to the "merge" block.
///     %t0 = invoke i32 %ptr() to label %merge_bb unwind label %unwind_dst
///
///   merge_bb:
///     ; Uses of the original invoke instruction are replaced by uses of the
///     ; phi node, and the merge block branches to the normal destination.
///     %t2 = phi i32 [ %t0, %else_bb ], [ %t1, %then_bb ]
///     br %normal_dst
///
/// An indirect musttail call is processed slightly differently in that:
/// 1. No merge block needed for the orginal and the cloned callsite, since
///    either one ends the flow. No phi node is needed either.
/// 2. The return statement following the original call site is duplicated too
///    and placed immediately after the cloned call site per the IR convention.
///
/// For example, the musttail call instruction below:
///
///   orig_bb:
///     %t0 = musttail call i32 %ptr()
///     ...
///
/// Is replaced by the following:
///
///   cond_bb:
///     %cond = Cond
///     br i1 %cond, %then_bb, %orig_bb
///
///   then_bb:
///     ; The clone of the original call instruction is placed in the "then"
///     ; block. It is not yet promoted.
///     %t1 = musttail call i32 %ptr()
///     ret %t1
///
///   orig_bb:
///     ; The original call instruction stays in its original block.
///     %t0 = musttail call i32 %ptr()
///     ret %t0
static CallBase &versionCallSiteWithCond(CallBase &CB, Value *Cond,
                                         MDNode *BranchWeights) {

  IRBuilder<> Builder(&CB);
  CallBase *OrigInst = &CB;
  BasicBlock *OrigBlock = OrigInst->getParent();

  if (OrigInst->isMustTailCall()) {
    // Create an if-then structure. The original instruction stays in its block,
    // and a clone of the original instruction is placed in the "then" block.
    Instruction *ThenTerm =
        SplitBlockAndInsertIfThen(Cond, &CB, false, BranchWeights);
    BasicBlock *ThenBlock = ThenTerm->getParent();
    ThenBlock->setName("if.true.direct_targ");
    CallBase *NewInst = cast<CallBase>(OrigInst->clone());
    NewInst->insertBefore(ThenTerm->getIterator());

    // Place a clone of the optional bitcast after the new call site.
    Value *NewRetVal = NewInst;
    auto Next = OrigInst->getNextNode();
    if (auto *BitCast = dyn_cast_or_null<BitCastInst>(Next)) {
      assert(BitCast->getOperand(0) == OrigInst &&
             "bitcast following musttail call must use the call");
      auto NewBitCast = BitCast->clone();
      NewBitCast->replaceUsesOfWith(OrigInst, NewInst);
      NewBitCast->insertBefore(ThenTerm->getIterator());
      NewRetVal = NewBitCast;
      Next = BitCast->getNextNode();
    }

    // Place a clone of the return instruction after the new call site.
    ReturnInst *Ret = dyn_cast_or_null<ReturnInst>(Next);
    assert(Ret && "musttail call must precede a ret with an optional bitcast");
    auto NewRet = Ret->clone();
    if (Ret->getReturnValue())
      NewRet->replaceUsesOfWith(Ret->getReturnValue(), NewRetVal);
    NewRet->insertBefore(ThenTerm->getIterator());

    // A return instructions is terminating, so we don't need the terminator
    // instruction just created.
    ThenTerm->eraseFromParent();

    return *NewInst;
  }

  // Create an if-then-else structure. The original instruction is moved into
  // the "else" block, and a clone of the original instruction is placed in the
  // "then" block.
  Instruction *ThenTerm = nullptr;
  Instruction *ElseTerm = nullptr;
  SplitBlockAndInsertIfThenElse(Cond, &CB, &ThenTerm, &ElseTerm, BranchWeights);
  BasicBlock *ThenBlock = ThenTerm->getParent();
  BasicBlock *ElseBlock = ElseTerm->getParent();
  BasicBlock *MergeBlock = OrigInst->getParent();

  ThenBlock->setName("if.true.direct_targ");
  ElseBlock->setName("if.false.orig_indirect");
  MergeBlock->setName("if.end.icp");

  CallBase *NewInst = cast<CallBase>(OrigInst->clone());
  OrigInst->moveBefore(ElseTerm->getIterator());
  NewInst->insertBefore(ThenTerm->getIterator());

  // If the original call site is an invoke instruction, we have extra work to
  // do since invoke instructions are terminating. We have to fix-up phi nodes
  // in the invoke's normal and unwind destinations.
  if (auto *OrigInvoke = dyn_cast<InvokeInst>(OrigInst)) {
    auto *NewInvoke = cast<InvokeInst>(NewInst);

    // Invoke instructions are terminating, so we don't need the terminator
    // instructions that were just created.
    ThenTerm->eraseFromParent();
    ElseTerm->eraseFromParent();

    // Branch from the "merge" block to the original normal destination.
    Builder.SetInsertPoint(MergeBlock);
    Builder.CreateBr(OrigInvoke->getNormalDest());

    // Fix-up phi nodes in the original invoke's normal and unwind destinations.
    fixupPHINodeForNormalDest(OrigInvoke, OrigBlock, MergeBlock);
    fixupPHINodeForUnwindDest(OrigInvoke, MergeBlock, ThenBlock, ElseBlock);

    // Now set the normal destinations of the invoke instructions to be the
    // "merge" block.
    OrigInvoke->setNormalDest(MergeBlock);
    NewInvoke->setNormalDest(MergeBlock);
  }

  // Create a phi node for the returned value of the call site.
  createRetPHINode(OrigInst, NewInst, MergeBlock, Builder);

  return *NewInst;
}

/// A CFI type test that guards a call site: the block of the call site is
/// reached from the true edge of a branch on the type test, possibly through
/// blocks that just fall through to the next one (as left behind by earlier
/// promotions of the same call site).
struct TypeTestGuard {
  CallInst *TypeTest = nullptr;
  CondBrInst *Br = nullptr;
  /// The blocks from the true successor of the branch to the block of the call
  /// site, each the unique predecessor of the next.
  SmallVector<BasicBlock *, 2> Blocks;
};

/// Returns the type test on \p Ptr guarding entry into \p BB, if any. This is
/// the shape that -fsanitize=cfi-icall and -fsanitize=cfi-vcall emit before a
/// call.
static std::optional<TypeTestGuard> findTypeTestGuard(BasicBlock *BB,
                                                      Value *Ptr) {
  TypeTestGuard Guard;
  Guard.Blocks.push_back(BB);
  for (unsigned Depth = 0; Depth < 4; ++Depth) {
    BasicBlock *First = Guard.Blocks.front();
    BasicBlock *Pred = First->getUniquePredecessor();
    if (!Pred)
      return std::nullopt;
    if (isa<UncondBrInst>(Pred->getTerminator())) {
      Guard.Blocks.insert(Guard.Blocks.begin(), Pred);
      continue;
    }
    auto *Br = dyn_cast<CondBrInst>(Pred->getTerminator());
    if (!Br || Br->getSuccessor(0) != First || Br->getSuccessor(1) == First)
      return std::nullopt;
    auto *TypeTest = dyn_cast<CallInst>(Br->getCondition());
    if (!TypeTest || TypeTest->getIntrinsicID() != Intrinsic::type_test ||
        TypeTest->getArgOperand(0)->stripPointerCasts() !=
            Ptr->stripPointerCasts())
      return std::nullopt;
    Guard.TypeTest = TypeTest;
    Guard.Br = Br;
    return Guard;
  }
  return std::nullopt;
}

/// Returns true if \p C points into a global that carries the type identifier
/// tested by \p TypeTest at that offset, i.e. the type test is known to
/// succeed when the tested pointer is equal to \p C.
static bool passesTypeTest(Constant *C, const CallInst *TypeTest,
                           const DataLayout &DL) {
  Metadata *TypeId =
      cast<MetadataAsValue>(TypeTest->getArgOperand(1))->getMetadata();
  APInt Offset(DL.getIndexTypeSizeInBits(C->getType()), 0);
  auto *GO = dyn_cast<GlobalObject>(C->stripAndAccumulateConstantOffsets(
      DL, Offset, /*AllowNonInbounds=*/true));
  if (!GO)
    return false;
  SmallVector<MDNode *, 2> Types;
  GO->getMetadata(LLVMContext::MD_type, Types);
  for (MDNode *Type : Types) {
    if (Type->getOperand(1) != TypeId)
      continue;
    auto *TypeOffset = mdconst::dyn_extract<ConstantInt>(Type->getOperand(0));
    if (TypeOffset && Offset == TypeOffset->getZExtValue())
      return true;
  }
  return false;
}

/// Returns true if the promotion condition for \p CB can be evaluated before
/// the type test \p Guard. The guarded blocks must contain nothing before
/// \p CB except pseudo probes, which stay with the indirect call, and
/// instructions without side effects, which are cloned onto the direct path
/// when it uses them.
static bool canHoistPromotionCond(const CallBase &CB,
                                  const TypeTestGuard &Guard) {
  if (!isa<CallInst>(CB) || CB.isMustTailCall())
    return false;
  for (const BasicBlock *BB : Guard.Blocks) {
    for (const Instruction &I : *BB) {
      if (&I == &CB)
        return true;
      if (isa<PHINode>(I))
        return false;
      if (I.isTerminator() || isa<PseudoProbeInst>(I))
        continue;
      if (I.mayHaveSideEffects())
        return false;
    }
  }
  llvm_unreachable("call site is not in the guarded blocks");
}

/// \p DirectCall is the promoted copy of the original call site, which was
/// guarded by \p Guard. Rewrite the CFG so that
/// the promotion condition is evaluated before the type test and the direct
/// call is reached without it:
///
///   pred:                                pred:
///     %t = type.test(%p, T)                %c = icmp eq %p, @f
///     br %t, cont, trap                    br %c, direct, guard
///   cont:                     ==>        guard:
///     %c = icmp eq %p, @f                  %t = type.test(%p, T)
///     br %c, direct, indirect              br %t, cont, trap
///   direct: call @f; br merge            cont: br indirect
///   indirect: call %p; br merge          direct: call @f; br merge
///   merge: ...                           indirect: call %p; br merge
///                                        merge: ...
///
/// This is valid because the condition implies that the type test succeeds
/// (see passesTypeTest). Instructions of the guarded blocks that precede the
/// call are cloned into the direct block, and values defined there that are
/// used after the merge get a phi node.
static void hoistPromotionCondAboveTypeTest(const TypeTestGuard &Guard,
                                            CallBase &DirectCall,
                                            MDNode *BranchWeights) {
  BasicBlock *ThenBlock = DirectCall.getParent();
  BasicBlock *CondBlock = Guard.Blocks.back();
  assert(ThenBlock->getSinglePredecessor() == CondBlock &&
         "unexpected shape of the versioned call site");
  auto *CondBr = cast<CondBrInst>(CondBlock->getTerminator());
  assert(CondBr->getSuccessor(0) == ThenBlock &&
         "unexpected shape of the versioned call site");
  BasicBlock *ElseBlock = CondBr->getSuccessor(1);
  BasicBlock *MergeBlock = ElseBlock->getSingleSuccessor();
  Value *Cond = CondBr->getCondition();
  CondBrInst *GuardBr = Guard.Br;
  BasicBlock *Pred = GuardBr->getParent();
  SmallPtrSet<const BasicBlock *, 4> GuardedBlocks;
  for (BasicBlock *BB : Guard.Blocks)
    GuardedBlocks.insert(BB);

  // The condition must only depend on values available in the predecessor.
  SmallPtrSet<Instruction *, 4> CondInsts;
  SmallVector<Value *, 4> Worklist{Cond};
  while (!Worklist.empty()) {
    auto *I = dyn_cast<Instruction>(Worklist.pop_back_val());
    if (!I || I->getParent() != CondBlock || !CondInsts.insert(I).second)
      continue;
    append_range(Worklist, I->operands());
  }
  for (Instruction *I : CondInsts)
    for (Value *Op : I->operands())
      if (auto *OpI = dyn_cast<Instruction>(Op))
        if (GuardedBlocks.count(OpI->getParent()) && !CondInsts.count(OpI))
          return;

  // Split the guard into its own block. Take the type test along if it is
  // right before the branch, so that it is only evaluated on the indirect
  // path.
  Instruction *SplitPt = GuardBr;
  if (Guard.TypeTest->getParent() == Pred &&
      Guard.TypeTest->getNextNode() == GuardBr)
    SplitPt = Guard.TypeTest;
  BasicBlock *GuardBlock =
      Pred->splitBasicBlock(SplitPt->getIterator(), "if.type_test");

  // Evaluate the condition in the predecessor and branch to the direct call
  // when it holds, otherwise to the guard.
  Instruction *PredTerm = Pred->getTerminator();
  for (Instruction &I : llvm::make_early_inc_range(*CondBlock))
    if (CondInsts.count(&I))
      I.moveBefore(PredTerm->getIterator());
  CondBrInst *NewBr =
      CondBrInst::Create(Cond, ThenBlock, GuardBlock, PredTerm->getIterator());
  NewBr->setDebugLoc(CondBr->getDebugLoc());
  if (BranchWeights)
    NewBr->setMetadata(LLVMContext::MD_prof, BranchWeights);
  PredTerm->eraseFromParent();
  ThenBlock->replacePhiUsesWith(CondBlock, Pred);

  // The condition is now known to be false whenever the original block is
  // reached.
  UncondBrInst::Create(ElseBlock, CondBr->getIterator());
  CondBr->eraseFromParent();

  // Clone the remaining instructions of the guarded blocks into the direct
  // block, where the direct call may use them. Clones that end up unused (for
  // instance the load of the function pointer of a promoted virtual call) are
  // removed again below.
  ValueToValueMapTy VMap;
  SmallVector<Instruction *, 4> Clones;
  BasicBlock::iterator InsertPt = ThenBlock->begin();
  for (BasicBlock *BB : Guard.Blocks) {
    for (Instruction &I : *BB) {
      if (I.isTerminator() || isa<PseudoProbeInst>(I))
        continue;
      Instruction *Clone = I.clone();
      Clone->setName(I.getName());
      Clone->insertBefore(InsertPt);
      VMap[&I] = Clone;
      Clones.push_back(Clone);
    }
  }
  for (Instruction &I : *ThenBlock)
    RemapInstruction(&I, VMap,
                     RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);

  // The guarded blocks no longer dominate the merge block. Uses of their
  // values reached through the direct path need the clone, and uses after the
  // merge need a phi node.
  for (BasicBlock *BB : Guard.Blocks) {
    for (Instruction &I : *BB) {
      if (I.isTerminator() || isa<PseudoProbeInst>(I))
        continue;
      PHINode *PN = nullptr;
      for (Use &U : llvm::make_early_inc_range(I.uses())) {
        auto *User = cast<Instruction>(U.getUser());
        BasicBlock *UserBB = User->getParent();
        if (auto *UserPN = dyn_cast<PHINode>(User))
          UserBB = UserPN->getIncomingBlock(U);
        if (GuardedBlocks.count(UserBB) || UserBB == ElseBlock)
          continue;
        if (UserBB == ThenBlock) {
          U.set(VMap[&I]);
          continue;
        }
        assert(MergeBlock && "use of a value defined before the call site "
                             "that is not dominated by the call site's block");
        if (!PN) {
          PN = PHINode::Create(I.getType(), 2, I.getName() + ".icp",
                               MergeBlock->begin());
          PN->addIncoming(&I, ElseBlock);
          PN->addIncoming(cast<Instruction>(VMap[&I]), ThenBlock);
        }
        U.set(PN);
      }
    }
  }

  // The clones have no side effects, so drop the ones nothing ended up using.
  for (Instruction *Clone : reverse(Clones))
    if (Clone->use_empty())
      Clone->eraseFromParent();
}

// Predicate and clone the given call site using condition `CB.callee ==
// Callee`. See the comment `versionCallSiteWithCond` for the transformation.
CallBase &llvm::versionCallSite(CallBase &CB, Value *Callee,
                                MDNode *BranchWeights) {

  IRBuilder<> Builder(&CB);

  // Create the compare. The called value and callee must have the same type to
  // be compared.
  if (CB.getCalledOperand()->getType() != Callee->getType())
    Callee = Builder.CreateBitCast(Callee, CB.getCalledOperand()->getType());
  auto *Cond = Builder.CreateICmpEQ(CB.getCalledOperand(), Callee);

  // The address comparison has made the callee's address significant.
  // Strip unnamed_addr so the symbol is recorded as address-significant
  // and kept unique by the linker.
  if (auto *GV = dyn_cast<GlobalValue>(Callee->stripPointerCasts()))
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::None);

  return versionCallSiteWithCond(CB, Cond, BranchWeights);
}

bool llvm::isLegalToPromote(const CallBase &CB, Function *Callee,
                            const char **FailureReason) {
  assert(!CB.getCalledFunction() && "Only indirect call sites can be promoted");

  auto &DL = Callee->getDataLayout();

  // Check the return type. The callee's return value type must be bitcast
  // compatible with the call site's type.
  Type *CallRetTy = CB.getType();
  Type *FuncRetTy = Callee->getReturnType();
  if (CallRetTy != FuncRetTy)
    if (!CastInst::isBitOrNoopPointerCastable(FuncRetTy, CallRetTy, DL)) {
      if (FailureReason)
        *FailureReason = "Return type mismatch";
      return false;
    }

  // The number of formal arguments of the callee.
  unsigned NumParams = Callee->getFunctionType()->getNumParams();

  // The number of actual arguments in the call.
  unsigned NumArgs = CB.arg_size();

  // Check the number of arguments. The callee and call site must agree on the
  // number of arguments.
  if (NumArgs != NumParams && !Callee->isVarArg()) {
    if (FailureReason)
      *FailureReason = "The number of arguments mismatch";
    return false;
  }

  // Check the argument types. The callee's formal argument types must be
  // bitcast compatible with the corresponding actual argument types of the call
  // site.
  unsigned I = 0;
  for (; I < NumParams; ++I) {
    // Make sure that the callee and call agree on byval/inalloca. The types do
    // not have to match.
    if (Callee->hasParamAttribute(I, Attribute::ByVal) !=
        CB.getAttributes().hasParamAttr(I, Attribute::ByVal)) {
      if (FailureReason)
        *FailureReason = "byval mismatch";
      return false;
    }
    if (Callee->hasParamAttribute(I, Attribute::InAlloca) !=
        CB.getAttributes().hasParamAttr(I, Attribute::InAlloca)) {
      if (FailureReason)
        *FailureReason = "inalloca mismatch";
      return false;
    }

    Type *FormalTy = Callee->getFunctionType()->getFunctionParamType(I);
    Type *ActualTy = CB.getArgOperand(I)->getType();
    if (FormalTy == ActualTy)
      continue;
    if (!CastInst::isBitOrNoopPointerCastable(ActualTy, FormalTy, DL)) {
      if (FailureReason)
        *FailureReason = "Argument type mismatch";
      return false;
    }

    // MustTail call needs stricter type match. See
    // Verifier::verifyMustTailCall().
    if (CB.isMustTailCall()) {
      PointerType *PF = dyn_cast<PointerType>(FormalTy);
      PointerType *PA = dyn_cast<PointerType>(ActualTy);
      if (!PF || !PA || PF->getAddressSpace() != PA->getAddressSpace()) {
        if (FailureReason)
          *FailureReason = "Musttail call Argument type mismatch";
        return false;
      }
    }
  }
  for (; I < NumArgs; I++) {
    // Vararg functions can have more arguments than parameters.
    assert(Callee->isVarArg());
    if (CB.paramHasAttr(I, Attribute::StructRet)) {
      if (FailureReason)
        *FailureReason = "SRet arg to vararg function";
      return false;
    }
  }

  return true;
}

CallBase &llvm::promoteCall(CallBase &CB, Function *Callee,
                            CastInst **RetBitCast) {
  assert(!CB.getCalledFunction() && "Only indirect call sites can be promoted");

  // Set the called function of the call site to be the given callee (but don't
  // change the type).
  CB.setCalledOperand(Callee);

  // Since the call site will no longer be direct, we must clear metadata that
  // is only appropriate for indirect calls. This includes !prof and !callees
  // metadata.
  CB.setMetadata(LLVMContext::MD_prof, nullptr);
  CB.setMetadata(LLVMContext::MD_callees, nullptr);

  // If the function type of the call site matches that of the callee, no
  // additional work is required.
  if (CB.getFunctionType() == Callee->getFunctionType())
    return CB;

  // Save the return types of the call site and callee.
  Type *CallSiteRetTy = CB.getType();
  Type *CalleeRetTy = Callee->getReturnType();

  // Change the function type of the call site the match that of the callee.
  CB.mutateFunctionType(Callee->getFunctionType());

  // Inspect the arguments of the call site. If an argument's type doesn't
  // match the corresponding formal argument's type in the callee, bitcast it
  // to the correct type.
  auto CalleeType = Callee->getFunctionType();
  auto CalleeParamNum = CalleeType->getNumParams();

  LLVMContext &Ctx = Callee->getContext();
  const AttributeList &CallerPAL = CB.getAttributes();
  // The new list of argument attributes.
  SmallVector<AttributeSet, 4> NewArgAttrs;
  bool AttributeChanged = false;

  for (unsigned ArgNo = 0; ArgNo < CalleeParamNum; ++ArgNo) {
    auto *Arg = CB.getArgOperand(ArgNo);
    Type *FormalTy = CalleeType->getParamType(ArgNo);
    Type *ActualTy = Arg->getType();
    if (FormalTy != ActualTy) {
      auto *Cast =
          CastInst::CreateBitOrPointerCast(Arg, FormalTy, "", CB.getIterator());
      CB.setArgOperand(ArgNo, Cast);

      // Remove any incompatible attributes for the argument.
      AttrBuilder ArgAttrs(Ctx, CallerPAL.getParamAttrs(ArgNo));
      ArgAttrs.remove(AttributeFuncs::typeIncompatible(
          FormalTy, CallerPAL.getParamAttrs(ArgNo)));

      // We may have a different byval/inalloca type.
      if (ArgAttrs.getByValType())
        ArgAttrs.addByValAttr(Callee->getParamByValType(ArgNo));
      if (ArgAttrs.getInAllocaType())
        ArgAttrs.addInAllocaAttr(Callee->getParamInAllocaType(ArgNo));

      NewArgAttrs.push_back(AttributeSet::get(Ctx, ArgAttrs));
      AttributeChanged = true;
    } else
      NewArgAttrs.push_back(CallerPAL.getParamAttrs(ArgNo));
  }

  // If the return type of the call site doesn't match that of the callee, cast
  // the returned value to the appropriate type.
  // Remove any incompatible return value attribute.
  AttrBuilder RAttrs(Ctx, CallerPAL.getRetAttrs());
  if (!CallSiteRetTy->isVoidTy() && CallSiteRetTy != CalleeRetTy) {
    createRetBitCast(CB, CallSiteRetTy, RetBitCast);
    RAttrs.remove(
        AttributeFuncs::typeIncompatible(CalleeRetTy, CallerPAL.getRetAttrs()));
    AttributeChanged = true;
  }

  // Set the new callsite attribute.
  if (AttributeChanged)
    CB.setAttributes(AttributeList::get(Ctx, CallerPAL.getFnAttrs(),
                                        AttributeSet::get(Ctx, RAttrs),
                                        NewArgAttrs));

  return CB;
}

CallBase &llvm::promoteCallWithIfThenElse(CallBase &CB, Function *Callee,
                                          MDNode *BranchWeights) {
  // If the called value is guarded by a CFI type test that the callee is known
  // to pass, the direct call can skip the check.
  std::optional<TypeTestGuard> Guard;
  if (HoistPromotionCondAboveTypeTest)
    if (auto G = findTypeTestGuard(CB.getParent(), CB.getCalledOperand()))
      if (canHoistPromotionCond(CB, *G) &&
          passesTypeTest(Callee, G->TypeTest, CB.getDataLayout()))
        Guard = G;

  // Version the indirect call site. If the called value is equal to the given
  // callee, 'NewInst' will be executed, otherwise the original call site will
  // be executed.
  CallBase &NewInst = versionCallSite(CB, Callee, BranchWeights);

  // Promote 'NewInst' so that it directly calls the desired function.
  CallBase &DirectCall = promoteCall(NewInst, Callee);
  if (Guard)
    hoistPromotionCondAboveTypeTest(*Guard, DirectCall, BranchWeights);
  return DirectCall;
}

CallBase *llvm::promoteCallWithIfThenElse(CallBase &CB, Function &Callee,
                                          PGOContextualProfile &CtxProf) {
  assert(CB.isIndirectCall());
  if (!CtxProf.isFunctionKnown(Callee))
    return nullptr;
  auto &Caller = *CB.getFunction();
  auto *CSInstr = CtxProfAnalysis::getCallsiteInstrumentation(CB);
  if (!CSInstr)
    return nullptr;
  const uint64_t CSIndex = CSInstr->getIndex()->getZExtValue();

  CallBase &DirectCall = promoteCall(
      versionCallSite(CB, &Callee, /*BranchWeights=*/nullptr), &Callee);
  CSInstr->moveBefore(CB.getIterator());
  const auto NewCSID = CtxProf.allocateNextCallsiteIndex(Caller);
  auto *NewCSInstr = cast<InstrProfCallsite>(CSInstr->clone());
  NewCSInstr->setIndex(NewCSID);
  NewCSInstr->setCallee(&Callee);
  NewCSInstr->insertBefore(DirectCall.getIterator());
  auto &DirectBB = *DirectCall.getParent();
  auto &IndirectBB = *CB.getParent();

  assert((CtxProfAnalysis::getBBInstrumentation(IndirectBB) == nullptr) &&
         "The ICP direct BB is new, it shouldn't have instrumentation");
  assert((CtxProfAnalysis::getBBInstrumentation(DirectBB) == nullptr) &&
         "The ICP indirect BB is new, it shouldn't have instrumentation");

  // Allocate counters for the new basic blocks.
  const uint32_t DirectID = CtxProf.allocateNextCounterIndex(Caller);
  const uint32_t IndirectID = CtxProf.allocateNextCounterIndex(Caller);
  auto *EntryBBIns =
      CtxProfAnalysis::getBBInstrumentation(Caller.getEntryBlock());
  auto *DirectBBIns = cast<InstrProfCntrInstBase>(EntryBBIns->clone());
  DirectBBIns->setIndex(DirectID);
  DirectBBIns->insertInto(&DirectBB, DirectBB.getFirstInsertionPt());

  auto *IndirectBBIns = cast<InstrProfCntrInstBase>(EntryBBIns->clone());
  IndirectBBIns->setIndex(IndirectID);
  IndirectBBIns->insertInto(&IndirectBB, IndirectBB.getFirstInsertionPt());

  const GlobalValue::GUID CalleeGUID = Callee.getGUID();
  const uint32_t NewCountersSize = IndirectID + 1;

  auto ProfileUpdater = [&](PGOCtxProfContext &Ctx) {
    assert(Ctx.guid() == Caller.getGUID());
    assert(NewCountersSize - 2 == Ctx.counters().size());
    // All the ctx-es belonging to a function must have the same size counters.
    Ctx.resizeCounters(NewCountersSize);

    // Maybe in this context, the indirect callsite wasn't observed at all. That
    // would make both direct and indirect BBs cold - which is what we already
    // have from resising the counters.
    if (!Ctx.hasCallsite(CSIndex))
      return;
    auto &CSData = Ctx.callsite(CSIndex);

    uint64_t TotalCount = 0;
    for (const auto &[_, V] : CSData)
      TotalCount += V.getEntrycount();
    uint64_t DirectCount = 0;
    // If we called the direct target, update the DirectCount. If we didn't, we
    // still want to update the indirect BB (to which the TotalCount goes, in
    // that case).
    if (auto It = CSData.find(CalleeGUID); It != CSData.end()) {
      assert(CalleeGUID == It->second.guid());
      DirectCount = It->second.getEntrycount();
      // This direct target needs to be moved to this caller under the
      // newly-allocated callsite index.
      assert(Ctx.callsites().count(NewCSID) == 0);
      Ctx.ingestContext(NewCSID, std::move(It->second));
      CSData.erase(CalleeGUID);
    }

    assert(TotalCount >= DirectCount);
    uint64_t IndirectCount = TotalCount - DirectCount;
    // The ICP's effect is as-if the direct BB would have been taken DirectCount
    // times, and the indirect BB, IndirectCount times
    Ctx.counters()[DirectID] = DirectCount;
    Ctx.counters()[IndirectID] = IndirectCount;
  };
  CtxProf.update(ProfileUpdater, Caller);
  return &DirectCall;
}

CallBase &llvm::promoteCallWithVTableCmp(CallBase &CB, Instruction *VPtr,
                                         Function *Callee,
                                         ArrayRef<Constant *> AddressPoints,
                                         MDNode *BranchWeights) {
  assert(!AddressPoints.empty() && "Caller should guarantee");
  // If the vtable pointer is guarded by a CFI type test that all the address
  // points are known to pass, the direct call can skip the check.
  std::optional<TypeTestGuard> Guard;
  if (HoistPromotionCondAboveTypeTest)
    if (auto G = findTypeTestGuard(CB.getParent(), VPtr))
      if (canHoistPromotionCond(CB, *G) &&
          all_of(AddressPoints, [&](Constant *AddressPoint) {
            return passesTypeTest(AddressPoint, G->TypeTest,
                                  CB.getDataLayout());
          }))
        Guard = G;

  IRBuilder<> Builder(&CB);
  SmallVector<Value *, 2> ICmps;
  for (auto &AddressPoint : AddressPoints) {
    ICmps.push_back(Builder.CreateICmpEQ(VPtr, AddressPoint));
    // The address comparison has made the vtable address significant.
    // Strip unnamed_addr so the vtable is recorded as address-significant
    // and kept unique by the linker.
    if (auto *GV =
            dyn_cast<GlobalValue>(AddressPoint->stripInBoundsConstantOffsets()))
      GV->setUnnamedAddr(GlobalValue::UnnamedAddr::None);
  }

  // TODO: Perform tree height reduction if the number of ICmps is high.
  Value *Cond = Builder.CreateOr(ICmps);

  // Version the indirect call site. If Cond is true, 'NewInst' will be
  // executed, otherwise the original call site will be executed.
  CallBase &NewInst = versionCallSiteWithCond(CB, Cond, BranchWeights);

  // Promote 'NewInst' so that it directly calls the desired function.
  CallBase &DirectCall = promoteCall(NewInst, Callee);
  if (Guard)
    hoistPromotionCondAboveTypeTest(*Guard, DirectCall, BranchWeights);
  return DirectCall;
}

bool llvm::tryPromoteCall(CallBase &CB) {
  assert(!CB.getCalledFunction());
  Module *M = CB.getCaller()->getParent();
  const DataLayout &DL = M->getDataLayout();
  Value *Callee = CB.getCalledOperand();

  LoadInst *VTableEntryLoad = dyn_cast<LoadInst>(Callee);
  if (!VTableEntryLoad)
    return false; // Not a vtable entry load.
  Value *VTableEntryPtr = VTableEntryLoad->getPointerOperand();
  APInt VTableOffset(DL.getIndexTypeSizeInBits(VTableEntryPtr->getType()), 0);
  Value *VTableBasePtr = VTableEntryPtr->stripAndAccumulateConstantOffsets(
      DL, VTableOffset, /* AllowNonInbounds */ true);
  LoadInst *VTablePtrLoad = dyn_cast<LoadInst>(VTableBasePtr);
  if (!VTablePtrLoad)
    return false; // Not a vtable load.
  Value *Object = VTablePtrLoad->getPointerOperand();
  APInt ObjectOffset(DL.getIndexTypeSizeInBits(Object->getType()), 0);
  Value *ObjectBase = Object->stripAndAccumulateConstantOffsets(
      DL, ObjectOffset, /* AllowNonInbounds */ true);
  if (!(isa<AllocaInst>(ObjectBase) && ObjectOffset == 0))
    // Not an Alloca or the offset isn't zero.
    return false;

  // Look for the vtable pointer store into the object by the ctor.
  BasicBlock::iterator BBI(VTablePtrLoad);
  Value *VTablePtr = FindAvailableLoadedValue(
      VTablePtrLoad, VTablePtrLoad->getParent(), BBI, 0, nullptr, nullptr);
  if (!VTablePtr || !VTablePtr->getType()->isPointerTy())
    return false; // No vtable found.
  APInt VTableOffsetGVBase(DL.getIndexTypeSizeInBits(VTablePtr->getType()), 0);
  Value *VTableGVBase = VTablePtr->stripAndAccumulateConstantOffsets(
      DL, VTableOffsetGVBase, /* AllowNonInbounds */ true);
  GlobalVariable *GV = dyn_cast<GlobalVariable>(VTableGVBase);
  if (!(GV && GV->isConstant() && GV->hasDefinitiveInitializer()))
    // Not in the form of a global constant variable with an initializer.
    return false;

  APInt VTableGVOffset = VTableOffsetGVBase + VTableOffset;
  if (!(VTableGVOffset.getActiveBits() <= 64))
    return false; // Out of range.

  Function *DirectCallee = nullptr;
  std::tie(DirectCallee, std::ignore) =
      getFunctionAtVTableOffset(GV, VTableGVOffset.getZExtValue(), *M);
  if (!DirectCallee)
    return false; // No function pointer found.

  if (!isLegalToPromote(CB, DirectCallee))
    return false;

  // Success.
  promoteCall(CB, DirectCallee);
  return true;
}

#undef DEBUG_TYPE
