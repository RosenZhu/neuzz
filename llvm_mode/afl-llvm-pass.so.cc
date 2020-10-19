/*
  Copyright 2015 Google LLC All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.
*/

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <map>
#include <set>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

using namespace llvm;

namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;
      AFLCoverage() : ModulePass(ID) { }

      bool runOnModule(Module &M) override;

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }

  };

}


char AFLCoverage::ID = 0;


/* Check users of CmpInst recursively, and find br instructions that use this cmpinst. 

user_inst: a user of the cmp_inst, or user of user of cmp_inst, or...
*/
bool isBranchRelated(Instruction *user_inst){
  // bool res = false;//, recursive_res = false;
  
  if (Value *vusr = dyn_cast<Value>(user_inst)){
    if (user_inst->user_empty()){
      return false;
    }

    for (auto Ur : user_inst->users()){
      // an instruction uses user_inst
      if (auto UrInst = dyn_cast<Instruction>(Ur)){
        /* If the user is a BranchInst, return true */
        if (isa<BranchInst>(UrInst)){
          return true;
        }

        if (isBranchRelated(UrInst)) return true;
      }

    }
  }


  return false;
}

bool AFLCoverage::runOnModule(Module &M) {

  LLVMContext &C = M.getContext();

  IntegerType *Int8Ty  = IntegerType::getInt8Ty(C);
  IntegerType *Int32Ty = IntegerType::getInt32Ty(C);
  IntegerType *Int64Ty = IntegerType::getInt64Ty(C);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  GlobalVariable *AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

  GlobalVariable *AFLPrevLoc = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  GlobalVariable *AFLPrevBBVal = new GlobalVariable(
      M, Int64Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_bbval",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);
  
  GlobalVariable *AFLCurBBVal = new GlobalVariable(
      M, Int64Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_cur_bbval",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */

  int inst_blocks = 0;

  for (auto &F : M){

    for (auto &BB : F) {
      
      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(&(*IP));

      if (AFL_R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int cur_loc = AFL_R(MAP_SIZE);

      ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

      /* Load prev_loc */

      LoadInst *PrevLoc = IRB.CreateLoad(AFLPrevLoc);
      PrevLoc->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt32Ty());

      /* Load SHM pointer */

      LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
      MapPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *BitMapIndex = IRB.CreateXor(PrevLocCasted, CurLoc);
      Value *MapPtrIdx = IRB.CreateGEP(MapPtr, BitMapIndex);

      /* Update bitmap */

      LoadInst *Counter = IRB.CreateLoad(MapPtrIdx);
      Counter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
      IRB.CreateStore(Incr, MapPtrIdx)
          ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      /* Set prev_loc to cur_loc >> 1 */

      StoreInst *Store =
          IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
      Store->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      /* Store 'edge' values: 
          The start block in an edge indicates the edge behaviour, so the 'edge' value 
          should be the value of the start block.
          BB values: XOR all values of operands in cmps;
       */
      
      // load previous bb value
      LoadInst *PrevBBVal = IRB.CreateLoad(AFLPrevBBVal);
      PrevBBVal->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *PrevBBValCasted = IRB.CreateZExt(PrevBBVal, IRB.getInt64Ty());
      //load shm pointer
      ConstantInt *ConstMapSize = ConstantInt::get(Int64Ty, MAP_SIZE);
      Value *EdgeValMapPtrIdx = IRB.CreateGEP(MapPtr, 
                                      IRB.CreateAdd(BitMapIndex, ConstMapSize));
      // Get current BB value and update bbvalue map
      
      //Value *EdgeVal;
      bool curZero = false; // flag that indicates the value of current bb is zero
      Instruction* Instterm = dyn_cast<Instruction>(BB.getTerminator());
      if (Instterm){
        
        if(isa<SwitchInst>(Instterm)) {
          SwitchInst* Sw = cast<SwitchInst>(Instterm);
          
          Value *SwCond = Sw->getCondition();
          if (SwCond && SwCond->getType()->isIntegerTy() && !isa<ConstantInt>(SwCond)){
            IRBuilder<> IRBSW(Sw);
            Value *SwCondExt = IRBSW.CreateZExt(SwCond, Int64Ty);
            // EdgeVal = IRBSW.CreateXor(PrevBBValCasted, SwCondExt);
            // IRBSW.CreateStore(EdgeVal, EdgeValMapPtrIdx)
            //     ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

            // value of start block
            IRBSW.CreateStore(PrevBBValCasted, EdgeValMapPtrIdx)
                ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
            // Set __afl_prev_bbval to cur_bbval
            IRBSW.CreateStore(SwCondExt, AFLPrevBBVal)
                ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
            
          } else {
            curZero = true;
          }
          
        }

        else if(isa<BranchInst>(Instterm)) {
          BranchInst *Brinst = cast<BranchInst>(Instterm);
          
          if (Brinst->isConditional()){
            StoreInst *ResetCurBB = 
                IRB.CreateStore(ConstantInt::get(Int64Ty, 0), AFLCurBBVal); 
            ResetCurBB->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

            for (auto &I : BB){
              if (auto cinst = dyn_cast<CmpInst>(&I)){
                // if (isBranchRelated(&I)){

                  /* Insert Point is critical. Wrong insert point results in segfault. 
                  The insert point can't be set to be after a given instruction -- 
                  instead, you should set it to be before the next instruction.*/
                  Instruction *InsertPoint = cinst->getNextNode();
                  IRBuilder<> IRBRI(InsertPoint);
                  Value *opArg[2], *opdCasted[2];
                  opArg[0] = cinst->getOperand(0);
                  opArg[1] = cinst->getOperand(1);
                  Type *OpType = opArg[0]->getType();
                  if (!((OpType->isIntegerTy() && OpType->getIntegerBitWidth() <= 64) ||
                          OpType->isFloatTy() || OpType->isDoubleTy())){
                    continue;
                  } else if (OpType->isFloatTy()){
                    /* bitcast can only cast between the types with the same length of bits */
                    Value *tempOp0 = IRBRI.CreateBitCast(opArg[0], Int32Ty);
                    Value *tempOp1 = IRBRI.CreateBitCast(opArg[1], Int32Ty);
                    opdCasted[0] = IRBRI.CreateZExt(tempOp0, Int64Ty);
                    opdCasted[1] = IRBRI.CreateZExt(tempOp1, Int64Ty);
                  } else if (OpType->isDoubleTy()){
                    opdCasted[0] = IRBRI.CreateBitCast(opArg[0], Int64Ty);
                    opdCasted[1] = IRBRI.CreateBitCast(opArg[1], Int64Ty);
                  } else if (OpType->isIntegerTy() && OpType->getIntegerBitWidth() < 64){
                    opdCasted[0] = IRBRI.CreateZExt(opArg[0], Int64Ty);
                    opdCasted[1] = IRBRI.CreateZExt(opArg[1], Int64Ty);
                  } else{
                    opdCasted[0] = opArg[0];
                    opdCasted[1] = opArg[1];
                  }

                  // /* shift right by 1. 
                  // The ‘lshr’ instruction (logical shift right) returns the first operand 
                  //   shifted to the right a specified number of bits with zero fill. */
                  // opdCasted[1] = IRB.CreateLShr(opdCasted[1], 1);
                  
                  LoadInst *LoadCurBBVal = IRBRI.CreateLoad(AFLCurBBVal);
                  Value *LoadCurBBValCasted = IRBRI.CreateZExt(LoadCurBBVal, IRBRI.getInt64Ty());
                
                  Value *CurBBVal = IRBRI.CreateXor(LoadCurBBValCasted, 
                                              IRBRI.CreateXor(opdCasted[0], opdCasted[1]));
                  
                  IRBRI.CreateStore(CurBBVal, AFLCurBBVal) 
                      ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
                  
                // }
              }
            }
            
            IRBuilder<> endIRBR(Brinst);
            LoadInst *endBBVal = endIRBR.CreateLoad(AFLCurBBVal);
            Value *endCurBBValCasted = endIRBR.CreateZExt(endBBVal, endIRBR.getInt64Ty());
            // EdgeVal = endIRBR.CreateXor(PrevBBValCasted, endCurBBValCasted);
            // endIRBR.CreateStore(EdgeVal, EdgeValMapPtrIdx)
            //     ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

            // value of start block
            endIRBR.CreateStore(PrevBBValCasted, EdgeValMapPtrIdx)
                ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
            // update previous data
            endIRBR.CreateStore(endCurBBValCasted, AFLPrevBBVal)
                ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

          } else {
            curZero = true;
          }
        }

        else{
          curZero = true;
        }
         
      } else curZero = true;

      if (curZero){
        // Other kinds of BBs get the BB value 0
        //EdgeVal = IRB.CreateXor(PrevBBValCasted, ConstantInt::get(Int64Ty, 0));
        IRB.CreateStore(PrevBBValCasted, EdgeValMapPtrIdx)
            ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
            
        StoreInst *BBValStore = IRB.CreateStore(ConstantInt::get(Int64Ty, 0), AFLPrevBBVal);
        BBValStore->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      }

      /* Instrumented BBs */
      inst_blocks++;

    }

  }


  /* Say something nice. */

  if (!be_quiet) {

    if (!inst_blocks) WARNF("No instrumentation targets found.");
    else OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks, getenv("AFL_HARDEN") ? "hardened" :
             ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
              "ASAN/MSAN" : "non-hardened"), inst_ratio);
    OKF("For NEUZZ Verify.");
  }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}


static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_ModuleOptimizerEarly, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
