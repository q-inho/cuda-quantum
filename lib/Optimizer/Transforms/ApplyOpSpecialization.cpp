/*************************************************************** -*- C++ -*- ***
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 *******************************************************************************/

#include "PassDetails.h"
#include "cudaq/Optimizer/Builder/Factory.h"
#include "cudaq/Optimizer/Dialect/Characteristics.h"
#include "cudaq/Optimizer/Transforms/Passes.h"
#include "cudaq/Todo.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"

#define DEBUG_TYPE "quake-apply-rewrite"

using namespace mlir;

namespace {
/// A quake.apply can indicate any of the following: a regular call to a
/// Callable (kernel), a call to a variant of a Callable with some control
/// qubits, a call to a variant of a Callable in adjoint form, or a call to a
/// Callable that is both adjoint and has control qubits.
struct ApplyVariants {
  bool needsControlVariant : 1;
  bool needsAdjointVariant : 1;
  bool needsAdjointControlVariant : 1;
};

/// Map from `func::FuncOp` to the variants to be created.
using ApplyOpAnalysisInfo = DenseMap<Operation *, ApplyVariants>;

/// This analysis scans the IR for `ApplyOp`s to see which ones need to have
/// variants created.
struct ApplyOpAnalysis {
  ApplyOpAnalysis(ModuleOp op) : module(op) {
    performAnalysis(op.getOperation());
  }

  const ApplyOpAnalysisInfo &getAnalysisInfo() const { return infoMap; }

private:
  void performAnalysis(Operation *op) {
    op->walk([&](quake::ApplyOp appOp) {
      if (!appOp.applyToVariant())
        return;
      auto callee = appOp.getCallee();
      auto funcOp = module.lookupSymbol<func::FuncOp>(callee);
      auto *fnOp = funcOp.getOperation();
      ApplyVariants variant;
      auto it = infoMap.find(fnOp);
      if (it != infoMap.end())
        variant = it->second;
      if (appOp.getIsAdj() && !appOp.getControls().empty())
        variant.needsAdjointControlVariant = true;
      else if (appOp.getIsAdj())
        variant.needsAdjointVariant = true;
      else if (!appOp.getControls().empty())
        variant.needsControlVariant = true;
      infoMap.insert(std::make_pair(fnOp, variant));
    });
  }

  ModuleOp module;
  ApplyOpAnalysisInfo infoMap;
};
} // namespace

static std::string getAdjCtrlVariantFunctionName(const std::string &n) {
  return n + ".adj.ctrl";
}

static std::string getAdjVariantFunctionName(const std::string &n) {
  return n + ".adj";
}

static std::string getCtrlVariantFunctionName(const std::string &n) {
  return n + ".ctrl";
}

static std::string getVariantFunctionName(quake::ApplyOp appOp,
                                          const std::string &calleeName) {
  if (appOp.getIsAdj() && !appOp.getControls().empty())
    return getAdjCtrlVariantFunctionName(calleeName);
  if (appOp.getIsAdj())
    return getAdjVariantFunctionName(calleeName);
  if (!appOp.getControls().empty())
    return getCtrlVariantFunctionName(calleeName);
  return calleeName;
}

// We expect the loop control value to have the following form.
//
//   %final = cc.loop while ((%iter = %initial) -> (iN)) {
//     ...
//     %cond = arith.cmpi {<.<=,!=,>=,>}, %iter, %bound : iN
//     cc.condition %cond (%iter : iN)
//   } do {
//    ^bb1(%iter : iN):
//     ...
//     cc.continue %iter : iN
//   } step {
//    ^bb2(%iter : iN):
//     ...
//     %next = arith.{addi,subi} %iter, %step : iN
//     cc.continue %next : iN
//   }
//
// with the additional requirement that none of the `...` sections can modify
// the value of `%bound` or `%step`. Those values are invariant if there are
// no side-effects in the loop Op (no store or call operations) and these values
// do not depend on a block argument.
// FIXME: assumes only the LCV is passed as a Value.
static bool hasMonotonicPHIControl(cudaq::cc::LoopOp loop) {
  if (loop.getInitArgs().empty() || loop.getResults().empty())
    return false;
  auto &whileBlock = loop.getWhileRegion().back();
  auto condition = dyn_cast<cudaq::cc::ConditionOp>(whileBlock.back());
  if (!condition || whileBlock.getArguments()[0] != condition.getResults()[0])
    return false;
  auto *cmpOp = condition.getCondition().getDefiningOp();
  if (std::find(cmpOp->getOperands().begin(), cmpOp->getOperands().end(),
                whileBlock.getArguments()[0]) == cmpOp->getOperands().end())
    return false;
  auto &bodyBlock = loop.getBodyRegion().back();
  auto bodyTermOp = dyn_cast<cudaq::cc::ContinueOp>(bodyBlock.back());
  if (!bodyTermOp || (bodyBlock.getArguments()[0] != bodyTermOp.getOperand(0)))
    return false;
  auto &stepBlock = loop.getStepRegion().back();
  auto backedgeOp = dyn_cast<cudaq::cc::ContinueOp>(stepBlock.back());
  if (!backedgeOp)
    return false;
  auto *mutateOp = backedgeOp.getOperand(0).getDefiningOp();
  if (!isa<arith::AddIOp, arith::SubIOp>(mutateOp) ||
      std::find(mutateOp->getOperands().begin(), mutateOp->getOperands().end(),
                stepBlock.getArguments()[0]) == mutateOp->getOperands().end())
    return false;
  // FIXME: should verify %bound, %step are loop invariant.
  return true;
}

// From the comparison Op in the while block, gather a list of all the scalar
// temporaries that are referenced. One of these should be the induction
// variable that controls the loop.
static SmallVector<Operation *> populateComparisonTemps(Operation *cmpOp,
                                                        Block &whileBlock) {
  SmallVector<Operation *> results;
  SmallVector<Operation *> worklist = {cmpOp};
  do {
    auto *op = worklist.back();
    worklist.pop_back();
    if (auto loadOp = dyn_cast<memref::LoadOp>(op)) {
      auto *defOp = loadOp.getMemRef().getDefiningOp();
      if (auto alloc = dyn_cast_or_null<memref::AllocaOp>(defOp)) {
        auto memrefTy = alloc.getType();
        // Induction must be a scalar integral type.
        if (memrefTy.getShape().empty() &&
            memrefTy.getElementType().isa<IntegerType>())
          results.push_back(defOp);
      }
    } else {
      for (auto val : op->getOperands())
        if (auto *def = val.getDefiningOp();
            def && def->getBlock() == &whileBlock)
          worklist.push_back(def);
    }
  } while (!worklist.empty());
  return results;
}

// We expect the loop control value to have the following form.
//
//   cc.loop while {
//     ...
//     %0 = memref.load %iter[] : memref<iN>
//     %1 = arith.cmpi {<,<=,!=,>=,>}, %0, %bound : iN
//     cc.condition %1
//   } do {
//     ...
//   } step {
//     ...
//     %0 = memref.load %iter[] : memref<iN>
//     %1 = arith.{addi,subi} %0, %step : iN
//     memref.store %1, %iter[] : memref<iN>
//   }
//
// with the additional requirement that none of the `...` sections can modify
// the value of `%bound` or `%step`. Those values are invariant if there are
// no side-effects in the loop Op (no store or call operations) and these values
// do not depend on a block argument.
static bool hasMonotonicLCV(cudaq::cc::LoopOp loop) {
  if (!loop.getInitArgs().empty() && !loop.getResults().empty())
    return false;
  auto &whileBlock = loop.getWhileRegion().back();
  auto condition = dyn_cast<cudaq::cc::ConditionOp>(whileBlock.back());
  if (!condition)
    return false;
  auto *cmpOp = condition.getCondition().getDefiningOp();
  auto compare = dyn_cast_or_null<arith::CmpIOp>(cmpOp);
  if (!compare)
    return false;
  // Collect any loads for the expressions into compare in the while region.
  SmallVector<Operation *> comparisonTemps =
      populateComparisonTemps(cmpOp, whileBlock);
  auto &stepBlock = loop.getStepRegion().back();
  // Search loads in step region. Exactly one must match that in the while
  // region and be mutated by a store to itself.
  auto matchedWhileVariable = [&]() {
    unsigned count = 0;
    for (auto &op : llvm::reverse(stepBlock))
      if (auto storeOp = dyn_cast<memref::StoreOp>(op))
        if (std::find(comparisonTemps.begin(), comparisonTemps.end(),
                      storeOp.getMemRef().getDefiningOp()) !=
            comparisonTemps.end())
          if (auto *def = storeOp.getValue().getDefiningOp())
            if (isa<arith::AddIOp, arith::SubIOp>(def))
              for (auto defOpnd : def->getOperands()) // exactly 2
                if (auto loadOp =
                        dyn_cast<memref::LoadOp>(defOpnd.getDefiningOp()))
                  if (storeOp.getMemRef().getDefiningOp() ==
                      loadOp.getMemRef().getDefiningOp())
                    ++count;
    return count == 1;
  }();
  if (!matchedWhileVariable)
    return false;
  // FIXME: should verify %bound, %step are loop invariant.
  return true;
}

// Check that there is a lcv for the loop and that the generated function is
// monotonic and constant slope.
// Check for either a closed form value passed as a block argument via the
// backedge of the loop (as from mem2reg) or a memory-bound variable.
static bool hasMonotonicControlInduction(cudaq::cc::LoopOp loop) {
  return hasMonotonicLCV(loop) || hasMonotonicPHIControl(loop);
}

// A counted loop is defined to be a loop that will execute some bounded number
// of iterations that can be predetermined before the loop, in fact, executes. A
// loop such as `for(i = 0; i < n; ++i)` is a counted loop that must execute
//   n : if n > 0
//   0 : if n <= 0
// iterations. Early exits (break statements) are not permitted.
static bool isaCountedLoop(Operation &op) {
  if (auto loopOp = dyn_cast<cudaq::cc::LoopOp>(op)) {
    // Cannot be a `while` or `do while` loop.
    if (loopOp.isPostConditional() || !loopOp.hasStep())
      return false;
    auto &reg = loopOp.getBodyRegion();
    // This is a `for` loop and must have a body with a continue terminator.
    // Currently, only a single basic block is allowed to keep things simple.
    // This is in keeping with our definition of structured control flow.
    return !reg.empty() && reg.hasOneBlock() &&
           isa<cudaq::cc::ContinueOp>(reg.front().getTerminator()) &&
           hasMonotonicControlInduction(loopOp);
  }
  return false;
}

// Returns true if this region contains unstructured control flow. Branches
// between basic blocks in a Region are defined to be unstructured. A Region
// with a single Block which contains cc.scope, cc.loop and cc.if, which
// themselves contain single Blocks recursively, will be considered structured.
// FIXME: Limitation: at present, the compiler does not recover structured
// control flow from a primitive CFG.
static bool regionHasUnstructuredControlFlow(Region &region) {
  if (region.empty())
    return false;
  if (!region.hasOneBlock())
    return true;
  auto &block = region.front();
  for (auto &op : block) {
    if (op.getNumRegions() == 0)
      continue;
    if (!isa<cudaq::cc::IfOp>(op) && !isaCountedLoop(op) &&
        op.getNumRegions() > 1)
      return true; // Op has multiple regions but is not a known Op.
    for (auto &reg : op.getRegions())
      if (regionHasUnstructuredControlFlow(reg))
        return true;
  }
  return false;
}

namespace {
/// Replace an apply op with a call to the correct variant function.
struct ApplyOpPattern : public OpRewritePattern<quake::ApplyOp> {
  using Base = OpRewritePattern<quake::ApplyOp>;
  using Base::Base;

  LogicalResult matchAndRewrite(quake::ApplyOp appOp,
                                PatternRewriter &rewriter) const override {
    auto calleeName = getVariantFunctionName(
        appOp, appOp.getCallee().getRootReference().str());
    auto *ctx = appOp.getContext();
    auto consTy = quake::QVecType::getUnsized(ctx);
    SmallVector<Value> newArgs;
    if (!appOp.getControls().empty()) {
      auto consOp = rewriter.create<quake::ConcatOp>(appOp.getLoc(), consTy,
                                                     appOp.getControls());
      newArgs.push_back(consOp);
    }
    newArgs.append(appOp.getArgs().begin(), appOp.getArgs().end());
    rewriter.replaceOpWithNewOp<func::CallOp>(appOp, appOp.getResultTypes(),
                                              calleeName, newArgs);
    return success();
  }
};

class ApplySpecializationPass
    : public cudaq::opt::ApplySpecializationBase<ApplySpecializationPass> {
public:
  void runOnOperation() override {
    ApplyOpAnalysis analysis(getOperation());
    const auto &applyVariants = analysis.getAnalysisInfo();
    step1(applyVariants);
    step2();
  }

  /// Step 1. Instantiate all the implied variants of functions from all
  /// quake.apply operations that were found.
  void step1(const ApplyOpAnalysisInfo &applyVariants) {
    ModuleOp module = getOperation();

    // Loop over all the globals in the module.
    for (auto &global : *module.getBody()) {
      auto variantIter = applyVariants.find(&global);
      if (variantIter == applyVariants.end())
        continue;

      // Found a FuncOp that needs to be specialized.
      auto funcOp = dyn_cast<func::FuncOp>(global);
      assert(funcOp && "global must be a FuncOp");
      auto &variant = variantIter->second;

      if (variant.needsControlVariant)
        createControlVariantOf(funcOp);
      if (variant.needsAdjointVariant)
        createAdjointVariantOf(
            funcOp, getAdjVariantFunctionName(funcOp.getName().str()));
      if (variant.needsAdjointControlVariant)
        createAdjointControlVariantOf(funcOp);
    }
  }

  func::FuncOp createControlVariantOf(func::FuncOp funcOp) {
    ModuleOp module = getOperation();
    auto *ctx = module.getContext();
    auto funcName = getCtrlVariantFunctionName(funcOp.getName().str());
    auto funcTy = funcOp.getFunctionType();
    auto qvecTy = quake::QVecType::getUnsized(ctx);
    auto loc = funcOp.getLoc();
    SmallVector<Type> inTys = {qvecTy};
    inTys.append(funcTy.getInputs().begin(), funcTy.getInputs().end());
    auto newFunc = cudaq::opt::factory::createFunction(
        funcName, funcTy.getResults(), inTys, module);
    newFunc.setPrivate();
    IRMapping mapping;
    funcOp.getBody().cloneInto(&newFunc.getBody(), mapping);
    auto newCond = newFunc.getBody().front().insertArgument(0u, qvecTy, loc);
    newFunc.walk([&](Operation *op) {
      if (op->hasTrait<cudaq::QuantumGate>()) {
        // This is a quantum op. It should be updated with an additional control
        // argument, `newCond`.
        OpBuilder builder(op);
        auto arrAttr = op->getAttr(segmentSizes).cast<DenseI32ArrayAttr>();
        SmallVector<Value> operands(op->getOperands().begin(),
                                    op->getOperands().begin() + arrAttr[0]);
        operands.push_back(newCond);
        operands.append(op->getOperands().begin() + arrAttr[0],
                        op->getOperands().end());
        auto newArrAttr = DenseI32ArrayAttr::get(
            ctx, {arrAttr[0], arrAttr[1] + 1, arrAttr[2]});
        NamedAttrList attrs(op->getAttrs());
        attrs.set(segmentSizes, newArrAttr);
        OperationState res(op->getLoc(), op->getName().getStringRef(), operands,
                           op->getResultTypes(), attrs);
        builder.create(res); // Quake quantum gates have no results
        op->erase();
      }
    });
    return newFunc;
  }

  /// The adjoint variant of the function is the "reverse" computation. We want
  /// to reverse the flow graph so the gates appear "upside down".
  func::FuncOp createAdjointVariantOf(func::FuncOp funcOp,
                                      std::string &&funcName) {
    ModuleOp module = getOperation();
    auto loc = funcOp.getLoc();
    auto &funcBody = funcOp.getBody();

    // Check our restrictions.
    if (regionHasUnstructuredControlFlow(funcBody)) {
      emitError(loc,
                "cannot make adjoint of kernel with unstructured control flow");
      signalPassFailure();
      return {};
    }
    if (cudaq::opt::hasCallOp(funcOp)) {
      emitError(loc, "cannot make adjoint of kernel with calls");
      signalPassFailure();
      return {};
    }
    if (cudaq::opt::internal::hasCharacteristic(
            [](Operation &op) {
              return isa<cudaq::cc::CreateLambdaOp,
                         cudaq::cc::InstantiateCallableOp>(op);
            },
            *funcOp.getOperation())) {
      emitError(loc, "cannot make adjoint of kernel with callable expressions");
      signalPassFailure();
      return {};
    }
    if (cudaq::opt::hasMeasureOp(funcOp)) {
      emitError(loc, "cannot make adjoint of kernel with a measurement");
      signalPassFailure();
      return {};
    }

    auto funcTy = funcOp.getFunctionType();
    auto newFunc = cudaq::opt::factory::createFunction(
        funcName, funcTy.getResults(), funcTy.getInputs(), module);
    newFunc.setPrivate();
    IRMapping mapping;
    funcBody.cloneInto(&newFunc.getBody(), mapping);
    reverseTheOpsInTheBlock(loc, newFunc.getBody().front().getTerminator(),
                            getOpsToInvert(newFunc.getBody().front()));
    return newFunc;
  }

  static SmallVector<Operation *> getOpsToInvert(Block &block) {
    SmallVector<Operation *> ops;
    for (auto &op : block)
      if (cudaq::opt::hasQuantum(op))
        ops.push_back(&op);
    return ops;
  }

  static Value cloneRootSubexpression(OpBuilder &builder, Block &block,
                                      Value root) {
    if (auto *op = root.getDefiningOp())
      if (op->getBlock() == &block) {
        for (Value v : op->getOperands())
          cloneRootSubexpression(builder, block, v);
        return builder.clone(*op)->getResult(0);
      }
    return root;
  }

  // Build an arith.constant Op for an integral type (including index).
  static Value createIntConstant(OpBuilder &builder, Location loc, Type ty,
                                 std::int64_t val) {
    auto attr = builder.getIntegerAttr(ty, val);
    return builder.create<arith::ConstantOp>(loc, attr, ty);
  }

  /// Clone the LoopOp, \p loop, and return a new LoopOp that runs the loop
  /// backwards. The loop is assumed to be a simple counted loop (a generator of
  /// a monotonic indexing function). The loop control could be in either the
  /// memory or value domain. The step and bounds of the original loop must be
  /// loop invariant.
  static cudaq::cc::LoopOp cloneReversedLoop(OpBuilder &builder,
                                             cudaq::cc::LoopOp loop) {
    auto loc = loop.getLoc();
    // Recover the different subexpressions from the loop. Given:
    //
    //   for (int i = A; i `cmp` B; i = i `bump` C) ...
    //
    // Get references to each of: `i`, A, B, C, `cmp`, and `bump` regardless of
    // the loop structure.
    bool inductionIsValue = hasMonotonicPHIControl(loop);
    auto &whileRegion = loop.getWhileRegion();
    auto condOp = cast<cudaq::cc::ConditionOp>(whileRegion.back().back());
    auto cmpOp = cast<arith::CmpIOp>(condOp.getCondition().getDefiningOp());
    auto pair0 = [&]() -> std::pair<Operation *, Operation *> {
      if (!inductionIsValue) {
        auto comparisonTemps =
            populateComparisonTemps(cmpOp, whileRegion.back());
        for (auto &op : llvm::reverse(loop.getStepRegion().back())) {
          if (auto storeOp = dyn_cast<memref::StoreOp>(op)) {
            auto *storeTo = storeOp.getMemRef().getDefiningOp();
            if (std::find(comparisonTemps.begin(), comparisonTemps.end(),
                          storeTo) != comparisonTemps.end())
              return {storeTo, storeOp.getValue().getDefiningOp()};
          }
        }
      }
      return {};
    }();
    auto inductionVar{pair0.first};
    auto stepOp{pair0.second};
    Value initialValue =
        inductionIsValue
            ? loop.getInitArgs()[0]
            : builder.create<memref::LoadOp>(loc, inductionVar->getResult(0));
    auto inductionOnLhs = [&](auto binOp) -> Value {
      if (auto load = dyn_cast<memref::LoadOp>(binOp.getLhs().getDefiningOp()))
        if (load.getMemRef().getDefiningOp() == inductionVar)
          return binOp.getRhs();
      return {};
    };
    auto oppositeOfInduction = [&](auto binOp) -> Value {
      if (auto result = inductionOnLhs(binOp))
        return result;
      [[maybe_unused]] auto load =
          dyn_cast<memref::LoadOp>(binOp.getRhs().getDefiningOp());
      assert(load && load.getMemRef().getDefiningOp() == inductionVar);
      return binOp.getLhs();
    };
    Value terminalValue = [&]() {
      if (inductionIsValue) {
        if (cmpOp.getLhs() == loop.getWhileRegion().front().getArgument(0))
          return cmpOp.getRhs();
        assert(cmpOp.getRhs() == loop.getWhileRegion().front().getArgument(0));
        return cmpOp.getLhs();
      }
      return oppositeOfInduction(cmpOp);
    }();
    auto trip0 = [&]() -> std::tuple<Value, bool, bool> {
      if (inductionIsValue) {
        auto contOp =
            dyn_cast<cudaq::cc::ContinueOp>(loop.getStepRegion().back().back());
        stepOp = contOp.getOperand(0).getDefiningOp();
        if (auto addOp = dyn_cast<arith::AddIOp>(stepOp)) {
          if (addOp.getLhs() == loop.getStepRegion().back().getArgument(0))
            return {addOp.getRhs(), true, false};
          assert(addOp.getRhs() == loop.getStepRegion().back().getArgument(0));
          return {addOp.getLhs(), true, true};
        }
        auto subOp = cast<arith::SubIOp>(stepOp);
        return {subOp.getRhs(), false, false};
      }
      if (auto addOp = dyn_cast<arith::AddIOp>(stepOp)) {
        auto stepVal = oppositeOfInduction(addOp);
        return {stepVal, true, addOp.getLhs() == stepVal};
      }
      auto subOp = cast<arith::SubIOp>(stepOp);
      auto result = inductionOnLhs(subOp);
      assert(result && "induction variable expected on lhs of subtraction");
      return {result, false, false};
    }();
    auto stepValue{std::get<0>(trip0)};
    auto stepIsAnAddOp{std::get<1>(trip0)};
    auto commuteTheAddOp{std::get<2>(trip0)};

    // Now rewrite the loop to run in reverse. `builder` is set at the point we
    // want to insert the new loop.
    Value newTermVal = cloneRootSubexpression(
        builder, loop.getWhileRegion().back(), terminalValue);
    Value newStepVal =
        cloneRootSubexpression(builder, loop.getStepRegion().back(), stepValue);
    auto zero = createIntConstant(builder, loc, newStepVal.getType(), 0);
    if (!stepIsAnAddOp) {
      // Negate the step value when arith.subi.
      newStepVal = builder.create<arith::SubIOp>(loc, zero, newStepVal);
    }
    Value iters = builder.create<arith::SubIOp>(loc, newTermVal, initialValue);
    auto pred = cmpOp.getPredicate();
    // FIXME: This assumes the unsigned value range, if used, for the loop fits
    // within the signed value range of the type of the induction.
    if (pred == arith::CmpIPredicate::ule ||
        pred == arith::CmpIPredicate::sle ||
        pred == arith::CmpIPredicate::uge || pred == arith::CmpIPredicate::sge)
      iters = builder.create<arith::AddIOp>(loc, iters, newStepVal);
    iters = builder.create<arith::DivSIOp>(loc, iters, newStepVal);
    Value noLoopCond = builder.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::sgt, iters, zero);
    iters = builder.create<arith::SelectOp>(loc, iters.getType(), noLoopCond,
                                            iters, zero);
    auto one = createIntConstant(builder, loc, iters.getType(), 1);
    Value adjustIters = builder.create<arith::SubIOp>(loc, iters, one);
    Value nStep = builder.create<arith::MulIOp>(loc, adjustIters, newStepVal);
    Value newInitVal = builder.create<arith::AddIOp>(loc, initialValue, nStep);

    // Create the list of input arguments to loop. We're going to add an
    // argument to the end that is the number of iterations left to execute.
    SmallVector<Value> inputs;
    if (inductionIsValue)
      inputs.push_back(newInitVal);
    else
      builder.create<memref::StoreOp>(loc, newInitVal,
                                      inductionVar->getResult(0));
    inputs.push_back(iters);

    // Create the new LoopOp. This requires threading the new value that is the
    // number of iterations left to execute. In the whileRegion, update the
    // condition test to use the new argument. In the bodyRegion, update to pass
    // through the new argument. In the stepRegion, decrement the new argument
    // by 1 and convert the original step expression to be a negative step.
    IRRewriter rewriter(builder);
    return rewriter.create<cudaq::cc::LoopOp>(
        loc, ValueRange{inputs}.getTypes(), inputs, /*postCondition=*/false,
        [&](OpBuilder &builder, Location loc, Region &region) {
          IRMapping dummyMap;
          loop.getWhileRegion().cloneInto(&region, dummyMap);
          Block &entry = region.front();
          entry.addArgument(iters.getType(), loc);
          Block &block = region.back();
          auto condOp = cast<cudaq::cc::ConditionOp>(block.back());
          IRRewriter rewriter(builder);
          rewriter.setInsertionPoint(condOp);
          SmallVector<Value> args = condOp.getResults();
          Value trip = block.getArguments().back();
          args.push_back(trip);
          auto zero = createIntConstant(builder, loc, trip.getType(), 0);
          auto newCond = rewriter.create<arith::CmpIOp>(
              loc, arith::CmpIPredicate::sgt, trip, zero);
          rewriter.replaceOpWithNewOp<cudaq::cc::ConditionOp>(condOp, newCond,
                                                              args);
        },
        [&](OpBuilder &builder, Location loc, Region &region) {
          IRMapping dummyMap;
          loop.getBodyRegion().cloneInto(&region, dummyMap);
          Block &entry = region.front();
          entry.addArgument(iters.getType(), loc);
          auto &term = region.back().back();
          IRRewriter rewriter(builder);
          rewriter.setInsertionPoint(&term);
          SmallVector<Value> args(entry.getArguments().begin(),
                                  entry.getArguments().end());
          rewriter.replaceOpWithNewOp<cudaq::cc::ContinueOp>(&term, args);
        },
        [&](OpBuilder &builder, Location loc, Region &region) {
          IRMapping dummyMap;
          if (!inductionIsValue) {
            // In memory case, create the new op before doing the clone and
            // before we lose track of which op is the step op.
            OpBuilder::InsertionGuard guard(builder);
            builder.setInsertionPoint(stepOp);
            IRRewriter rewriter(builder);
            if (stepIsAnAddOp)
              rewriter.replaceOpWithNewOp<arith::SubIOp>(
                  stepOp, stepOp->getOperand(commuteTheAddOp ? 1 : 0),
                  stepOp->getOperand(commuteTheAddOp ? 0 : 1));
            else
              rewriter.replaceOpWithNewOp<arith::AddIOp>(
                  stepOp, stepOp->getOperand(0), stepOp->getOperand(1));
          }
          loop.getStepRegion().cloneInto(&region, dummyMap);
          Block &entry = region.front();
          entry.addArgument(iters.getType(), loc);
          auto contOp = cast<cudaq::cc::ContinueOp>(region.back().back());
          IRRewriter rewriter(builder);
          rewriter.setInsertionPoint(contOp);
          SmallVector<Value> args;
          if (inductionIsValue) {
            // In the value case, replace after the clone since we need to
            // thread the new value and it's trivial to find the stepOp.
            auto *stepOp = contOp.getOperand(0).getDefiningOp();
            auto newBump = [&]() -> Value {
              if (stepIsAnAddOp)
                return rewriter.create<arith::SubIOp>(
                    loc, stepOp->getOperand(commuteTheAddOp ? 1 : 0),
                    stepOp->getOperand(commuteTheAddOp ? 0 : 1));
              return rewriter.create<arith::AddIOp>(loc, stepOp->getOperands());
            }();
            args.push_back(newBump);
          }
          auto one = createIntConstant(rewriter, loc, iters.getType(), 1);
          args.push_back(rewriter.create<arith::SubIOp>(
              loc, entry.getArguments().back(), one));
          rewriter.replaceOpWithNewOp<cudaq::cc::ContinueOp>(contOp, args);
        });
  }

  /// For each Op in \p invertedOps, visit them in reverse order and move each
  /// to just in front of \p term (the end of the function). This reversal of
  /// the order of quantum operations is done recursively.
  static void reverseTheOpsInTheBlock(Location loc, Operation *term,
                                      SmallVector<Operation *> &&invertedOps) {
    OpBuilder builder(term);
    for (auto *op : llvm::reverse(invertedOps)) {
      auto invert = [&](Region &reg) {
        if (reg.empty())
          return;
        auto &block = reg.front();
        reverseTheOpsInTheBlock(loc, block.getTerminator(),
                                getOpsToInvert(block));
      };
      if (auto ifOp = dyn_cast<cudaq::cc::IfOp>(op)) {
        LLVM_DEBUG(llvm::dbgs() << "moving if: " << ifOp << ".\n");
        auto *newIf = builder.clone(*op);
        op->replaceAllUsesWith(newIf);
        op->erase();
        auto newIfOp = cast<cudaq::cc::IfOp>(newIf);
        invert(newIfOp.getThenRegion());
        invert(newIfOp.getElseRegion());
        continue;
      }
      if (auto forOp = dyn_cast<scf::ForOp>(op)) {
        LLVM_DEBUG(llvm::dbgs() << "moving for: " << forOp << ".\n");
        TODO_loc(loc, "cannot make adjoint of kernel with scf.for");
        // should we convert to cc.loop and use code below?
      }
      if (auto loopOp = dyn_cast<cudaq::cc::LoopOp>(op)) {
        LLVM_DEBUG(llvm::dbgs() << "moving loop: " << loopOp << ".\n");
        auto newLoopOp = cloneReversedLoop(builder, loopOp);
        op->replaceAllUsesWith(newLoopOp->getResults().drop_back());
        op->erase();
        invert(newLoopOp.getBodyRegion());
        continue;
      }
      if (auto scopeOp = dyn_cast<cudaq::cc::ScopeOp>(op)) {
        LLVM_DEBUG(llvm::dbgs() << "moving scope: " << scopeOp << ".\n");
        auto *newScope = builder.clone(*op);
        op->replaceAllUsesWith(newScope);
        op->erase();
        auto newScopeOp = cast<cudaq::cc::ScopeOp>(newScope);
        invert(newScopeOp.getInitRegion());
        continue;
      }

      bool opWasNegated = false;
      IRMapping mapper;
      LLVM_DEBUG(llvm::dbgs() << "moving quantum op: " << *op << ".\n");
      auto arrAttr = op->getAttr(segmentSizes).cast<DenseI32ArrayAttr>();
      // Walk over any floating-point parameters to `op` and negate them.
      for (auto iter = op->getOperands().begin(),
                endIter = op->getOperands().begin() + arrAttr[0];
           iter != endIter; ++iter) {
        Value val = *iter;
        Value neg = builder.create<arith::NegFOp>(loc, val.getType(), val);
        mapper.map(val, neg);
        opWasNegated = true;
      }

      // If this is a quantum op that is not self adjoint, we need
      // to adjoint it.
      if (auto quantumOp = dyn_cast_or_null<quake::OperatorInterface>(op);
          !quantumOp->hasTrait<cudaq::Hermitian>() && !opWasNegated) {
        if (op->hasAttr("is_adj"))
          op->removeAttr("is_adj");
        else
          op->setAttr("is_adj", builder.getUnitAttr());
      }

      auto *newOp = builder.clone(*op, mapper);
      assert(newOp->getNumResults() == 0);
      op->erase();
    }
  }

  /// This is the combination of adjoint and control transformations. We will
  /// create a control variant here, even if it wasn't needed to simplify
  /// things. The dead variant can be eliminated as unreferenced.
  func::FuncOp createAdjointControlVariantOf(func::FuncOp funcOp) {
    ModuleOp module = getOperation();
    auto funcName = funcOp.getName().str();
    auto ctrlFuncName = getCtrlVariantFunctionName(funcName);
    auto ctrlFunc = module.lookupSymbol<func::FuncOp>(ctrlFuncName);
    if (!ctrlFunc)
      ctrlFunc = createControlVariantOf(funcOp);

    auto newFuncName = getAdjCtrlVariantFunctionName(funcName);
    return createAdjointVariantOf(ctrlFunc, std::move(newFuncName));
  }

  /// Step 2. Specialize all the quake.apply ops and convert them to calls.
  void step2() {
    ModuleOp module = getOperation();
    auto *ctx = module.getContext();
    RewritePatternSet patterns(ctx);
    patterns.insert<ApplyOpPattern>(ctx);
    ConversionTarget target(*ctx);
    target.addLegalDialect<func::FuncDialect, quake::QuakeDialect>();
    target.addDynamicallyLegalOp<quake::ApplyOp>(
        [](quake::ApplyOp apply) { return apply->hasAttr("replaced"); });
    if (failed(applyPartialConversion(module, target, std::move(patterns)))) {
      emitError(module.getLoc(), "could not rewrite all apply ops.");
      signalPassFailure();
    }
  }

  // MLIR dependency: internal name used by tablegen.
  static constexpr char segmentSizes[] = "operand_segment_sizes";
};
} // namespace

std::unique_ptr<mlir::Pass> cudaq::opt::createApplyOpSpecializationPass() {
  return std::make_unique<ApplySpecializationPass>();
}
