/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

// vim:ts=2:sw=2:et:

#include "common/LLVMWarningsPush.hpp"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/PassAnalysisSupport.h"
#include "llvm/Analysis/TargetFolder.h"
#include "common/LLVMWarningsPop.hpp"

#include "common/LLVMUtils.h"

#include "llvmWrapper/IR/Instructions.h"

#include "common/IGCIRBuilder.h"

#include "GenISAIntrinsics/GenIntrinsics.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/MetaDataUtilsWrapper.h"

#include "Compiler/CISACodeGen/Emu64OpsPass.h"

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;


using std::ldexp;


namespace {

    typedef llvm::IGCIRBuilder<TargetFolder> BuilderType;
    typedef std::pair<Value*, Value*> ValuePair;

    class InstExpander;
    class Preprocessor;

    class Emu64Ops : public FunctionPass {
        friend class InstExpander;
        friend class Preprocessor;

        const DataLayout* DL;
        IGC::CodeGenContext* CGC;

        BuilderType* IRB;
        InstExpander* Expander;

        LLVMContext* TheContext;
        Module* TheModule;
        Function* TheFunction;

        typedef DenseMap<Value*, ValuePair> ValueMapTy;
        ValueMapTy ValueMap;

        // Special bitcasts of 64-bit arguments, which need special handling as we
        // cannot replace argument type.
        SmallPtrSet<BitCastInst*, 8> Arg64Casts;

        SmallPtrSet<Instruction*, 32> DeadInsts;

    public:

        static char ID;

        Emu64Ops() : FunctionPass(ID), DL(nullptr), CGC(nullptr), IRB(nullptr),
            Expander(nullptr), TheContext(nullptr), TheModule(nullptr),
            TheFunction(nullptr) {
            initializeEmu64OpsPass(*PassRegistry::getPassRegistry());
        }

        bool runOnFunction(Function& F) override;

    private:
        void getAnalysisUsage(AnalysisUsage& AU) const override {
            AU.addRequired<CodeGenContextWrapper>();
            AU.addRequired<MetaDataUtilsWrapper>();
        }

        LLVMContext* getContext() const { return TheContext; }
        Module* getModule() const { return TheModule; }
        Function* getFunction() const { return TheFunction; }

        bool hasPtr64() const { return DL->getPointerSizeInBits() == 64; }
        bool isPtr64(const PointerType* PtrTy) const {
            return CGC->getRegisterPointerSizeInBits(PtrTy->getAddressSpace()) == 64;
        }
        bool isInt64(const Type* Ty) const { return Ty->isIntegerTy(64); }
        bool isInt64(const Value* V) const { return isInt64(V->getType()); }

        bool isArg64Cast(BitCastInst* BC) const { return Arg64Casts.count(BC) != 0; }

        Type* getV2Int32Ty(unsigned NumElts = 1) const {
            return VectorType::get(IRB->getInt32Ty(), NumElts * 2);
        }

        ValuePair getExpandedValues(Value* V);
        void setExpandedValues(Value* V, Value* Lo, Value* Hi);

        unsigned getAlignment(LoadInst* LD) const {
            unsigned Align = LD->getAlignment();
            if (Align == 0)
                Align = DL->getABITypeAlignment(LD->getType());
            return Align;
        }

        unsigned getAlignment(StoreInst* ST) const {
            unsigned Align = ST->getAlignment();
            if (Align == 0)
                Align = DL->getABITypeAlignment(ST->getType());
            return Align;
        }

        void dupMemoryAttribute(LoadInst* NewLD, LoadInst* RefLD, unsigned Off) const {
            unsigned Align = getAlignment(RefLD);

            NewLD->setVolatile(RefLD->isVolatile());
            NewLD->setAlignment(unsigned(MinAlign(Align, Off)));
            NewLD->setOrdering(RefLD->getOrdering());
            IGCLLVM::CopySyncScopeID(NewLD, RefLD);
        }

        void dupMemoryAttribute(StoreInst* NewST, StoreInst* RefST, unsigned Off) const {
            unsigned Align = getAlignment(RefST);

            NewST->setVolatile(RefST->isVolatile());
            NewST->setAlignment(unsigned(MinAlign(Align, Off)));
            NewST->setOrdering(RefST->getOrdering());
            IGCLLVM::CopySyncScopeID(NewST, RefST);
        }

        bool expandArguments(Function& F);
        bool preparePHIs(Function& F);
        bool expandInsts(Function& F);
        bool populatePHIs(Function& F);
        bool removeDeadInsts();
    };

    class InstExpander : public InstVisitor<InstExpander, bool> {
        friend class InstVisitor<InstExpander, bool>;

        Emu64Ops* Emu;
        BuilderType* IRB;

    public:
        InstExpander(Emu64Ops* E, BuilderType* B) : Emu(E), IRB(B) {}

        bool expand(Instruction* I);

    private:
        bool visitInstruction(Instruction&);

        bool visitRet(ReturnInst&);
        bool visitBr(BranchInst&) { return false; }
        bool visitSwitch(SwitchInst&) { return false; }
        bool visitIndirectBr(IndirectBrInst&) { return false; }
        bool visitInvoke(InvokeInst&) { return false; }
        bool visitResume(ResumeInst&) { return false; }
        bool visitUnreachable(UnreachableInst&) { return false; }

        bool visitAdd(BinaryOperator&);
        bool visitFAdd(BinaryOperator&) { return false; }
        bool visitSub(BinaryOperator&);
        bool visitFSub(BinaryOperator&) { return false; }
        bool visitMul(BinaryOperator&);
        bool visitFMul(BinaryOperator&) { return false; }
        bool visitSDiv(BinaryOperator&);
        bool visitUDiv(BinaryOperator&);
        bool visitFDiv(BinaryOperator&) { return false; }
        bool visitSRem(BinaryOperator&);
        bool visitURem(BinaryOperator&);
        bool visitFRem(BinaryOperator&) { return false; }

        bool visitShl(BinaryOperator&);
        bool visitLShr(BinaryOperator&);
        bool visitAShr(BinaryOperator&);
        bool visitAnd(BinaryOperator&);
        bool visitOr(BinaryOperator&);
        bool visitXor(BinaryOperator&);

        bool visitAlloca(AllocaInst&) { return false; }
        bool visitLoad(LoadInst&);
        bool visitStore(StoreInst&);
        bool visitGetElementPtr(GetElementPtrInst&) { return false; }
        bool visitFence(FenceInst&) { return false; }
        bool visitAtomicCmpXchg(AtomicCmpXchgInst&);
        bool visitAtomicRMW(AtomicRMWInst&);

        bool visitTrunc(TruncInst&);
        bool visitSExt(SExtInst&);
        bool visitZExt(ZExtInst&);
        bool visitFPToUI(FPToUIInst&);
        bool visitFPToSI(FPToSIInst&);
        bool visitUIToFP(UIToFPInst&);
        bool visitSIToFP(SIToFPInst&);
        bool visitFPTrunc(FPTruncInst&) { return false; }
        bool visitFPExt(FPExtInst&) { return false; }
        bool visitPtrToInt(PtrToIntInst&);
        bool visitIntToPtr(IntToPtrInst&);
        bool visitBitCast(BitCastInst&);
        bool visitAddrSpaceCast(AddrSpaceCastInst&) { return false; }

        bool visitICmp(ICmpInst&);
        bool visitFCmp(FCmpInst&) { return false; }
        bool visitPHI(PHINode&);
        bool visitCall(CallInst&);
        bool visitSelect(SelectInst&);

        bool visitVAArg(VAArgInst&);
        bool visitExtractElement(ExtractElementInst&);
        bool visitInsertElement(InsertElementInst&);
        bool visitShuffleVector(ShuffleVectorInst&) { return false; }
        bool visitExtractValue(ExtractValueInst&);
        bool visitInsertValue(InsertValueInst&);
        bool visitLandingPad(LandingPadInst&);

        Value* convertUIToFP32(Type* DstTy, Value* Lo, Value* Hi, Instruction* Pos);
    };

    class Preprocessor {
        Emu64Ops* Emu;
        BuilderType* IRB;

    public:
        Preprocessor(Emu64Ops* E, BuilderType* B) : Emu(E), IRB(B) {}

        bool preprocess(Function& F) {
            bool Changed = false;
            // Preprocess additions with overflow.
            for (auto& BB : F) {
                for (auto BI = BB.begin(), BE = BB.end(); BI != BE; /*EMPTY*/) {
                    IntrinsicInst* II = dyn_cast<IntrinsicInst>(BI);
                    if (!II || II->getIntrinsicID() != Intrinsic::uadd_with_overflow ||
                        !II->getArgOperand(0)->getType()->isIntegerTy(64)) {
                        ++BI;
                        continue;
                    }

                    IRB->SetInsertPoint(II);

                    Value* LHS = II->getArgOperand(0);
                    Value* RHS = II->getArgOperand(1);
                    Value* Res = IRB->CreateAdd(LHS, RHS);
                    Value* Overflow = IRB->CreateICmpULT(Res, LHS);

                    for (auto UI = II->user_begin(),
                        UE = II->user_end(); UI != UE; /*EMPTY*/) {
                        User* U = *UI++;
                        ExtractValueInst* Ex = cast<ExtractValueInst>(U);
                        assert(Ex->getNumIndices() == 1);

                        unsigned Idx = *Ex->idx_begin();
                        assert(Idx == 0 || Idx == 1);
                        Ex->replaceAllUsesWith((Idx == 0) ? Res : Overflow);
                        Ex->eraseFromParent();
                    }
                    assert(II->user_empty());
                    ++BI;
                    II->eraseFromParent();
                    Changed = true;
                }
            }
            // Preprocess non-LOAD/-STORE pointer usage if there's 64-bit pointer.
            if (Emu->hasPtr64()) {
                for (auto& BB : F) {
                    SmallVector<Instruction*, 16> LocalDeadInsts;
                    for (auto BI = BB.begin(), BE = BB.end(); BI != BE; ++BI) {
                        switch (BI->getOpcode()) {
                        default: // By default, NOTHING!
                            break;
                        case Instruction::ICmp: {
                            ICmpInst* Cmp = cast<ICmpInst>(BI);
                            PointerType* PtrTy =
                                dyn_cast<PointerType>(Cmp->getOperand(0)->getType());
                            if (!PtrTy || !Emu->isPtr64(PtrTy))
                                continue;

                            IRB->SetInsertPoint(Cmp);

                            Value* LHS = Cmp->getOperand(0);
                            Value* RHS = Cmp->getOperand(1);
                            LHS = IRB->CreatePtrToInt(LHS, IRB->getInt64Ty());
                            RHS = IRB->CreatePtrToInt(RHS, IRB->getInt64Ty());
                            Cmp->setOperand(0, LHS);
                            Cmp->setOperand(1, RHS);

                            Changed = true;
                            break;
                        }
                        case Instruction::Select: {
                            SelectInst* SI = cast<SelectInst>(BI);
                            PointerType* PtrTy = dyn_cast<PointerType>(SI->getType());
                            if (!PtrTy || !Emu->isPtr64(PtrTy))
                                continue;

                            IRB->SetInsertPoint(SI);

                            Value* TVal = SI->getTrueValue();
                            Value* FVal = SI->getFalseValue();
                            TVal = IRB->CreatePtrToInt(TVal, IRB->getInt64Ty());
                            FVal = IRB->CreatePtrToInt(FVal, IRB->getInt64Ty());
                            Value* NewPtr = IRB->CreateSelect(SI->getCondition(), TVal, FVal);
                            NewPtr = IRB->CreateIntToPtr(NewPtr, PtrTy);
                            SI->replaceAllUsesWith(NewPtr);
                            LocalDeadInsts.push_back(SI);

                            Changed = true;
                            break;
                        }
                        case Instruction::Load: {
                            LoadInst* LD = cast<LoadInst>(BI);
                            PointerType* PtrTy = dyn_cast<PointerType>(LD->getType());
                            if (!PtrTy || !Emu->isPtr64(PtrTy))
                                continue;

                            IRB->SetInsertPoint(LD);

                            // Cast the original pointer to pointer to pointer to i64.
                            Value* OldPtr = LD->getPointerOperand();
                            PointerType* OldPtrTy = cast<PointerType>(OldPtr->getType());
                            PointerType* NewPtrTy =
                                IRB->getInt64Ty()->getPointerTo(OldPtrTy->getAddressSpace());
                            Value* NewPtr = IRB->CreateBitCast(OldPtr, NewPtrTy);
                            // Create new load.
                            LoadInst* NewLD = IRB->CreateLoad(NewPtr);
                            Emu->dupMemoryAttribute(NewLD, LD, 0);
                            // Cast the load i64 back to pointer.
                            Value* NewVal = IRB->CreateIntToPtr(NewLD, PtrTy);
                            LD->replaceAllUsesWith(NewVal);
                            LocalDeadInsts.push_back(LD);

                            Changed = true;
                            break;
                        }
                        case Instruction::Store: {
                            StoreInst* ST = cast<StoreInst>(BI);
                            PointerType* PtrTy =
                                dyn_cast<PointerType>(ST->getValueOperand()->getType());
                            if (!PtrTy || !Emu->isPtr64(PtrTy))
                                continue;

                            IRB->SetInsertPoint(ST);

                            // Cast the pointer to pointer to pointer to i64.
                            Value* OldPtr = ST->getPointerOperand();
                            PointerType* OldPtrTy = cast<PointerType>(OldPtr->getType());
                            PointerType* NewPtrTy =
                                IRB->getInt64Ty()->getPointerTo(OldPtrTy->getAddressSpace());
                            Value* NewPtr = IRB->CreateBitCast(OldPtr, NewPtrTy);
                            // Cast the pointer to be stored into i64.
                            Value* OldVal = ST->getValueOperand();
                            Value* NewVal = IRB->CreatePtrToInt(OldVal, IRB->getInt64Ty());
                            // Create new store.
                            StoreInst* NewST = IRB->CreateStore(NewVal, NewPtr);
                            Emu->dupMemoryAttribute(NewST, ST, 0);
                            LocalDeadInsts.push_back(ST);

                            Changed = true;
                            break;
                        }
                        case Instruction::IntToPtr: {
                            IntToPtrInst* I2P = cast<IntToPtrInst>(BI);
                            Value* Src = I2P->getOperand(0);
                            PointerType* PtrTy = cast<PointerType>(I2P->getType());
                            if (!Emu->isPtr64(PtrTy) && !Emu->isInt64(Src))
                                continue;

                            IRB->SetInsertPoint(I2P);
                            unsigned int ptrSize = Emu->CGC->getRegisterPointerSizeInBits(PtrTy->getAddressSpace());

                            Src = IRB->CreateZExtOrTrunc(Src, IRB->getIntNTy(ptrSize));
                            I2P->setOperand(0, Src);

                            Changed = true;
                            break;
                        }
                        case Instruction::PtrToInt: {
                            PtrToIntInst* P2I = cast<PtrToIntInst>(BI);
                            Value* Src = P2I->getOperand(0);
                            PointerType* PtrTy = cast<PointerType>(Src->getType());
                            if (!Emu->isPtr64(PtrTy) || Emu->isInt64(Src))
                                continue;

                            IRB->SetInsertPoint(P2I);

                            Value* NewVal = IRB->CreatePtrToInt(Src, IRB->getInt64Ty());
                            NewVal = IRB->CreateZExtOrTrunc(NewVal, P2I->getType());
                            P2I->replaceAllUsesWith(NewVal);
                            LocalDeadInsts.push_back(P2I);

                            Changed = true;
                            break;
                        }
                        }
                    }
                    // Remove dead instructions.
                    for (auto I : LocalDeadInsts)
                        I->eraseFromParent();
                }
            }

            return Changed;
        }
    };

    char Emu64Ops::ID = 0;

} // End anonymous namespace

FunctionPass* createEmu64OpsPass() {
    return new Emu64Ops();
}

#define PASS_FLAG     "igc-emu64ops"
#define PASS_DESC     "IGC 64-bit ops emulation"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(Emu64Ops, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(Emu64Ops, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY, PASS_ANALYSIS)

bool Emu64Ops::runOnFunction(Function& F) {
    // Skip non-kernel function.
    MetaDataUtils* MDU = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    auto FII = MDU->findFunctionsInfoItem(&F);
    if (FII == MDU->end_FunctionsInfo())
        return false;

    DL = &F.getParent()->getDataLayout();
    CGC = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

    BuilderType TheBuilder(F.getContext(), TargetFolder(*DL));
    InstExpander TheExpander(this, &TheBuilder);
    Preprocessor ThePreprocessor(this, &TheBuilder);
    IRB = &TheBuilder;
    Expander = &TheExpander;

    TheContext = &F.getContext();
    TheModule = F.getParent();
    TheFunction = &F;

    ValueMap.clear();
    Arg64Casts.clear();
    DeadInsts.clear();

    bool Changed = false;
    Changed |= ThePreprocessor.preprocess(F);
    Changed |= expandArguments(F);
    Changed |= preparePHIs(F);
    Changed |= expandInsts(F);
    Changed |= populatePHIs(F);
    Changed |= removeDeadInsts();

    return Changed;
}

ValuePair Emu64Ops::getExpandedValues(Value* V) {
    ValueMapTy::iterator VMI;
    bool New;
    std::tie(VMI, New) = ValueMap.insert(std::make_pair(V, ValuePair()));
    if (!New)
        return VMI->second;

    if (dyn_cast<ConstantInt>(V)) {
        Value* Lo = IRB->CreateTrunc(V, IRB->getInt32Ty());
        Value* Hi = IRB->CreateTrunc(IRB->CreateLShr(V, 32), IRB->getInt32Ty());
        VMI->second = std::make_pair(Lo, Hi);
        return VMI->second;
    }

    if (dyn_cast<UndefValue>(V)) {
        Value* Lo = UndefValue::get(IRB->getInt32Ty());
        Value* Hi = UndefValue::get(IRB->getInt32Ty());
        VMI->second = std::make_pair(Lo, Hi);
        return VMI->second;
    }

    if (ConstantExpr * CE = dyn_cast<ConstantExpr>(V)) {
        Value* Lo = nullptr;
        Value* Hi = nullptr;
        if (isa<PtrToIntOperator>(CE)) {
            V = IRB->CreateBitCast(V, getV2Int32Ty());
            Lo = IRB->CreateExtractElement(V, IRB->getInt32(0));
            Hi = IRB->CreateExtractElement(V, IRB->getInt32(1));
        }
        else {
            Lo = IRB->CreateTrunc(V, IRB->getInt32Ty());
            Hi = IRB->CreateTrunc(IRB->CreateLShr(V, 32), IRB->getInt32Ty());
        }
        VMI->second = std::make_pair(Lo, Hi);
        return VMI->second;
    }

    errs() << "V = " << *V << '\n';
    llvm_unreachable("TODO: NOT IMPLEMENTED!");
}

void Emu64Ops::setExpandedValues(Value* V, Value* Lo, Value* Hi) {
    ValuePair Pair = std::make_pair(Lo, Hi);
    ValueMap.insert(std::make_pair(V, Pair));
}

bool Emu64Ops::expandArguments(Function& F) {
    Instruction* Pos = &F.getEntryBlock().front();
    IRB->SetInsertPoint(Pos);

    bool Changed = false;
    for (auto& Arg : F.args()) {
        if (!isInt64(&Arg))
            continue;
        Value* V = IRB->CreateBitCast(&Arg, getV2Int32Ty());
        Value* Lo = IRB->CreateExtractElement(V, IRB->getInt32(0));
        Value* Hi = IRB->CreateExtractElement(V, IRB->getInt32(1));
        setExpandedValues(&Arg, Lo, Hi);
        Arg64Casts.insert(cast<BitCastInst>(V));
        Changed = true;
    }

    return Changed;
}

bool Emu64Ops::preparePHIs(Function& F) {
    bool Changed = false;
    for (auto& BB : F) {
        PHINode* PN = nullptr;
        for (auto BI = BB.begin(); (PN = dyn_cast<PHINode>(BI)) != nullptr; ++BI) {
            if (!isInt64(PN))
                continue;
            IRB->SetInsertPoint(PN);

            unsigned NumIncomingValues = PN->getNumIncomingValues();
            Value* Lo = IRB->CreatePHI(IRB->getInt32Ty(), NumIncomingValues);
            Value* Hi = IRB->CreatePHI(IRB->getInt32Ty(), NumIncomingValues);
            setExpandedValues(PN, Lo, Hi);

            DeadInsts.insert(PN);
            Changed = true;
        }
    }

    return Changed;
}

bool Emu64Ops::expandInsts(Function& F) {
    ReversePostOrderTraversal<Function*> RPOT(&F);
    bool Changed = false;
    for (auto& BB : RPOT) {
        for (auto BI = BB->begin(), BE = BB->end(); BI != BE; /*EMPTY*/) {
            Instruction* I = &(*BI++);

            bool LocalChanged = Expander->expand(I);
            Changed |= LocalChanged;

            if (LocalChanged) {
                BI = std::next(BasicBlock::iterator(I));
                BE = I->getParent()->end();
                DeadInsts.insert(I);
            }
        }
    }

    return Changed;
}

bool Emu64Ops::populatePHIs(Function& F) {
    bool Changed = false;
    for (auto& BB : F) {
        PHINode* PN;
        for (auto BI = BB.begin(); (PN = dyn_cast<PHINode>(BI)) != nullptr; ++BI) {
            if (!isInt64(PN))
                continue;
            Value* L = nullptr, * H = nullptr;
            std::tie(L, H) = getExpandedValues(PN);

            PHINode* Lo = cast<PHINode>(L);
            PHINode* Hi = cast<PHINode>(H);
            for (auto& Op : PN->operands()) {
                BasicBlock* BB = PN->getIncomingBlock(Op);
                std::tie(L, H) = getExpandedValues(Op.get());
                Lo->addIncoming(L, BB);
                Hi->addIncoming(H, BB);
            }
            Changed = true;
        }
    }
    return Changed;
}

bool Emu64Ops::removeDeadInsts() {
    bool Changed = false;
    for (auto* I : DeadInsts) {
        Type* Ty = I->getType();
        if (!Ty->isVoidTy())
            I->replaceAllUsesWith(UndefValue::get(Ty));
        I->eraseFromParent();
        Changed = true;
    }
    return Changed;
}

bool InstExpander::expand(Instruction* I) {
    IRB->SetInsertPoint(I);

    if (!visit(*I))
        return false;

    return true;
}

bool InstExpander::visitInstruction(Instruction& I) {
#if 1
    errs() << "I = " << I << '\n';
#endif
    llvm_unreachable("UNKNOWN INSTRUCTION is BEING EXPANDED!");
    return false;
}

bool InstExpander::visitRet(ReturnInst& RI) {
    if (Value * V = RI.getReturnValue())
        if (Emu->isInt64(V))
            // TODO: Add 64-bit return value support when function/subroutine call is
            // supported.
            llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}

bool InstExpander::visitAdd(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* L0 = nullptr, * H0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(BinOp.getOperand(1));

    GenISAIntrinsic::ID GIID = GenISAIntrinsic::GenISA_add_pair;
    Function* IFunc = GenISAIntrinsic::getDeclaration(Emu->getModule(), GIID);
    Value* V = IRB->CreateCall4(IFunc, L0, H0, L1, H1);
    Value* Lo = IRB->CreateExtractValue(V, 0);
    Value* Hi = IRB->CreateExtractValue(V, 1);

    Emu->setExpandedValues(&BinOp, Lo, Hi);
    return true;
}

bool InstExpander::visitSub(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* L0 = nullptr, * H0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(BinOp.getOperand(1));

    GenISAIntrinsic::ID GIID = GenISAIntrinsic::GenISA_sub_pair;
    Function* IFunc = GenISAIntrinsic::getDeclaration(Emu->getModule(), GIID);
    Value* V = IRB->CreateCall4(IFunc, L0, H0, L1, H1);
    Value* Lo = IRB->CreateExtractValue(V, 0);
    Value* Hi = IRB->CreateExtractValue(V, 1);

    Emu->setExpandedValues(&BinOp, Lo, Hi);
    return true;
}

bool InstExpander::visitMul(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* L0 = nullptr, * H0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(BinOp.getOperand(1));

    GenISAIntrinsic::ID GIID = GenISAIntrinsic::GenISA_mul_pair;
    Function* IFunc = GenISAIntrinsic::getDeclaration(Emu->getModule(), GIID);
    Value* V = IRB->CreateCall4(IFunc, L0, H0, L1, H1);
    Value* Lo = IRB->CreateExtractValue(V, 0);
    Value* Hi = IRB->CreateExtractValue(V, 1);

    Emu->setExpandedValues(&BinOp, Lo, Hi);
    return true;
}

bool InstExpander::visitSDiv(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;
    llvm_unreachable("There should not be `sdiv` which is already emulated by library call.");
    return false;
}

bool InstExpander::visitUDiv(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;
    llvm_unreachable("There should not be `udiv` which is already emulated by library call.");
    return false;
}

bool InstExpander::visitSRem(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;
    llvm_unreachable("There should not be `srem` which is already emulated by library call.");
    return false;
}

bool InstExpander::visitURem(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;
    llvm_unreachable("There should not be `urem` which is already emulated by library call.");
    return false;
}

bool InstExpander::visitShl(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* ShAmt = nullptr;
    std::tie(ShAmt, std::ignore) = Emu->getExpandedValues(BinOp.getOperand(1));

    BasicBlock* OldBB = BinOp.getParent();
    BasicBlock* InnerTBB = nullptr;
    BasicBlock* InnerFBB = nullptr;
    Value* InnerResLo = nullptr;
    Value* InnerResHi = nullptr;
    Value* ResLo = nullptr;
    Value* ResHi = nullptr;

    Value* Cond = nullptr;

    if (!isa<ConstantInt>(ShAmt)) {
        // Create outer if-endif to handle the special case where `ShAmt` is zero.
        // We have option to handle that with `((S >> (~ShAmt)) >> 1)`. However, as
        // a zero `ShAmt` is a very rare case so that the outer branch should be
        // uniform one in most cases.
        Value* NE = IRB->CreateICmpNE(IRB->CreateAnd(ShAmt, 63),
            Constant::getNullValue(ShAmt->getType()));
        BasicBlock* JointBB = OldBB->splitBasicBlock(&BinOp);
        ResLo = PHINode::Create(IRB->getInt32Ty(), 2, ".shl.outer.merge.lo", &BinOp);
        ResHi = PHINode::Create(IRB->getInt32Ty(), 2, ".shl.outer.merge.hi", &BinOp);

        BasicBlock* TrueBB = BasicBlock::Create(*Emu->getContext(),
            ".shl.outer.true.branch");
        TrueBB->insertInto(Emu->getFunction(), JointBB);
        Instruction* TrueJmp = BranchInst::Create(JointBB, TrueBB);

        OldBB->getTerminator()->eraseFromParent();
        BranchInst::Create(TrueBB, JointBB, NE, OldBB);

        // Create the inner branch.
        IRB->SetInsertPoint(&(*TrueBB->begin()));

        Cond = IRB->CreateICmpEQ(IRB->CreateAnd(ShAmt, 32),
            Constant::getNullValue(ShAmt->getType()));
        // Prepare to generate branches to handle the case where `ShAmt` is less
        // than 32 or otherwise.
        BasicBlock* InnerJBB = TrueBB->splitBasicBlock(TrueJmp);
        InnerResLo = PHINode::Create(IRB->getInt32Ty(), 2, ".shl.merge.inner.lo", TrueJmp);
        InnerResHi = PHINode::Create(IRB->getInt32Ty(), 2, ".shl.merge.inner.hi", TrueJmp);

        InnerTBB = BasicBlock::Create(*Emu->getContext(), ".shl.inner.true.branch");
        InnerTBB->insertInto(Emu->getFunction(), InnerJBB);
        BranchInst::Create(InnerJBB, InnerTBB);

        InnerFBB = BasicBlock::Create(*Emu->getContext(), ".shl.inner.false.branch");
        InnerFBB->insertInto(Emu->getFunction(), InnerJBB);
        BranchInst::Create(InnerJBB, InnerFBB);

        TrueBB->getTerminator()->eraseFromParent();
        BranchInst::Create(InnerTBB, InnerFBB, Cond, TrueBB);

        // The result is the same as the source if ShAmt is 0, i.e. NE is
        // false.
        cast<PHINode>(ResLo)->addIncoming(Lo, OldBB);
        cast<PHINode>(ResHi)->addIncoming(Hi, OldBB);
        // Prepare the result from inner if-else-endif.
        cast<PHINode>(ResLo)->addIncoming(InnerResLo, InnerJBB);
        cast<PHINode>(ResHi)->addIncoming(InnerResHi, InnerJBB);
    }
    else
        Cond = IRB->CreateICmpEQ(IRB->CreateAnd(ShAmt, 32),
            Constant::getNullValue(ShAmt->getType()));

    if (Cond == IRB->getTrue() || InnerTBB) {
        if (InnerTBB) IRB->SetInsertPoint(&(*InnerTBB->begin()));

        Value* L = IRB->CreateShl(Lo, ShAmt);
        Value* Amt =
            isa<ConstantInt>(ShAmt) ? IRB->CreateSub(IRB->getInt32(32), ShAmt) :
            IRB->CreateNeg(ShAmt);
        Value* T0 = IRB->CreateLShr(Lo, Amt);
        Value* H = IRB->CreateShl(Hi, ShAmt);
        H = IRB->CreateOr(H, T0);

        if (InnerTBB) {
            assert(isa<PHINode>(InnerResLo) && isa<PHINode>(InnerResHi));
            cast<PHINode>(InnerResLo)->addIncoming(L, InnerTBB);
            cast<PHINode>(InnerResHi)->addIncoming(H, InnerTBB);
        }
        else {
            ResLo = L;
            ResHi = H;
        }
    }

    if (Cond == IRB->getFalse() || InnerFBB) {
        if (InnerFBB) IRB->SetInsertPoint(&(*InnerFBB->begin()));

        Value* L = IRB->getInt32(0);
        Value* Amt =
            isa<ConstantInt>(ShAmt) ? IRB->CreateSub(ShAmt, IRB->getInt32(32)) : ShAmt;
        Value* H = IRB->CreateShl(Lo, Amt);

        if (InnerFBB) {
            assert(isa<PHINode>(InnerResLo) && isa<PHINode>(InnerResHi));
            cast<PHINode>(InnerResLo)->addIncoming(L, InnerFBB);
            cast<PHINode>(InnerResHi)->addIncoming(H, InnerFBB);
        }
        else {
            ResLo = L;
            ResHi = H;
        }
    }

    Emu->setExpandedValues(&BinOp, ResLo, ResHi);
    return true;
}

bool InstExpander::visitLShr(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* ShAmt = nullptr;
    std::tie(ShAmt, std::ignore) = Emu->getExpandedValues(BinOp.getOperand(1));

    BasicBlock* OldBB = BinOp.getParent();
    BasicBlock* InnerTBB = nullptr;
    BasicBlock* InnerFBB = nullptr;
    Value* InnerResLo = nullptr;
    Value* InnerResHi = nullptr;
    Value* ResLo = nullptr;
    Value* ResHi = nullptr;

    Value* Cond = nullptr;

    if (!isa<ConstantInt>(ShAmt)) {
        // Create outer if-endif to handle the special case where `ShAmt` is zero.
        // We have option to handle that with `((S >> (~ShAmt)) >> 1)`. However, as
        // a zero `ShAmt` is a very rare case so that the outer branch should be
        // uniform one in most cases.
        Value* NE = IRB->CreateICmpNE(IRB->CreateAnd(ShAmt, 63),
            Constant::getNullValue(ShAmt->getType()));
        BasicBlock* JointBB = OldBB->splitBasicBlock(&BinOp);
        ResLo = PHINode::Create(IRB->getInt32Ty(), 2, ".lshr.outer.merge.lo", &BinOp);
        ResHi = PHINode::Create(IRB->getInt32Ty(), 2, ".lshr.outer.merge.hi", &BinOp);

        BasicBlock* TrueBB = BasicBlock::Create(*Emu->getContext(),
            ".lshr.outer.true.branch");
        TrueBB->insertInto(Emu->getFunction(), JointBB);
        Instruction* TrueJmp = BranchInst::Create(JointBB, TrueBB);

        OldBB->getTerminator()->eraseFromParent();
        BranchInst::Create(TrueBB, JointBB, NE, OldBB);

        // Create the inner branch.
        IRB->SetInsertPoint(&(*TrueBB->begin()));

        Cond = IRB->CreateICmpEQ(IRB->CreateAnd(ShAmt, 32),
            Constant::getNullValue(ShAmt->getType()));
        // Prepare to generate branches to handle the case where `ShAmt` is less
        // than 32 or otherwise.
        BasicBlock* InnerJBB = TrueBB->splitBasicBlock(TrueJmp);
        InnerResLo = PHINode::Create(IRB->getInt32Ty(), 2, ".lshr.merge.inner.lo", TrueJmp);
        InnerResHi = PHINode::Create(IRB->getInt32Ty(), 2, ".lshr.merge.inner.hi", TrueJmp);

        InnerTBB = BasicBlock::Create(*Emu->getContext(), ".lshr.inner.true.branch");
        InnerTBB->insertInto(Emu->getFunction(), InnerJBB);
        BranchInst::Create(InnerJBB, InnerTBB);

        InnerFBB = BasicBlock::Create(*Emu->getContext(), ".lshr.inner.false.branch");
        InnerFBB->insertInto(Emu->getFunction(), InnerJBB);
        BranchInst::Create(InnerJBB, InnerFBB);

        TrueBB->getTerminator()->eraseFromParent();
        BranchInst::Create(InnerTBB, InnerFBB, Cond, TrueBB);

        // The result is the same as the source if ShAmt is 0, i.e. NE is
        // false.
        cast<PHINode>(ResLo)->addIncoming(Lo, OldBB);
        cast<PHINode>(ResHi)->addIncoming(Hi, OldBB);
        // Prepare the result from inner if-else-endif.
        cast<PHINode>(ResLo)->addIncoming(InnerResLo, InnerJBB);
        cast<PHINode>(ResHi)->addIncoming(InnerResHi, InnerJBB);
    }
    else
        Cond = IRB->CreateICmpEQ(IRB->CreateAnd(ShAmt, 32),
            Constant::getNullValue(ShAmt->getType()));

    if (Cond == IRB->getTrue() || InnerTBB) {
        if (InnerTBB) IRB->SetInsertPoint(&(*InnerTBB->begin()));

        Value* H = IRB->CreateLShr(Hi, ShAmt);
        Value* Amt =
            isa<ConstantInt>(ShAmt) ? IRB->CreateSub(IRB->getInt32(32), ShAmt) :
            IRB->CreateNeg(ShAmt);
        Value* T0 = IRB->CreateShl(Hi, Amt);
        Value* L = IRB->CreateLShr(Lo, ShAmt);
        L = IRB->CreateOr(L, T0);

        if (InnerTBB) {
            assert(isa<PHINode>(InnerResLo) && isa<PHINode>(InnerResHi));
            cast<PHINode>(InnerResLo)->addIncoming(L, InnerTBB);
            cast<PHINode>(InnerResHi)->addIncoming(H, InnerTBB);
        }
        else {
            ResLo = L;
            ResHi = H;
        }
    }

    if (Cond == IRB->getFalse() || InnerFBB) {
        if (InnerFBB) IRB->SetInsertPoint(&(*InnerFBB->begin()));

        Value* H = IRB->getInt32(0);
        Value* Amt =
            isa<ConstantInt>(ShAmt) ? IRB->CreateSub(ShAmt, IRB->getInt32(32)) : ShAmt;
        Value* L = IRB->CreateLShr(Hi, Amt);

        if (InnerFBB) {
            assert(isa<PHINode>(InnerResLo) && isa<PHINode>(InnerResHi));
            cast<PHINode>(InnerResLo)->addIncoming(L, InnerFBB);
            cast<PHINode>(InnerResHi)->addIncoming(H, InnerFBB);
        }
        else {
            ResLo = L;
            ResHi = H;
        }
    }

    Emu->setExpandedValues(&BinOp, ResLo, ResHi);
    return true;
}

bool InstExpander::visitAShr(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* ShAmt = nullptr;
    std::tie(ShAmt, std::ignore) = Emu->getExpandedValues(BinOp.getOperand(1));

    BasicBlock* OldBB = BinOp.getParent();
    BasicBlock* InnerTBB = nullptr;
    BasicBlock* InnerFBB = nullptr;
    Value* InnerResLo = nullptr;
    Value* InnerResHi = nullptr;
    Value* ResLo = nullptr;
    Value* ResHi = nullptr;

    Value* Cond = nullptr;

    if (!isa<ConstantInt>(ShAmt)) {
        // Create outer if-endif to handle the special case where `ShAmt` is zero.
        // We have option to handle that with `((S >> (~ShAmt)) >> 1)`. However, as
        // a zero `ShAmt` is a very rare case so that the outer branch should be
        // uniform one in most cases.
        Value* NE = IRB->CreateICmpNE(IRB->CreateAnd(ShAmt, 63),
            Constant::getNullValue(ShAmt->getType()));
        BasicBlock* JointBB = OldBB->splitBasicBlock(&BinOp);
        ResLo = PHINode::Create(IRB->getInt32Ty(), 2, ".ashr.outer.merge.lo", &BinOp);
        ResHi = PHINode::Create(IRB->getInt32Ty(), 2, ".ashr.outer.merge.hi", &BinOp);

        BasicBlock* TrueBB = BasicBlock::Create(*Emu->getContext(),
            ".ashr.outer.true.branch");
        TrueBB->insertInto(Emu->getFunction(), JointBB);
        Instruction* TrueJmp = BranchInst::Create(JointBB, TrueBB);

        OldBB->getTerminator()->eraseFromParent();
        BranchInst::Create(TrueBB, JointBB, NE, OldBB);

        // Create the inner branch.
        IRB->SetInsertPoint(&(*TrueBB->begin()));

        Cond = IRB->CreateICmpEQ(IRB->CreateAnd(ShAmt, 32),
            Constant::getNullValue(ShAmt->getType()));
        // Prepare to generate branches to handle the case where `ShAmt` is less
        // than 32 or otherwise.
        BasicBlock* InnerJBB = TrueBB->splitBasicBlock(TrueJmp);
        InnerResLo = PHINode::Create(IRB->getInt32Ty(), 2, ".ashr.merge.inner.lo", TrueJmp);
        InnerResHi = PHINode::Create(IRB->getInt32Ty(), 2, ".ashr.merge.inner.hi", TrueJmp);

        InnerTBB = BasicBlock::Create(*Emu->getContext(), ".ashr.inner.true.branch");
        InnerTBB->insertInto(Emu->getFunction(), InnerJBB);
        BranchInst::Create(InnerJBB, InnerTBB);

        InnerFBB = BasicBlock::Create(*Emu->getContext(), ".ashr.inner.false.branch");
        InnerFBB->insertInto(Emu->getFunction(), InnerJBB);
        BranchInst::Create(InnerJBB, InnerFBB);

        TrueBB->getTerminator()->eraseFromParent();
        BranchInst::Create(InnerTBB, InnerFBB, Cond, TrueBB);

        // The result is the same as the source if ShAmt is 0, i.e. NE is
        // false.
        cast<PHINode>(ResLo)->addIncoming(Lo, OldBB);
        cast<PHINode>(ResHi)->addIncoming(Hi, OldBB);
        // Prepare the result from inner if-else-endif.
        cast<PHINode>(ResLo)->addIncoming(InnerResLo, InnerJBB);
        cast<PHINode>(ResHi)->addIncoming(InnerResHi, InnerJBB);
    }
    else
        Cond = IRB->CreateICmpEQ(IRB->CreateAnd(ShAmt, 32),
            Constant::getNullValue(ShAmt->getType()));

    if (Cond == IRB->getTrue() || InnerTBB) {
        if (InnerTBB) IRB->SetInsertPoint(&(*InnerTBB->begin()));

        Value* H = IRB->CreateAShr(Hi, ShAmt);
        Value* Amt =
            isa<ConstantInt>(ShAmt) ? IRB->CreateSub(IRB->getInt32(32), ShAmt) :
            IRB->CreateNeg(ShAmt);
        Value* T0 = IRB->CreateShl(Hi, Amt);
        Value* L = IRB->CreateLShr(Lo, ShAmt);
        L = IRB->CreateOr(L, T0);

        if (InnerTBB) {
            assert(isa<PHINode>(InnerResLo) && isa<PHINode>(InnerResHi));
            cast<PHINode>(InnerResLo)->addIncoming(L, InnerTBB);
            cast<PHINode>(InnerResHi)->addIncoming(H, InnerTBB);
        }
        else {
            ResLo = L;
            ResHi = H;
        }
    }

    if (Cond == IRB->getFalse() || InnerFBB) {
        if (InnerFBB) IRB->SetInsertPoint(&(*InnerFBB->begin()));

        Value* H = IRB->CreateAShr(Hi, 31);
        Value* Amt =
            isa<ConstantInt>(ShAmt) ? IRB->CreateSub(ShAmt, IRB->getInt32(32)) : ShAmt;
        Value* L = IRB->CreateAShr(Hi, Amt);

        if (InnerFBB) {
            assert(isa<PHINode>(InnerResLo) && isa<PHINode>(InnerResHi));
            cast<PHINode>(InnerResLo)->addIncoming(L, InnerFBB);
            cast<PHINode>(InnerResHi)->addIncoming(H, InnerFBB);
        }
        else {
            ResLo = L;
            ResHi = H;
        }
    }

    Emu->setExpandedValues(&BinOp, ResLo, ResHi);
    return true;
}

bool InstExpander::visitAnd(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* L0 = nullptr, * H0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(BinOp.getOperand(1));

    Value* Lo = IRB->CreateAnd(L0, L1);
    Value* Hi = IRB->CreateAnd(H0, H1);

    Emu->setExpandedValues(&BinOp, Lo, Hi);
    return true;
}

bool InstExpander::visitOr(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* L0 = nullptr, * H0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(BinOp.getOperand(1));

    Value* Lo = IRB->CreateOr(L0, L1);
    Value* Hi = IRB->CreateOr(H0, H1);

    Emu->setExpandedValues(&BinOp, Lo, Hi);
    return true;
}

bool InstExpander::visitXor(BinaryOperator& BinOp) {
    if (!Emu->isInt64(&BinOp))
        return false;

    Value* L0 = nullptr, * H0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(BinOp.getOperand(0));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(BinOp.getOperand(1));

    Value* Lo = IRB->CreateXor(L0, L1);
    Value* Hi = IRB->CreateXor(H0, H1);

    Emu->setExpandedValues(&BinOp, Lo, Hi);
    return true;
}

bool InstExpander::visitLoad(LoadInst& LD) {
    if (!Emu->isInt64(&LD))
        return false;

    Value* OldPtr = LD.getPointerOperand();
    PointerType* OldPtrTy = cast<PointerType>(OldPtr->getType());
    PointerType* NewPtrTy =
        Emu->getV2Int32Ty()->getPointerTo(OldPtrTy->getAddressSpace());
    Value* NewPtr = IRB->CreatePointerCast(OldPtr, NewPtrTy);

    LoadInst* NewLD = IRB->CreateLoad(NewPtr);
    Emu->dupMemoryAttribute(NewLD, &LD, 0);

    Value* Lo = IRB->CreateExtractElement(NewLD, IRB->getInt32(0));
    Value* Hi = IRB->CreateExtractElement(NewLD, IRB->getInt32(1));

    Emu->setExpandedValues(&LD, Lo, Hi);
    return true;
}

bool InstExpander::visitStore(StoreInst& ST) {
    Value* V = ST.getValueOperand();
    if (!Emu->isInt64(V))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(V);

    Type* V2I32Ty = Emu->getV2Int32Ty();
    Value* NewVal = UndefValue::get(V2I32Ty);
    NewVal = IRB->CreateInsertElement(NewVal, Lo, IRB->getInt32(0));
    NewVal = IRB->CreateInsertElement(NewVal, Hi, IRB->getInt32(1));

    Value* OldPtr = ST.getPointerOperand();
    PointerType* OldPtrTy = cast<PointerType>(OldPtr->getType());
    PointerType* NewPtrTy = V2I32Ty->getPointerTo(OldPtrTy->getAddressSpace());
    Value* NewPtr = IRB->CreatePointerCast(OldPtr, NewPtrTy);

    StoreInst* NewST = IRB->CreateStore(NewVal, NewPtr);
    Emu->dupMemoryAttribute(NewST, &ST, 0);

    return true;
}

bool InstExpander::visitAtomicCmpXchg(AtomicCmpXchgInst& ACXI) {
    Value* V = ACXI.getCompareOperand();
    if (!Emu->isInt64(V))
        return false;
    llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}

bool InstExpander::visitAtomicRMW(AtomicRMWInst& RMW) {
    if (!Emu->isInt64(&RMW))
        return false;
    llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}

bool InstExpander::visitTrunc(TruncInst& TI) {
    Value* Src = TI.getOperand(0);
    if (!Emu->isInt64(Src))
        return false;

    Value* Lo = nullptr;
    std::tie(Lo, std::ignore) = Emu->getExpandedValues(Src);
    assert(Lo->getType()->getScalarSizeInBits() >=
        TI.getType()->getScalarSizeInBits());

    if (Lo->getType()->getScalarSizeInBits() !=
        TI.getType()->getScalarSizeInBits())
        Lo = IRB->CreateTrunc(Lo, TI.getType());

    TI.replaceAllUsesWith(Lo);
    return true;
}

bool InstExpander::visitSExt(SExtInst& SEI) {
    if (!Emu->isInt64(&SEI))
        return false;

    Value* Src = SEI.getOperand(0);
    Type* SrcTy = SEI.getSrcTy();
    unsigned SrcWidth = SrcTy->getIntegerBitWidth();
    assert(SrcWidth <= 32);

    Value* Lo = Src;
    if (SrcWidth < 32)
        Lo = IRB->CreateSExt(Lo, IRB->getInt32Ty());
    Value* Hi = IRB->CreateAShr(Lo, IRB->getInt32(31));

    Emu->setExpandedValues(&SEI, Lo, Hi);
    return true;
}

bool InstExpander::visitZExt(ZExtInst& ZEI) {
    if (!Emu->isInt64(&ZEI))
        return false;

    Value* Src = ZEI.getOperand(0);
    Type* SrcTy = ZEI.getSrcTy();
    unsigned SrcWidth = SrcTy->getIntegerBitWidth();
    assert(SrcWidth <= 32);

    Value* Lo = Src;
    if (SrcWidth < 32)
        Lo = IRB->CreateZExt(Lo, IRB->getInt32Ty());
    Value* Hi = IRB->getInt32(0);

    Emu->setExpandedValues(&ZEI, Lo, Hi);
    return true;
}

bool InstExpander::visitFPToUI(FPToUIInst& F2U) {
    if (!Emu->isInt64(&F2U))
        return false;

    Intrinsic::ID IID;
    Value* Src = F2U.getOperand(0);
    Type* SrcTy = Src->getType();

    if (SrcTy->isHalfTy()) {
        // Convert half directly into 32-bit integer.
        Value* Lo = IRB->CreateFPToUI(Src, IRB->getInt32Ty());
        // FIXME: Due to the current OCL builtin implementation, the conversion
        // from ulong to half and its saturated version share the same LLVM IR. We
        // cannot tell them during the emulation. Special handle the out-of-range
        // case for half. We need to revise OCL builtin to use different LLVM IR.
        Value* EQ =
            IRB->CreateICmpEQ(Lo, Constant::getAllOnesValue(IRB->getInt32Ty()));
        Value* Hi =
            IRB->CreateSelect(EQ, Constant::getAllOnesValue(IRB->getInt32Ty()),
                Constant::getNullValue(IRB->getInt32Ty()));

        Emu->setExpandedValues(&F2U, Lo, Hi);
        return true;
    }

    IID = Intrinsic::trunc;
    Function* Trunc = Intrinsic::getDeclaration(Emu->getModule(), IID, SrcTy);
    IID = Intrinsic::fma;
    Function* Fma = Intrinsic::getDeclaration(Emu->getModule(), IID, SrcTy);

    Value* FC0 = ConstantFP::get(SrcTy, ldexp(1., -32));
    Value* FC1 = ConstantFP::get(SrcTy, ldexp(-1., 32));

    Value* HiF = nullptr;
    Value* Hi = nullptr;
    if (SrcTy->isDoubleTy()) {
        // HW doesn't support rounding on double but does support it on float,
        // We have to build our own based on integer conversion.
        Hi = IRB->CreateFPToUI(IRB->CreateFMul(Src, FC0), IRB->getInt32Ty());
        HiF = IRB->CreateUIToFP(Hi, SrcTy);
    }
    else {
        HiF = IRB->CreateCall(Trunc, IRB->CreateFMul(Src, FC0));
        Hi = IRB->CreateFPToUI(HiF, IRB->getInt32Ty());
    }
    Value* LoF = IRB->CreateCall3(Fma, HiF, FC1, Src);
    Value* Lo = IRB->CreateFPToUI(LoF, IRB->getInt32Ty());

    Emu->setExpandedValues(&F2U, Lo, Hi);
    return true;
}

bool InstExpander::visitFPToSI(FPToSIInst& F2S) {
    if (!Emu->isInt64(&F2S))
        return false;

    Intrinsic::ID IID;
    Value* Src = F2S.getOperand(0);
    Type* SrcTy = Src->getType();

    if (SrcTy->isHalfTy()) {
        // Convert half directly into 32-bit integer.
        Value* Lo = IRB->CreateFPToSI(Src, IRB->getInt32Ty());
        Value* Hi = IRB->CreateAShr(Lo, 31);
        // FIXME: Due to the current OCL builtin implementation, the conversion
        // from ulong to half and its saturated version share the same LLVM IR. We
        // cannot tell them during the emulation. Special handle the out-of-range
        // case for half. We need to revise OCL builtin to use different LLVM IR.
        Value* IMax = IRB->getInt32(0x7FFFFFFFU);
        Value* IMin = IRB->getInt32(0x80000000U);
        Value* EQ = IRB->CreateICmpEQ(Lo, IMax);
        Hi = IRB->CreateSelect(EQ, Lo, Hi);
        Lo = IRB->CreateSelect(EQ, Constant::getAllOnesValue(IRB->getInt32Ty()),
            Lo);
        EQ = IRB->CreateICmpEQ(Lo, IMin);
        Hi = IRB->CreateSelect(EQ, Lo, Hi);
        Lo = IRB->CreateSelect(EQ, Constant::getNullValue(IRB->getInt32Ty()), Lo);

        Emu->setExpandedValues(&F2S, Lo, Hi);
        return true;
    }

    Value* Sign = nullptr;
    if (SrcTy->isDoubleTy()) {
        Sign = IRB->CreateBitCast(Src, Emu->getV2Int32Ty());
        Sign = IRB->CreateExtractElement(Sign, IRB->getInt32(1));
    }
    else {
        assert(SrcTy->isFloatTy() && "Unknown float type!");
        Sign = IRB->CreateBitCast(Src, IRB->getInt32Ty());
    }
    Sign = IRB->CreateAShr(Sign, 31);

    IID = Intrinsic::fabs;
    Function* FAbs = Intrinsic::getDeclaration(Emu->getModule(), IID, SrcTy);
    IID = Intrinsic::trunc;
    Function* Trunc = Intrinsic::getDeclaration(Emu->getModule(), IID, SrcTy);
    IID = Intrinsic::fma;
    Function* Fma = Intrinsic::getDeclaration(Emu->getModule(), IID, SrcTy);

    Value* FC0 = ConstantFP::get(SrcTy, ldexp(1., -32));
    Value* FC1 = ConstantFP::get(SrcTy, ldexp(-1., 32));

    Src = IRB->CreateCall(FAbs, Src);
    Value* HiF = nullptr;
    Value* Hi = nullptr;
    if (SrcTy->isDoubleTy()) {
        // HW doesn't support rounding on double but does support it on float,
        // We have to build our own based on integer conversion.
        Hi = IRB->CreateFPToUI(IRB->CreateFMul(Src, FC0), IRB->getInt32Ty());
        HiF = IRB->CreateUIToFP(Hi, SrcTy);
    }
    else {
        HiF = IRB->CreateCall(Trunc, IRB->CreateFMul(Src, FC0));
        Hi = IRB->CreateFPToUI(HiF, IRB->getInt32Ty());
    }
    Value* LoF = IRB->CreateCall3(Fma, HiF, FC1, Src);
    Value* Lo = IRB->CreateFPToUI(LoF, IRB->getInt32Ty());

    Lo = IRB->CreateXor(Lo, Sign);
    Hi = IRB->CreateXor(Hi, Sign);

    GenISAIntrinsic::ID GIID = GenISAIntrinsic::GenISA_sub_pair;
    Function* IFunc = GenISAIntrinsic::getDeclaration(Emu->getModule(), GIID);
    Value* V = IRB->CreateCall4(IFunc, Lo, Hi, Sign, Sign);
    Lo = IRB->CreateExtractValue(V, 0);
    Hi = IRB->CreateExtractValue(V, 1);

    Emu->setExpandedValues(&F2S, Lo, Hi);
    return true;
}

// Note: This method uses splitBasciBlock() which moves Pos (and all
// instructions following it till the end of basic block) to a new basic block.
// If IRB's insert point is set to Pos or an instruction after Pos in the same
// basic block than the insert basic block value in IRB will be invalid after
// this method completes. A correct insert point should be set in IRB after this
// method is called.
//
Value* InstExpander::convertUIToFP32(Type* DstTy, Value* Lo, Value* Hi, Instruction* Pos) {
    BuilderType::InsertPointGuard Guard(*IRB);
    IRB->SetInsertPoint(Pos);

    Intrinsic::ID IID;
    IID = Intrinsic::ctlz;
    Function* Lzd = Intrinsic::getDeclaration(Emu->getModule(), IID, Lo->getType());

    Value* ShAmt = IRB->CreateCall2(Lzd, Hi, IRB->getFalse());
    // Check ShAmt == 32
    Value* NE = IRB->CreateICmpNE(ShAmt, IRB->getInt32(32));

    BasicBlock* OldBB = Pos->getParent();
    BasicBlock* JointBB = OldBB->splitBasicBlock(Pos);
    PHINode* Res = PHINode::Create(IRB->getInt32Ty(), 2, ".u2f.outer.merge", Pos);

    {
        BuilderType::InsertPointGuard Guard(*IRB);

        BasicBlock* TrueBB = BasicBlock::Create(*Emu->getContext(), ".u2f.outer.true.branch");
        TrueBB->insertInto(Emu->getFunction(), JointBB);
        Instruction* TrueJmp = BranchInst::Create(JointBB, TrueBB);

        OldBB->getTerminator()->eraseFromParent();
        BranchInst::Create(TrueBB, JointBB, NE, OldBB);

        IRB->SetInsertPoint(&(*TrueBB->begin()));
        // Check ShAmt == 0
        NE = IRB->CreateICmpNE(ShAmt, Constant::getNullValue(ShAmt->getType()));

        BasicBlock* InnerJBB = TrueBB->splitBasicBlock(TrueJmp);
        PHINode* InnerResHi = PHINode::Create(IRB->getInt32Ty(), 2, ".u2f.inner.merge.hi", TrueJmp);
        PHINode* InnerResLo = PHINode::Create(IRB->getInt32Ty(), 2, ".u2f.inner.merge.lo", TrueJmp);

        BasicBlock* InnerTBB = BasicBlock::Create(*Emu->getContext(), ".u2f.inner.true.branch");
        InnerTBB->insertInto(Emu->getFunction(), InnerJBB);
        BranchInst::Create(InnerJBB, InnerTBB);

        TrueBB->getTerminator()->eraseFromParent();
        BranchInst::Create(InnerTBB, InnerJBB, NE, TrueBB);

        IRB->SetInsertPoint(&(*InnerTBB->begin()));
        // 0 < ShAmt < 32
        Value* L = IRB->CreateShl(Lo, ShAmt);
        Value* H = IRB->CreateShl(Hi, ShAmt);
        Value* T0 = IRB->CreateLShr(Lo, IRB->CreateNeg(ShAmt));
        H = IRB->CreateOr(H, T0);

        InnerResHi->addIncoming(Hi, TrueBB);
        InnerResLo->addIncoming(Lo, TrueBB);

        InnerResHi->addIncoming(H, InnerTBB);
        InnerResLo->addIncoming(L, InnerTBB);

        Instruction* InnerJmp = InnerJBB->getTerminator();
        IRB->SetInsertPoint(InnerJmp);
        // Check InnerResLo != 0 to round correctly.
        NE = IRB->CreateICmpNE(InnerResLo, Constant::getNullValue(InnerResLo->getType()));

        BasicBlock* RoundingJBB = InnerJBB->splitBasicBlock(InnerJmp);
        PHINode* RoundingRes = PHINode::Create(IRB->getInt32Ty(), 2, ".u2f.rounding.merge.hi", InnerJmp);

        BasicBlock* RoundingBB = BasicBlock::Create(*Emu->getContext(), ".u2f.roudning.branch");
        RoundingBB->insertInto(Emu->getFunction(), RoundingJBB);
        BranchInst::Create(RoundingJBB, RoundingBB);

        InnerJBB->getTerminator()->eraseFromParent();
        BranchInst::Create(RoundingBB, RoundingJBB, NE, InnerJBB);

        // Rounding
        IRB->SetInsertPoint(&(*RoundingBB->begin()));
        Value* Rounded = IRB->CreateOr(InnerResHi, IRB->getInt32(1));

        RoundingRes->addIncoming(InnerResHi, InnerJBB);
        RoundingRes->addIncoming(Rounded, RoundingBB);

        Res->addIncoming(Lo, OldBB);
        Res->addIncoming(RoundingRes, RoundingJBB);
    }

    Value* NewVal = IRB->CreateUIToFP(Res, DstTy);
    // Adjust
    Value* Exp = IRB->CreateSub(IRB->getInt32(127 + 32), ShAmt);
    Exp = IRB->CreateShl(Exp, IRB->getInt32(23));
    Exp = IRB->CreateBitCast(Exp, NewVal->getType());
    NewVal = IRB->CreateFMul(NewVal, Exp);

    return NewVal;
}

bool InstExpander::visitUIToFP(UIToFPInst& U2F) {
    Value* Src = U2F.getOperand(0);
    if (!Emu->isInt64(Src))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(Src);

    Type* DstTy = U2F.getType();
    Value* NewVal = nullptr;

    if (ConstantInt * CI = dyn_cast<ConstantInt>(Hi)) {
        if (CI->isNullValue()) {
            NewVal = IRB->CreateUIToFP(Lo, DstTy);
            U2F.replaceAllUsesWith(NewVal);
            return true;
        }
    }

    if (DstTy->isDoubleTy()) {
        Intrinsic::ID IID = Intrinsic::fma;
        Function* Fma = Intrinsic::getDeclaration(Emu->getModule(), IID, DstTy);
        Value* FC0 = ConstantFP::get(DstTy, ldexp(1., 32));
        Value* LoF = IRB->CreateUIToFP(Lo, DstTy);
        Value* HiF = IRB->CreateUIToFP(Hi, DstTy);
        NewVal = IRB->CreateCall3(Fma, HiF, FC0, LoF);
    }
    else {
        NewVal = convertUIToFP32(IRB->getFloatTy(), Lo, Hi, &U2F);
        IRB->SetInsertPoint(&U2F);
        // It's OK to apply the same approach in `convertUIToFP32` to convert 64-bit
        // integer into half. But, it would introduce a little more instructions to
        // properly round the remaining 48 bits into the high 16 bits. Instead, that
        // 64-bit integer is firstly converted into `float` and then converted into
        // `half`.
        if (DstTy->isHalfTy())
            NewVal = IRB->CreateFPTrunc(NewVal, DstTy);
    }

    U2F.replaceAllUsesWith(NewVal);
    return true;
}

bool InstExpander::visitSIToFP(SIToFPInst& S2F) {
    Value* Src = S2F.getOperand(0);
    if (!Emu->isInt64(Src))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(Src);

    Type* DstTy = S2F.getType();
    Value* NewVal = nullptr;

    if (ConstantInt * CI = dyn_cast<ConstantInt>(Hi)) {
        if (CI->isNullValue()) {
            NewVal = IRB->CreateUIToFP(Lo, DstTy);
            S2F.replaceAllUsesWith(NewVal);
            return true;
        }
        if (CI->isMinusOne()) {
            NewVal = IRB->CreateUIToFP(Lo, DstTy);
            NewVal = IRB->CreateFNeg(NewVal);
            S2F.replaceAllUsesWith(NewVal);
            return true;
        }
    }

    if (DstTy->isDoubleTy()) {
        Intrinsic::ID IID = Intrinsic::fma;
        Function* Fma = Intrinsic::getDeclaration(Emu->getModule(), IID, DstTy);
        Value* FC0 = ConstantFP::get(DstTy, ldexp(1., 32));
        Value* LoF = IRB->CreateUIToFP(Lo, DstTy);
        Value* HiF = IRB->CreateSIToFP(Hi, DstTy);
        NewVal = IRB->CreateCall3(Fma, HiF, FC0, LoF);
    }
    else {
        GenISAIntrinsic::ID GIID = GenISAIntrinsic::GenISA_sub_pair;
        Function* SubPair = GenISAIntrinsic::getDeclaration(Emu->getModule(), GIID);

        Value* Sign = IRB->CreateAShr(Hi, 31);

        // Get abs(x) = (x ^ s) - s;
        Lo = IRB->CreateXor(Lo, Sign);
        Hi = IRB->CreateXor(Hi, Sign);
        Value* V = IRB->CreateCall4(SubPair, Lo, Hi, Sign, Sign);
        Lo = IRB->CreateExtractValue(V, 0);
        Hi = IRB->CreateExtractValue(V, 1);

        NewVal = convertUIToFP32(IRB->getFloatTy(), Lo, Hi, &S2F);
        IRB->SetInsertPoint(&S2F);
        NewVal = IRB->CreateBitCast(NewVal, IRB->getInt32Ty());
        Sign = IRB->CreateAnd(Sign, IRB->getInt32(0x80000000));
        NewVal = IRB->CreateOr(NewVal, Sign);
        NewVal = IRB->CreateBitCast(NewVal, IRB->getFloatTy());
        if (DstTy->isHalfTy())
            NewVal = IRB->CreateFPTrunc(NewVal, DstTy);
    }

    S2F.replaceAllUsesWith(NewVal);
    return true;
}

bool InstExpander::visitPtrToInt(PtrToIntInst& P2I) {
    if (!Emu->isInt64(&P2I))
        return false;

    Value* Ptr = P2I.getOperand(0);

    GenISAIntrinsic::ID GIID = GenISAIntrinsic::GenISA_ptr_to_pair;
    Function* IFunc = GenISAIntrinsic::getDeclaration(Emu->getModule(), GIID, Ptr->getType());
    Value* V = IRB->CreateCall(IFunc, Ptr);
    Value* Lo = IRB->CreateExtractValue(V, 0);
    Value* Hi = IRB->CreateExtractValue(V, 1);

    Emu->setExpandedValues(&P2I, Lo, Hi);
    return true;
}

bool InstExpander::visitIntToPtr(IntToPtrInst& I2P) {
    Value* Src = I2P.getOperand(0);
    if (!Emu->isInt64(Src))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(Src);

    GenISAIntrinsic::ID GIID = GenISAIntrinsic::GenISA_pair_to_ptr;
    Function* IFunc = GenISAIntrinsic::getDeclaration(Emu->getModule(), GIID, I2P.getType());
    Value* Ptr = IRB->CreateCall2(IFunc, Lo, Hi);
    Ptr = IRB->CreateBitCast(Ptr, I2P.getType());

    I2P.replaceAllUsesWith(Ptr);
    return true;
}

bool InstExpander::visitBitCast(BitCastInst& BC) {
    Value* Src = BC.getOperand(0);
    if (!Emu->isInt64(&BC) && !Emu->isInt64(Src))
        return false;

    if (Emu->isInt64(&BC)) {
        Src = IRB->CreateBitCast(Src, Emu->getV2Int32Ty());
        Value* Lo = IRB->CreateExtractElement(Src, IRB->getInt32(0));
        Value* Hi = IRB->CreateExtractElement(Src, IRB->getInt32(1));

        Emu->setExpandedValues(&BC, Lo, Hi);
        return true;
    }

    assert(Emu->isInt64(Src));

    // Skip argument which is already prepared specially.
    if (Emu->isArg64Cast(&BC))
        return false;

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(Src);

    Type* V2I32Ty = Emu->getV2Int32Ty();
    Value* NewVal = UndefValue::get(V2I32Ty);
    NewVal = IRB->CreateInsertElement(NewVal, Lo, IRB->getInt32(0));
    NewVal = IRB->CreateInsertElement(NewVal, Hi, IRB->getInt32(1));
    NewVal = IRB->CreateBitCast(NewVal, BC.getType());

    BC.replaceAllUsesWith(NewVal);
    return true;
}

bool InstExpander::visitICmp(ICmpInst& Cmp) {
    if (!Emu->isInt64(Cmp.getOperand(0)))
        return false;

    auto Pred = Cmp.getPredicate();
    Value* L0 = nullptr, * H0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(Cmp.getOperand(0));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(Cmp.getOperand(1));

    Value* T0 = nullptr, * T1 = nullptr, * T2 = nullptr, * T3 = nullptr, * Res = nullptr;
    switch (Pred) {
    default:
        llvm_unreachable("Invalid ICmp predicate");
        break;
    case CmpInst::ICMP_EQ:
        T0 = IRB->CreateICmpEQ(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        Res = IRB->CreateAnd(T1, T0);
        break;
    case CmpInst::ICMP_NE:
        T0 = IRB->CreateICmpNE(L0, L1),
            T1 = IRB->CreateICmpNE(H0, H1);
        Res = IRB->CreateOr(T1, T0);
        break;
    case CmpInst::ICMP_UGT:
        T0 = IRB->CreateICmpUGT(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpUGT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    case CmpInst::ICMP_UGE:
        T0 = IRB->CreateICmpUGE(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpUGT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    case CmpInst::ICMP_ULT:
        T0 = IRB->CreateICmpULT(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpULT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    case CmpInst::ICMP_ULE:
        T0 = IRB->CreateICmpULE(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpULT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    case CmpInst::ICMP_SGT:
        T0 = IRB->CreateICmpUGT(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpSGT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    case CmpInst::ICMP_SGE:
        T0 = IRB->CreateICmpUGE(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpSGT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    case CmpInst::ICMP_SLT:
        T0 = IRB->CreateICmpULT(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpSLT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    case CmpInst::ICMP_SLE:
        T0 = IRB->CreateICmpULE(L0, L1);
        T1 = IRB->CreateICmpEQ(H0, H1);
        T2 = IRB->CreateAnd(T1, T0);
        T3 = IRB->CreateICmpSLT(H0, H1);
        Res = IRB->CreateOr(T2, T3);
        break;
    }
    assert(Res != nullptr);

    Cmp.replaceAllUsesWith(Res);
    return true;
}

bool InstExpander::visitPHI(PHINode& PN) {
    if (!Emu->isInt64(&PN))
        return false;
    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(&PN);
    assert(Lo != nullptr && Hi != nullptr);
    return false;
}

bool InstExpander::visitCall(CallInst& Call) {
    const Function* F = Call.getCalledFunction();

    // lambdas for splitting and combining i64 to <2 x i32>
    auto Combine2xi32Toi64 = [this](Value* val)->Value *
    {
        assert(Emu->isInt64(val));
        Value* InputLo = nullptr, * InputHi = nullptr;
        std::tie(InputLo, InputHi) = Emu->getExpandedValues(val);
        Type* V2I32Ty = Emu->getV2Int32Ty();
        Value* NewVal = UndefValue::get(V2I32Ty);
        NewVal = IRB->CreateInsertElement(NewVal, InputLo, IRB->getInt32(0));
        NewVal = IRB->CreateInsertElement(NewVal, InputHi, IRB->getInt32(1));
        NewVal = IRB->CreateBitCast(NewVal, IRB->getInt64Ty());
        return NewVal;
    };
    auto Spliti64To2xi32 = [this](Value* retVal, Value*& OutputLo, Value*& OutputHi)->void
    {
        assert(Emu->isInt64(retVal));
        Value* V = IRB->CreateBitCast(retVal, Emu->getV2Int32Ty());
        OutputLo = IRB->CreateExtractElement(V, IRB->getInt32(0));
        OutputHi = IRB->CreateExtractElement(V, IRB->getInt32(1));
    };

    if (F && F->isDeclaration()) {
        switch (F->getIntrinsicID()) {
        default:
            break;
            // Ignore the following intrinsics in CG.
        case Intrinsic::assume:
        case Intrinsic::expect:
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        case Intrinsic::var_annotation:
        case Intrinsic::ptr_annotation:
        case Intrinsic::annotation:
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
        case Intrinsic::invariant_start:
        case Intrinsic::invariant_end:
            return false;
        }
    }
    if (!Emu->isInt64(&Call)) {
        for (auto& Op : Call.operands()) {
            if (Emu->isInt64(Op.get()))
                goto Emu64BitCall;
        }
        return false;
    }
Emu64BitCall:
    if (auto * GI = dyn_cast<GenIntrinsicInst>(&Call))
    {
        switch (GI->getIntrinsicID())
        {
        case GenISAIntrinsic::GenISA_getMessagePhaseV:
        case GenISAIntrinsic::GenISA_simdGetMessagePhaseV:
        case GenISAIntrinsic::GenISA_getMessagePhaseX:
        case GenISAIntrinsic::GenISA_getMessagePhaseXV:
        case GenISAIntrinsic::GenISA_getMessagePhase:
        case GenISAIntrinsic::GenISA_broadcastMessagePhase:
        case GenISAIntrinsic::GenISA_broadcastMessagePhaseV:
        case GenISAIntrinsic::GenISA_simdGetMessagePhase:
        case GenISAIntrinsic::GenISA_RuntimeValue:
        case GenISAIntrinsic::GenISA_simdBlockRead:
        {
            auto* GenCopy = Call.clone();
            GenCopy->insertBefore(&Call);
            IRB->SetInsertPoint(&Call);
            Value* Lo = nullptr, * Hi = nullptr;
            Spliti64To2xi32(GenCopy, Lo, Hi);
            Call.replaceAllUsesWith(GenCopy);
            Emu->setExpandedValues(GenCopy, Lo, Hi);
            return true;
        }
        case GenISAIntrinsic::GenISA_intatomicraw:
        case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_intatomicrawA64:
        case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
        {
            auto* GenCopy = Call.clone();
            GenCopy->insertBefore(&Call);
            IRB->SetInsertPoint(GenCopy);

            uint opNum = 0;
            for (auto& Op : Call.operands())
            {
              if (Emu->isInt64(Op.get()))
              {
                Value* NewVal = Combine2xi32Toi64(Op.get());
                GenCopy->setOperand(opNum, NewVal);
              }
              opNum++;
            }
            IRB->SetInsertPoint(&Call);
            Value* Lo = nullptr, * Hi = nullptr;
            Spliti64To2xi32(GenCopy, Lo, Hi);
            Call.replaceAllUsesWith(GenCopy);
            Emu->setExpandedValues(GenCopy, Lo, Hi);
            return true;
        }
        case GenISAIntrinsic::GenISA_simdSetMessagePhaseV:
        case GenISAIntrinsic::GenISA_setMessagePhaseX:
        case GenISAIntrinsic::GenISA_setMessagePhaseXV:
        case GenISAIntrinsic::GenISA_setMessagePhase:
        case GenISAIntrinsic::GenISA_setMessagePhaseV:
        case GenISAIntrinsic::GenISA_simdSetMessagePhase:
        case GenISAIntrinsic::GenISA_setMessagePhaseX_legacy:
        case GenISAIntrinsic::GenISA_itof_rtn:
        case GenISAIntrinsic::GenISA_itof_rtp:
        case GenISAIntrinsic::GenISA_itof_rtz:
        case GenISAIntrinsic::GenISA_uitof_rtn:
        case GenISAIntrinsic::GenISA_uitof_rtp:
        case GenISAIntrinsic::GenISA_uitof_rtz:
        case GenISAIntrinsic::GenISA_simdBlockWrite:
        {
            auto* GenCopy = Call.clone();
            GenCopy->insertBefore(&Call);
            IRB->SetInsertPoint(GenCopy);
            uint opNum = 0;
            for (auto& Op : Call.operands())
            {
                if (Emu->isInt64(Op.get()))
                {
                    Value* NewVal = Combine2xi32Toi64(Op.get());
                    GenCopy->setOperand(opNum, NewVal);
                }
                opNum++;
            }
            Call.replaceAllUsesWith(GenCopy);
            return true;
        }
        case GenISAIntrinsic::GenISA_WaveAll:
        case GenISAIntrinsic::GenISA_WavePrefix:
        {
            auto* GenCopy = Call.clone();
            GenCopy->insertBefore(&Call);
            IRB->SetInsertPoint(GenCopy);

            // bitcast arg from 2xi32 to i64
            Value* NewVal = Combine2xi32Toi64(Call.getArgOperand(0));
            GenCopy->setOperand(0, NewVal);

            // bitcast output from i64 to 2xi32
            IRB->SetInsertPoint(&Call);
            Value* OutputLo = nullptr, * OutputHi = nullptr;
            Spliti64To2xi32(GenCopy, OutputLo, OutputHi);
            Call.replaceAllUsesWith(GenCopy);
            Emu->setExpandedValues(GenCopy, OutputLo, OutputHi);
            return true;
        }
        default:
            break;
        }
    }
    // Support for subroutine calls
    else if (F->hasFnAttribute("UserSubroutine"))
    {
        auto* CallCopy = Call.clone();
        CallCopy->insertBefore(&Call);
        IRB->SetInsertPoint(CallCopy);
        unsigned argNo = 0;
        for (auto& Op : Call.operands())
        {
            if (Emu->isInt64(Op.get()))
            {
                Value* NewVal = Combine2xi32Toi64(Op.get());
                CallCopy->setOperand(argNo, NewVal);
            }
            argNo++;
        }
        if (Emu->isInt64(&Call))
        {
            IRB->SetInsertPoint(&Call);
            Value* OutputLo = nullptr, * OutputHi = nullptr;
            Spliti64To2xi32(CallCopy, OutputLo, OutputHi);
            Emu->setExpandedValues(CallCopy, OutputLo, OutputHi);
        }
        Call.replaceAllUsesWith(CallCopy);
        return true;
    }

    // TODO: Add i64 emulation support.
    llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}

bool InstExpander::visitSelect(SelectInst& SI) {
    if (!Emu->isInt64(&SI))
        return false;

    Value* Cond = SI.getOperand(0);
    Value* H0 = nullptr, * L0 = nullptr;
    std::tie(L0, H0) = Emu->getExpandedValues(SI.getOperand(1));
    Value* L1 = nullptr, * H1 = nullptr;
    std::tie(L1, H1) = Emu->getExpandedValues(SI.getOperand(2));

    Value* Lo = IRB->CreateSelect(Cond, L0, L1);
    Value* Hi = IRB->CreateSelect(Cond, H0, H1);

    Emu->setExpandedValues(&SI, Lo, Hi);
    return true;
}

bool InstExpander::visitVAArg(VAArgInst& VAAI) {
    if (!Emu->isInt64(&VAAI))
        return false;
    // TODO: Add i64 emulation support.
    llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}

bool InstExpander::visitExtractElement(ExtractElementInst& EEI) {
    // Fix index operand if necessary.
    if (Emu->isInt64(EEI.getIndexOperand())) {
        Value* L = nullptr;
        std::tie(L, std::ignore) = Emu->getExpandedValues(EEI.getIndexOperand());
        EEI.setOperand(1, L);
    }

    // Skip if it's not 64-bit integer.
    if (!Emu->isInt64(&EEI))
        return false;

    // NOTE: This is NOT the efficient way to handle that and needs revising
    // later.

    Value* V = EEI.getVectorOperand();
    unsigned NumElts = V->getType()->getVectorNumElements();
    V = IRB->CreateBitCast(V, Emu->getV2Int32Ty(NumElts));
    // Re-calculate indices to Lo and Hi parts.
    Value* Idx = EEI.getIndexOperand();
    Idx = IRB->CreateMul(Idx, IRB->getInt32(2));
    Value* IdxLo = IRB->CreateAdd(Idx, IRB->getInt32(0));
    Value* IdxHi = IRB->CreateAdd(Idx, IRB->getInt32(1));
    Value* Lo = IRB->CreateExtractElement(V, IdxLo);
    Value* Hi = IRB->CreateExtractElement(V, IdxHi);

    Emu->setExpandedValues(&EEI, Lo, Hi);
    return true;
}

bool InstExpander::visitInsertElement(InsertElementInst& IEI) {
    // Fix index operand if necessary.
    if (Emu->isInt64(IEI.getOperand(2))) {
        Value* L = nullptr;
        std::tie(L, std::ignore) = Emu->getExpandedValues(IEI.getOperand(2));
        IEI.setOperand(2, L);
    }

    // Skip if it's not 64-bit integer.
    Value* V = IEI.getOperand(1);
    if (!Emu->isInt64(V))
        return false;

    // NOTE: This is NOT the efficient way to handle that and needs revising
    // later.

    Value* Lo = nullptr, * Hi = nullptr;
    std::tie(Lo, Hi) = Emu->getExpandedValues(V);

    // Create the emulated vector.
    Value* NewVal = IEI.getOperand(0);
    unsigned NumElts = NewVal->getType()->getVectorNumElements();
    NewVal = IRB->CreateBitCast(NewVal, Emu->getV2Int32Ty(NumElts));
    // Re-calculate indices to Lo and Hi parts.
    Value* Idx = IEI.getOperand(2);
    Idx = IRB->CreateMul(Idx, IRB->getInt32(2));
    Value* IdxLo = IRB->CreateAdd(Idx, IRB->getInt32(0));
    Value* IdxHi = IRB->CreateAdd(Idx, IRB->getInt32(1));
    // Insert Lo and Hi parts into the emulated vector.
    NewVal = IRB->CreateInsertElement(NewVal, Lo, IdxLo);
    NewVal = IRB->CreateInsertElement(NewVal, Hi, IdxHi);
    NewVal = IRB->CreateBitCast(NewVal, IEI.getType());

    IEI.replaceAllUsesWith(NewVal);
    return true;
}

bool InstExpander::visitExtractValue(ExtractValueInst& EVI) {
    if (!Emu->isInt64(&EVI))
        return false;
    // TODO: Add i64 emulation support.
    llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}

bool InstExpander::visitInsertValue(InsertValueInst& IVI) {
    if (!Emu->isInt64(IVI.getOperand(1)))
        return false;
    // TODO: Add i64 emulation support.
    llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}

bool InstExpander::visitLandingPad(LandingPadInst& LPI) {
    if (!Emu->isInt64(LPI.getOperand(1)))
        return false;
    // TODO: Add i64 emulation support.
    llvm_unreachable("TODO: NOT IMPLEMENTED YET!");
    return false;
}
