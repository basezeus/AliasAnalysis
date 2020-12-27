#include "FlowSensitiveAliasAnalysis.h"
#include "AliasBench/Benchmark.h"
#include "AliasGraph/AliasGraph.h"
#include "AliasToken/Alias.h"
#include "AliasToken/AliasToken.h"
#include "CFGUtils/CFGUtils.h"
#include "Worklist/Worklist.h"
#include "iostream"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "map"

using namespace llvm;
using AliasMap = AliasGraphUtil::AliasGraph<AliasUtil::Alias>;

class PointsToAnalysis {
   private:
    AliasMap GlobalAliasMap;
    std::map<Instruction*, AliasMap> AliasIn, AliasOut;
    AliasUtil::AliasTokens AT;
    BenchmarkUtil::BenchmarkRunner Bench;
    WorklistUtil::Worklist WL;

   public:
    PointsToAnalysis(Module& M) : WL(&M) { handleGlobalVar(M); }
    void handleGlobalVar(llvm::Module& M) {
        // Handle global variables
        for (auto& G : M.getGlobalList()) {
            auto Aliases = AT.extractAliasToken(&G);
            auto Redirections = AT.extractStatementType(&G);
            if (Aliases.size() == 2) {
                GlobalAliasMap.insert(Aliases[0], Aliases[1],
                                      Redirections.first, Redirections.second);
                // Handle the case when a global variable is initialized with an
                // address
                if (llvm::GlobalVariable* Constant =
                        llvm::dyn_cast<GlobalVariable>(G.getInitializer())) {
                    GlobalAliasMap.insert(Aliases[0],
                                          AT.getAliasToken(Constant), 2, 1);
                }
            }
        }
    }
    void runOnWorkList() {
        while (!WL.empty()) {
            Instruction* Inst = WL.pop();
            AliasMap OldAliasInfo = AliasOut[Inst];
            runAnalysis(Inst);
            AliasMap NewAliasInfo = AliasOut[Inst];
            if (!(OldAliasInfo == NewAliasInfo)) {
                for (Instruction* I : CFGUtils::GetSucc(Inst)) {
                    WL.push(I);
                }
            }
        }
    }
    void runAnalysis(llvm::Instruction* Inst) {
        llvm::BasicBlock* ParentBB = Inst->getParent();
        llvm::Function* ParentFunc = ParentBB->getParent();
        std::vector<AliasMap> Predecessors;
        // Handle function arguments
        AliasMap ArgAliasMap;
        for (auto Arg = ParentFunc->arg_begin(); Arg != ParentFunc->arg_end();
             Arg++) {
            auto Aliases = AT.extractAliasToken(Arg, ParentFunc);
            if (Aliases.size() == 2)
                ArgAliasMap.insert(Aliases[0], Aliases[1], 1, 0);
        }
        // Only calculate aliases for global variables and arguments at
        // the
        // start of the function
        if (&ParentBB->front() == Inst) {
            Predecessors.push_back(GlobalAliasMap);
            Predecessors.push_back(ArgAliasMap);
        }
        // Calculate control flow predecessor
        for (Instruction* I : CFGUtils::GetPred(Inst)) {
            if (AliasOut.find(I) != AliasOut.end())
                Predecessors.push_back(AliasOut[I]);
        }
        AliasIn[Inst].merge(Predecessors);
        AliasOut[Inst] = AliasIn[Inst];
        // Extract alias tokens from the instruction
        auto Aliases = AT.extractAliasToken(Inst);
        // Handle killing
        if (StoreInst* SI = dyn_cast<StoreInst>(Inst)) {
            if (Aliases.size() == 2) {
                auto Pointee = AliasOut[Inst].getPointee(Aliases[0]);
                if (Pointee.size() == 1) {
                    auto KillNode = *(Pointee.begin());
                    AliasOut[Inst].erase(KillNode);
                }
            }
        }
        // Handle special cases:
        // Handle GEP instructions
        if (GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(Inst)) {
            for (auto A : AliasOut[Inst].getPointee(Aliases[1])) {
                AliasUtil::Alias* FieldVal = new AliasUtil::Alias(A);
                FieldVal->setIndex(GEP);
                FieldVal = AT.getAliasToken(FieldVal);
                AliasOut[Inst].insert(Aliases[0], FieldVal, 1, 0);
            }
            // clear Aliases to avoid confusions
            Aliases.clear();
        }
        // handle function call
        if (CallInst* CI = dyn_cast<CallInst>(Inst)) {
            if (!CI->isIndirectCall()) {
                Function& Func = *CI->getCalledFunction();
                if (!CFGUtils::SkipFunction(Func)) {
                    // pass alias information
                    AliasIn[&(Func.front().front())].merge(
                        std::vector<AliasMap>{AliasIn[Inst]});
                    // handle return value
                    if (!CI->doesNotReturn()) {
                        if (ReturnInst* RI =
                                dyn_cast<ReturnInst>(&(Func.back().back()))) {
                            auto CallAliases = AT.extractAliasToken(RI);
                            if (CallAliases.size() == 1) {
                                AliasOut[&(Func.back().back())].insert(
                                    Aliases[0], CallAliases[0], 1, 1);
                            }
                        }
                    }
                    // handle pass by reference
                    int ArgNum = 0;
                    for (Value* Arg : CI->args()) {
                        AliasUtil::Alias* ActualArg =
                            AT.getAliasToken(new AliasUtil::Alias(Arg));
                        AliasUtil::Alias* FormalArg = AT.getAliasToken(
                            new AliasUtil::Alias(Func.getArg(ArgNum)));
                        AliasIn[&(Func.front().front())].insert(
                            FormalArg, ActualArg, 1, 1);
                        ArgNum += 1;
                    }
                    // handle change made to globals
                    for (auto P : AliasOut[&Func.back().back()]) {
                        if (!P.first->sameFunc(&Func)) {
                            AliasOut[Inst].insert(P.first, P.second);
                        }
                    }
                }
            }
        }
        // Find the relative redirection between lhs and rhs
        // example for a = &b:(1, 0)
        auto Redirections = AT.extractStatementType(Inst);
        if (Aliases.size() == 2) {
            // Default behavior is copy ie (1, 1)
            // for heap address in RHS make sure it is (x, 0)
            if (Aliases[1]->isMem()) Redirections.second = 0;
            AliasOut[Inst].insert(Aliases[0], Aliases[1], Redirections.first,
                                  Redirections.second);
        }
        // Evaluate precision
        auto BenchVar = Bench.extract(Inst);
        if (BenchVar.size() == 2) {
            Bench.evaluate(
                Inst, AliasOut[Inst].getPointee(AT.getAliasToken(BenchVar[0])),
                AliasOut[Inst].getPointee(AT.getAliasToken(BenchVar[1])));
        }
    }
    void printResults(llvm::Module& M) {
        for (Function& F : M.functions()) {
            for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;
                 ++I) {
                std::cout << AliasIn[&*I];
                llvm::errs() << "\n[Instruction] " << *I << "\n\n";
                std::cout << AliasOut[&*I];
                std::cout << "----------- \n";
            }
        }
        std::cout << Bench;
    }
};

bool FlowSensitiveAliasAnalysisPass::runOnModule(Module& M) {
    for (Function& F : M.functions()) {
        CFGUtils::InstNamer(F);
    }
    PointsToAnalysis PA(M);
    PA.runOnWorkList();
    PA.printResults(M);
    return false;
}

char FlowSensitiveAliasAnalysisPass::ID = 0;
static RegisterPass<FlowSensitiveAliasAnalysisPass> X(
    "aa-fs", "Implementation of flow-sensitive alias analysis in LLVM", true,
    true);
