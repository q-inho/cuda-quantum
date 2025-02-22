/*************************************************************** -*- C++ -*- ***
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 *******************************************************************************/

#include "cudaq/Optimizer/Dialect/CC/CCOps.h"
#include "cudaq/Optimizer/Builder/Factory.h"
#include "cudaq/Optimizer/Dialect/CC/CCDialect.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Utils/IndexingUtils.h"
#include "mlir/Dialect/Utils/StructuredOpsUtils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// LoopOp
//===----------------------------------------------------------------------===//

// Override the default.
Region &cudaq::cc::LoopOp::getLoopBody() { return getBodyRegion(); }

// The basic block of the step region must end in a continue op, which need not
// be pretty printed if the loop has no block arguments. This ensures the step
// block is properly terminated.
static void ensureStepTerminator(OpBuilder &builder, OperationState &result,
                                 Region *stepRegion) {
  auto *block = &stepRegion->back();
  auto addContinue = [&]() {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(block);
    builder.create<cudaq::cc::ContinueOp>(result.location);
  };
  if (block->empty()) {
    addContinue();
  } else {
    auto *term = &block->back();
    if (!term->hasTrait<OpTrait::IsTerminator>())
      addContinue();
  }
}

void cudaq::cc::LoopOp::build(OpBuilder &builder, OperationState &result,
                              TypeRange resultTypes, ValueRange iterArgs,
                              bool postCond, RegionBuilderFn whileBuilder,
                              RegionBuilderFn bodyBuilder,
                              RegionBuilderFn stepBuilder) {
  auto *whileRegion = result.addRegion();
  auto *bodyRegion = result.addRegion();
  auto *stepRegion = result.addRegion();
  whileBuilder(builder, result.location, *whileRegion);
  bodyBuilder(builder, result.location, *bodyRegion);
  if (stepBuilder) {
    stepBuilder(builder, result.location, *stepRegion);
    ensureStepTerminator(builder, result, stepRegion);
  }
  result.addAttribute(postCondAttrName(), builder.getBoolAttr(postCond));
  result.addOperands(iterArgs);
  result.addTypes(resultTypes);
}

void cudaq::cc::LoopOp::build(OpBuilder &builder, OperationState &result,
                              ValueRange iterArgs, bool postCond,
                              RegionBuilderFn whileBuilder,
                              RegionBuilderFn bodyBuilder,
                              RegionBuilderFn stepBuilder) {
  build(builder, result, TypeRange{}, iterArgs, postCond, whileBuilder,
        bodyBuilder, stepBuilder);
}

LogicalResult cudaq::cc::LoopOp::verify() {
  const auto initArgsSize = getInitArgs().size();
  if (getResults().size() != initArgsSize)
    return emitOpError("size of init args and outputs must be equal");
  if (getWhileRegion().front().getArguments().size() != initArgsSize)
    return emitOpError("size of init args and while region args must be equal");
  if (auto condOp = dyn_cast<cudaq::cc::ConditionOp>(
          getWhileRegion().front().getTerminator())) {
    if (condOp.getResults().size() != initArgsSize)
      return emitOpError("size of init args and condition op must be equal");
  } else {
    return emitOpError("while region must end with condition op");
  }
  if (getBodyRegion().front().getArguments().size() != initArgsSize)
    return emitOpError("size of init args and body region args must be equal");
  if (!getStepRegion().empty()) {
    if (getStepRegion().front().getArguments().size() != initArgsSize)
      return emitOpError(
          "size of init args and step region args must be equal");
    if (auto contOp = dyn_cast<cudaq::cc::ContinueOp>(
            getStepRegion().front().getTerminator())) {
      if (contOp.getOperands().size() != initArgsSize)
        return emitOpError("size of init args and continue op must be equal");
    } else {
      return emitOpError("step region must end with continue op");
    }
  }
  return success();
}

static void printInitializationList(OpAsmPrinter &p,
                                    Block::BlockArgListType blocksArgs,
                                    Operation::operand_range initializers) {
  assert(blocksArgs.size() == initializers.size() &&
         "expected same length of arguments and initializers");
  if (initializers.empty())
    return;

  p << "((";
  llvm::interleaveComma(llvm::zip(blocksArgs, initializers), p, [&](auto it) {
    p << std::get<0>(it) << " = " << std::get<1>(it);
  });
  p << ") -> (" << initializers.getTypes() << ")) ";
}

void cudaq::cc::LoopOp::print(OpAsmPrinter &p) {
  if (isPostConditional()) {
    p << " do ";
    printInitializationList(p, getBodyRegion().front().getArguments(),
                            getOperands());
    p.printRegion(getBodyRegion(), /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/true);
    p << " while ";
    p.printRegion(getWhileRegion(), /*printEntryBlockArgs=*/hasArguments(),
                  /*printBlockTerminators=*/true);
  } else {
    p << " while ";
    printInitializationList(p, getWhileRegion().front().getArguments(),
                            getOperands());
    p.printRegion(getWhileRegion(), /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/true);
    p << " do ";
    p.printRegion(getBodyRegion(), /*printEntryBlockArgs=*/hasArguments(),
                  /*printBlockTerminators=*/true);
    if (!getStepRegion().empty()) {
      p << " step ";
      p.printRegion(getStepRegion(), /*printEntryBlockArgs=*/hasArguments(),
                    /*printBlockTerminators=*/hasArguments());
    }
  }
  p.printOptionalAttrDict((*this)->getAttrs(), {postCondAttrName()});
}

ParseResult cudaq::cc::LoopOp::parse(OpAsmParser &parser,
                                     OperationState &result) {
  auto &builder = parser.getBuilder();
  bool isPostCondition = false;
  auto *cond = result.addRegion();
  auto *body = result.addRegion();
  auto *step = result.addRegion();
  auto parseOptBlockArgs =
      [&](SmallVector<OpAsmParser::Argument, 4> &regionArgs) {
        SmallVector<OpAsmParser::UnresolvedOperand, 4> operands;
        if (succeeded(parser.parseOptionalLParen())) {
          // Parse assignment list and results type list.
          if (parser.parseAssignmentList(regionArgs, operands) ||
              parser.parseArrowTypeList(result.types) || parser.parseRParen())
            return true;

          // Resolve input operands.
          for (auto argOperandType :
               llvm::zip(regionArgs, operands, result.types)) {
            auto type = std::get<2>(argOperandType);
            std::get<0>(argOperandType).type = type;
            if (parser.resolveOperand(std::get<1>(argOperandType), type,
                                      result.operands))
              return true;
          }
        }
        return false;
      };
  if (succeeded(parser.parseOptionalKeyword("while"))) {
    SmallVector<OpAsmParser::Argument, 4> regionArgs;
    if (parseOptBlockArgs(regionArgs) || parser.parseRegion(*cond, regionArgs))
      return failure();
    SmallVector<OpAsmParser::Argument, 4> emptyArgs;
    if (parser.parseKeyword("do") || parser.parseRegion(*body, emptyArgs))
      return failure();
    if (succeeded(parser.parseOptionalKeyword("step"))) {
      if (parser.parseRegion(*step, emptyArgs))
        return failure();
      OpBuilder opBuilder(builder.getContext());
      ensureStepTerminator(opBuilder, result, step);
    }
  } else if (succeeded(parser.parseOptionalKeyword("do"))) {
    isPostCondition = true;
    SmallVector<OpAsmParser::Argument, 4> regionArgs;
    if (parseOptBlockArgs(regionArgs) || parser.parseRegion(*body, regionArgs))
      return failure();
    SmallVector<OpAsmParser::Argument, 4> emptyArgs;
    if (parser.parseKeyword("while") || parser.parseRegion(*cond, emptyArgs))
      return failure();
  } else {
    return parser.emitError(parser.getNameLoc(), "expected 'while' or 'do'");
  }
  result.addAttribute(
      postCondAttrName(),
      builder.getIntegerAttr(builder.getI1Type(), isPostCondition));
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

void cudaq::cc::LoopOp::getSuccessorRegions(
    std::optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  if (!index) {
    // loop op - successor is either the while region or, if a post conditional
    // loop, the do region.
    if (isPostConditional())
      regions.push_back(
          RegionSuccessor(&getBodyRegion(), getDoEntryArguments()));
    else
      regions.push_back(
          RegionSuccessor(&getWhileRegion(), getWhileArguments()));
    return;
  }
  switch (index.value()) {
  case 0:
    // while region = successors are the owning loop op and the do region.
    regions.push_back(RegionSuccessor(&getBodyRegion(), getDoEntryArguments()));
    regions.push_back(RegionSuccessor(getResults()));
    break;
  case 1:
    // do region - successor is step if present or while if step is absent.
    // TODO: if the body contains a break, then the loop op is also a successor.
    if (hasStep())
      regions.push_back(RegionSuccessor(&getStepRegion(), getStepArguments()));
    else
      regions.push_back(
          RegionSuccessor(&getWhileRegion(), getWhileArguments()));
    break;
  case 2:
    // step region - if present, while region is always successor.
    if (hasStep())
      regions.push_back(
          RegionSuccessor(&getWhileRegion(), getWhileArguments()));
    break;
  }
}

OperandRange
cudaq::cc::LoopOp::getSuccessorEntryOperands(std::optional<unsigned> index) {
  return getInitArgs();
}

//===----------------------------------------------------------------------===//
// ScopeOp
//===----------------------------------------------------------------------===//

void cudaq::cc::ScopeOp::build(OpBuilder &builder, OperationState &result,
                               BodyBuilderFn bodyBuilder) {
  auto *bodyRegion = result.addRegion();
  bodyRegion->push_back(new Block);
  auto &bodyBlock = bodyRegion->front();
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(&bodyBlock);
  if (bodyBuilder)
    bodyBuilder(builder, result.location);
}

void cudaq::cc::ScopeOp::print(OpAsmPrinter &p) {
  bool printBlockTerminators = getRegion().getBlocks().size() > 1;
  if (!getResults().empty()) {
    p << " -> (" << getResultTypes() << ")";
    // Print terminator explicitly if the op defines values.
    printBlockTerminators = true;
  }
  p << ' ';
  p.printRegion(getRegion(), /*printEntryBlockArgs=*/false,
                printBlockTerminators);
  p.printOptionalAttrDict((*this)->getAttrs());
}

static void ensureScopeRegionTerminator(OpBuilder &builder,
                                        OperationState &result,
                                        Region *region) {
  auto *block = &region->back();
  if (!block)
    return;
  if (!block->empty()) {
    auto *term = &block->back();
    if (term->hasTrait<OpTrait::IsTerminator>())
      return;
  }
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  builder.create<cudaq::cc::ContinueOp>(result.location);
}

ParseResult cudaq::cc::ScopeOp::parse(OpAsmParser &parser,
                                      OperationState &result) {
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();
  auto *body = result.addRegion();
  if (parser.parseRegion(*body, /*arguments=*/{}, /*argTypes=*/{}) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  OpBuilder opBuilder(parser.getContext());
  ensureScopeRegionTerminator(opBuilder, result, body);
  return success();
}

void cudaq::cc::ScopeOp::getRegionInvocationBounds(
    ArrayRef<Attribute> attrs, SmallVectorImpl<InvocationBounds> &bounds) {}

void cudaq::cc::ScopeOp::getSuccessorRegions(
    std::optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  if (!index) {
    regions.push_back(RegionSuccessor(&getRegion()));
    return;
  }
  regions.push_back(RegionSuccessor(getResults()));
}

namespace {
// If there are no allocations in the scope, then the scope is not needed as
// there is nothing to deallocate. This transformation does the following
// rewrite.
//
//    op1
//    <vals> = cc.scope {
//      sop1; ...; sopN;
//      cc.continue <args>
//    }
//    op2
//  ──────────────────────────────────────
//    op1
//    br bb1^
//  ^bb1:
//    sop1; ...; sopN;
//    br bb2^(<args>)
//  ^bb2(<vals>):
//    op2
//
// The canonicalizer will then fuse these blocks appropriately.
struct EraseScopeWhenNotNeeded : public OpRewritePattern<cudaq::cc::ScopeOp> {
  using Base = OpRewritePattern<cudaq::cc::ScopeOp>;
  using Base::Base;

  LogicalResult matchAndRewrite(cudaq::cc::ScopeOp scope,
                                PatternRewriter &rewriter) const override {
    for (auto &reg : scope->getRegions())
      if (hasAllocation(reg))
        return success();

    // scope does not allocate, so the region can be inlined into the parent.
    auto loc = scope.getLoc();
    auto *scopeBlock = rewriter.getInsertionBlock();
    auto scopePt = rewriter.getInsertionPoint();
    // Split the block at the cc.scope. Make sure to maintain any values that
    // escape the cc.scope as block arguments.
    auto *splitBlock = rewriter.splitBlock(scopeBlock, scopePt);
    Block *succBlock;
    if (scope.getNumResults() == 0) {
      succBlock = splitBlock;
    } else {
      succBlock = rewriter.createBlock(
          splitBlock, scope.getResultTypes(),
          SmallVector<Location>(scope.getNumResults(), loc));
      rewriter.create<cf::BranchOp>(loc, splitBlock);
    }
    // Inline the cc.scope's region into the parent and create a branch to the
    // new successor block.
    auto &initRegion = scope.getInitRegion();
    auto *initBlock = &initRegion.front();
    auto *initTerminator = initRegion.back().getTerminator();
    auto initTerminatorOperands = initTerminator->getOperands();
    rewriter.setInsertionPointToEnd(&initRegion.back());
    rewriter.create<cf::BranchOp>(loc, succBlock, initTerminatorOperands);
    rewriter.eraseOp(initTerminator);
    rewriter.inlineRegionBefore(initRegion, succBlock);
    // Replace the cc.scope with a branch to the newly inlined region's entry
    // block.
    rewriter.setInsertionPointToEnd(scopeBlock);
    rewriter.create<cf::BranchOp>(loc, initBlock, ValueRange{});
    rewriter.replaceOp(scope, succBlock->getArguments());
    return success();
  }

  static bool hasAllocation(Region &region) {
    for (auto &block : region)
      for (auto &op : block) {
        if (auto mem = dyn_cast<MemoryEffectOpInterface>(op))
          return mem.hasEffect<MemoryEffects::Allocate>();
        for (auto &opReg : op.getRegions())
          if (hasAllocation(opReg))
            return true;
      }
    return false;
  }
};
} // namespace

void cudaq::cc::ScopeOp::getCanonicalizationPatterns(
    RewritePatternSet &patterns, MLIRContext *context) {
  patterns.add<EraseScopeWhenNotNeeded>(context);
}

//===----------------------------------------------------------------------===//
// IfOp
//===----------------------------------------------------------------------===//

void cudaq::cc::IfOp::build(OpBuilder &builder, OperationState &result,
                            TypeRange resultTypes, Value cond,
                            RegionBuilderFn thenBuilder,
                            RegionBuilderFn elseBuilder) {
  auto *thenRegion = result.addRegion();
  auto *elseRegion = result.addRegion();
  thenBuilder(builder, result.location, *thenRegion);
  if (elseBuilder)
    elseBuilder(builder, result.location, *elseRegion);
  result.addOperands(cond);
  result.addTypes(resultTypes);
}

LogicalResult cudaq::cc::IfOp::verify() {
  if (getNumResults() != 0 && getElseRegion().empty())
    return emitOpError("must have an else block if defining values");
  return success();
}

void cudaq::cc::IfOp::print(OpAsmPrinter &p) {
  p << '(' << getCondition() << ')';
  p.printOptionalArrowTypeList(getResultTypes());
  p << ' ';
  const bool printBlockTerminators =
      !getThenRegion().hasOneBlock() || (getNumResults() > 0);
  p.printRegion(getThenRegion(), /*printEntryBlockArgs=*/false,
                printBlockTerminators);
  if (!getElseRegion().empty()) {
    p << " else ";
    const bool printBlockTerminators =
        !getElseRegion().hasOneBlock() || (getNumResults() > 0);
    p.printRegion(getElseRegion(), /*printEntryBlockArgs=*/false,
                  printBlockTerminators);
  }
  p.printOptionalAttrDict((*this)->getAttrs());
}

static void ensureIfRegionTerminator(OpBuilder &builder, OperationState &result,
                                     Region *ifRegion) {
  auto *block = &ifRegion->back();
  if (!block)
    return;
  if (!block->empty()) {
    auto *term = &block->back();
    if (term->hasTrait<OpTrait::IsTerminator>())
      return;
  }
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  builder.create<cudaq::cc::ContinueOp>(result.location);
}

ParseResult cudaq::cc::IfOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  auto &builder = parser.getBuilder();
  auto *thenRegion = result.addRegion();
  auto *elseRegion = result.addRegion();
  OpAsmParser::UnresolvedOperand cond;
  auto i1Type = builder.getIntegerType(1);
  if (parser.parseLParen() || parser.parseOperand(cond) ||
      parser.parseRParen() ||
      parser.resolveOperand(cond, i1Type, result.operands))
    return failure();
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();
  if (parser.parseRegion(*thenRegion, /*arguments=*/{}, /*argTypes=*/{}))
    return failure();
  OpBuilder opBuilder(parser.getContext());
  ensureIfRegionTerminator(opBuilder, result, thenRegion);

  // If we find an 'else' keyword then parse the 'else' region.
  if (!parser.parseOptionalKeyword("else")) {
    if (parser.parseRegion(*elseRegion, /*arguments=*/{}, /*argTypes=*/{}))
      return failure();
    ensureIfRegionTerminator(opBuilder, result, elseRegion);
  }

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

void cudaq::cc::IfOp::getRegionInvocationBounds(
    ArrayRef<Attribute> attrs, SmallVectorImpl<InvocationBounds> &bounds) {
  // Assume non-constant condition. Each region may be executed 0 or 1 times.
  bounds.assign(2, {0, 1});
}

void cudaq::cc::IfOp::getSuccessorRegions(
    std::optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  if (index) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }
  // TODO: can constant fold if the condition is a constant here.
  regions.push_back(RegionSuccessor(&getThenRegion()));
  if (!getElseRegion().empty())
    regions.push_back(RegionSuccessor(&getElseRegion()));
}

//===----------------------------------------------------------------------===//
// CreateLambdaOp
//===----------------------------------------------------------------------===//

void cudaq::cc::CreateLambdaOp::build(OpBuilder &builder,
                                      OperationState &result,
                                      cudaq::cc::LambdaType lambdaTy,
                                      BodyBuilderFn bodyBuilder) {
  auto *bodyRegion = result.addRegion();
  bodyRegion->push_back(new Block);
  result.addTypes(TypeRange{lambdaTy});
  auto &bodyBlock = bodyRegion->front();
  auto argTys = lambdaTy.getSignature().getInputs();
  SmallVector<Location> locations(argTys.size(), result.location);
  bodyBlock.addArguments(argTys, locations);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(&bodyBlock);
  if (bodyBuilder)
    bodyBuilder(builder, result.location);
}

void cudaq::cc::CreateLambdaOp::print(OpAsmPrinter &p) {
  p << ' ';
  bool hasArgs = getRegion().getNumArguments() != 0;
  bool hasRes =
      getType().cast<cudaq::cc::LambdaType>().getSignature().getNumResults();
  p.printRegion(getRegion(), /*printEntryBlockArgs=*/hasArgs,
                /*printBlockTerminators=*/hasRes);
  p << " : " << getType();
  p.printOptionalAttrDict((*this)->getAttrs(), {"signature"});
}

ParseResult cudaq::cc::CreateLambdaOp::parse(OpAsmParser &parser,
                                             OperationState &result) {
  auto *body = result.addRegion();
  Type lambdaTy;
  if (parser.parseRegion(*body, /*arguments=*/{}, /*argTypes=*/{}) ||
      parser.parseColonType(lambdaTy) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  result.addAttribute("signature", TypeAttr::get(lambdaTy));
  result.addTypes(lambdaTy);
  CreateLambdaOp::ensureTerminator(*body, parser.getBuilder(), result.location);
  return success();
}

//===----------------------------------------------------------------------===//
// CallCallableOp
//===----------------------------------------------------------------------===//

LogicalResult cudaq::cc::CallCallableOp::verify() {
  FunctionType funcTy;
  auto ty = getCallee().getType();
  if (auto lambdaTy = dyn_cast<cudaq::cc::LambdaType>(ty))
    funcTy = lambdaTy.getSignature();
  else if (auto fTy = dyn_cast<FunctionType>(ty))
    funcTy = fTy;
  else
    return emitOpError("callee has unexpected type");

  // Check argument types.
  auto argTys = funcTy.getInputs();
  if (argTys.size() != getArgOperands().size())
    return emitOpError("call has incorrect arity");
  for (auto [targArg, argVal] : llvm::zip(argTys, getArgOperands()))
    if (targArg != argVal.getType())
      return emitOpError("argument type mismatch");

  // Check return types.
  auto resTys = funcTy.getResults();
  if (resTys.size() != getResults().size())
    return emitOpError("call has incorrect coarity");
  for (auto [targRes, callVal] : llvm::zip(resTys, getResults()))
    if (targRes != callVal.getType())
      return emitOpError("result type mismatch");
  return success();
}

//===----------------------------------------------------------------------===//
// ReturnOp
//===----------------------------------------------------------------------===//

LogicalResult cudaq::cc::ReturnOp::verify() {
  auto *op = getOperation();
  auto resultTypes = [&]() {
    if (auto func = op->getParentOfType<CreateLambdaOp>()) {
      auto lambdaTy = cast<LambdaType>(func->getResult(0).getType());
      return SmallVector<Type>(lambdaTy.getSignature().getResults());
    }
    if (auto func = op->getParentOfType<func::FuncOp>())
      return SmallVector<Type>(func.getResultTypes());
    return SmallVector<Type>();
  }();

  // The operand number and types must match the function signature.
  if (getNumOperands() != resultTypes.size())
    return emitOpError("has ")
           << getNumOperands()
           << " operands, but enclosing function/lambda returns "
           << resultTypes.size();
  for (auto ep :
       llvm::enumerate(llvm::zip(getOperands().getTypes(), resultTypes))) {
    auto p = ep.value();
    auto i = ep.index();
    if (std::get<0>(p) != std::get<1>(p))
      return emitOpError("type of return operand ")
             << i << " (" << std::get<0>(p)
             << ") doesn't match function/lambda result type ("
             << std::get<1>(p) << ')';
  }
  return success();
}

// Replace an Op of type FROM with an Op of type WITH if the Op appears to be
// directly owned by a func::FuncOp. This is required to replace cc.return with
// func.return.
template <typename FROM, typename WITH>
struct ReplaceInFunc : public OpRewritePattern<FROM> {
  using Base = OpRewritePattern<FROM>;
  using Base::Base;

  LogicalResult matchAndRewrite(FROM fromOp,
                                PatternRewriter &rewriter) const override {
    if (isa_and_nonnull<func::FuncOp>(fromOp->getParentOp()))
      rewriter.replaceOpWithNewOp<WITH>(fromOp, fromOp.getOperands());
    return success();
  }
};

void cudaq::cc::ReturnOp::getCanonicalizationPatterns(
    RewritePatternSet &patterns, MLIRContext *context) {
  patterns.add<ReplaceInFunc<cudaq::cc::ReturnOp, func::ReturnOp>>(context);
}

//===----------------------------------------------------------------------===//
// ConditionOp
//===----------------------------------------------------------------------===//

LogicalResult cudaq::cc::ConditionOp::verify() {
  Operation *self = getOperation();
  Region *region = self->getBlock()->getParent();
  auto parentOp = self->getParentOfType<LoopOp>();
  assert(parentOp); // checked by tablegen constraints
  if (&parentOp.getWhileRegion() != region)
    return emitOpError("only valid in the while region of a loop");
  return success();
}

MutableOperandRange cudaq::cc::ConditionOp::getMutableSuccessorOperands(
    std::optional<unsigned> index) {
  return getResultsMutable();
}

//===----------------------------------------------------------------------===//
// UnwindBreakOp
//===----------------------------------------------------------------------===//

LogicalResult cudaq::cc::UnwindBreakOp::verify() {
  // The arguments to this op must correspond to the LoopOp's results.
  bool foundFunc = true;
  auto *op = getOperation();
  auto resultTypes = [&]() {
    if (auto func = op->getParentOfType<LoopOp>())
      return SmallVector<Type>(func->getResultTypes());
    foundFunc = false;
    return SmallVector<Type>();
  }();
  if (!foundFunc)
    return emitOpError("cannot find nearest enclosing loop");
  if (getOperands().size() != resultTypes.size())
    return emitOpError("arity of arguments and loop result mismatch");
  for (auto p : llvm::zip(getOperands().getTypes(), resultTypes))
    if (std::get<0>(p) != std::get<1>(p))
      return emitOpError("argument type mismatch with loop result");
  return success();
}

// Replace an Op of type FROM with an Op of type WITH if the Op appears to be
// directly owned by a cc::LoopOp. This is required to replace unwind breaks and
// unwind continues with breaks and continues, resp., when a cc::ScopeOp is
// erased.
template <typename FROM, typename WITH>
struct ReplaceInLoop : public OpRewritePattern<FROM> {
  using Base = OpRewritePattern<FROM>;
  using Base::Base;

  LogicalResult matchAndRewrite(FROM fromOp,
                                PatternRewriter &rewriter) const override {
    if (isa_and_nonnull<cudaq::cc::LoopOp>(fromOp->getParentOp())) {
      auto *scopeBlock = rewriter.getInsertionBlock();
      auto scopePt = rewriter.getInsertionPoint();
      rewriter.splitBlock(scopeBlock, scopePt);
      rewriter.setInsertionPointToEnd(scopeBlock);
      rewriter.replaceOpWithNewOp<WITH>(fromOp, fromOp.getOperands());
    }
    return success();
  }
};

void cudaq::cc::UnwindBreakOp::getCanonicalizationPatterns(
    RewritePatternSet &patterns, MLIRContext *context) {
  patterns.add<ReplaceInLoop<cudaq::cc::UnwindBreakOp, cudaq::cc::BreakOp>>(
      context);
}

//===----------------------------------------------------------------------===//
// UnwindContinueOp
//===----------------------------------------------------------------------===//

LogicalResult cudaq::cc::UnwindContinueOp::verify() {
  // The arguments to this op must correspond to the LoopOp's results.
  bool foundFunc = true;
  auto *op = getOperation();
  auto resultTypes = [&]() {
    if (auto func = op->getParentOfType<LoopOp>())
      return SmallVector<Type>(func->getResultTypes());
    foundFunc = false;
    return SmallVector<Type>();
  }();
  if (!foundFunc)
    return emitOpError("cannot find nearest enclosing loop");
  if (getOperands().size() != resultTypes.size())
    return emitOpError("arity of arguments and loop result mismatch");
  for (auto p : llvm::zip(getOperands().getTypes(), resultTypes))
    if (std::get<0>(p) != std::get<1>(p))
      return emitOpError("argument type mismatch with loop result");
  return success();
}

void cudaq::cc::UnwindContinueOp::getCanonicalizationPatterns(
    RewritePatternSet &patterns, MLIRContext *context) {
  patterns
      .add<ReplaceInLoop<cudaq::cc::UnwindContinueOp, cudaq::cc::ContinueOp>>(
          context);
}

//===----------------------------------------------------------------------===//
// UnwindReturnOp
//===----------------------------------------------------------------------===//

LogicalResult cudaq::cc::UnwindReturnOp::verify() {
  // The arguments to this op must correspond to the FuncOp's results.
  bool foundFunc = true;
  auto *op = getOperation();
  auto resultTypes = [&]() {
    if (auto func = op->getParentOfType<CreateLambdaOp>()) {
      auto lambdaTy = cast<LambdaType>(func->getResult(0).getType());
      return SmallVector<Type>(lambdaTy.getSignature().getResults());
    }
    if (auto func = op->getParentOfType<func::FuncOp>())
      return SmallVector<Type>(func.getResultTypes());
    foundFunc = false;
    return SmallVector<Type>();
  }();
  if (!foundFunc)
    return emitOpError("cannot find nearest enclosing function/lambda");
  if (getOperands().size() != resultTypes.size())
    return emitOpError(
        "arity of arguments and function/lambda result mismatch");
  for (auto p : llvm::zip(getOperands().getTypes(), resultTypes))
    if (std::get<0>(p) != std::get<1>(p))
      return emitOpError("argument type mismatch with function/lambda result");
  return success();
}

//===----------------------------------------------------------------------===//
// Generated logic
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "cudaq/Optimizer/Dialect/CC/CCOps.cpp.inc"
