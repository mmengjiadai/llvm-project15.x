//===- SparseAnalysis.cpp - Sparse data-flow analysis ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/DataFlow/SparseAnalysis.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Interfaces/CallInterfaces.h"

using namespace mlir;
using namespace mlir::dataflow;

//===----------------------------------------------------------------------===//
// AbstractSparseLattice
//===----------------------------------------------------------------------===//

void AbstractSparseLattice::onUpdate(DataFlowSolver *solver) const {
  AnalysisState::onUpdate(solver);

  // Push all users of the value to the queue.
  for (Operation *user : point.get<Value>().getUsers())
    for (DataFlowAnalysis *analysis : useDefSubscribers)
      solver->enqueue({user, analysis});
}

//===----------------------------------------------------------------------===//
// AbstractSparseForwardDataFlowAnalysis
//===----------------------------------------------------------------------===//

AbstractSparseForwardDataFlowAnalysis::AbstractSparseForwardDataFlowAnalysis(
    DataFlowSolver &solver)
    : DataFlowAnalysis(solver) {
  registerPointKind<CFGEdge>();
}

LogicalResult
AbstractSparseForwardDataFlowAnalysis::initialize(Operation *top) {
  // Mark the entry block arguments as having reached their pessimistic
  // fixpoints.
  for (Region &region : top->getRegions()) {
    if (region.empty())
      continue;
    for (Value argument : region.front().getArguments())
      setToEntryState(getLatticeElement(argument));
  }

  return initializeRecursively(top);
}

LogicalResult
AbstractSparseForwardDataFlowAnalysis::initializeRecursively(Operation *op) {
  // Initialize the analysis by visiting every owner of an SSA value (all
  // operations and blocks).
  visitOperation(op);
  for (Region &region : op->getRegions()) {
    for (Block &block : region) {
      getOrCreate<Executable>(&block)->blockContentSubscribe(this);
      visitBlock(&block);
      for (Operation &op : block)
        if (failed(initializeRecursively(&op)))
          return failure();
    }
  }

  return success();
}

LogicalResult AbstractSparseForwardDataFlowAnalysis::visit(ProgramPoint point) {
  if (Operation *op = llvm::dyn_cast_if_present<Operation *>(point))
    visitOperation(op);
  else if (Block *block = llvm::dyn_cast_if_present<Block *>(point))
    visitBlock(block);
  else
    return failure();
  return success();
}

void AbstractSparseForwardDataFlowAnalysis::visitOperation(Operation *op) {
  // Exit early on operations with no results.
  if (op->getNumResults() == 0)
    return;

  // If the containing block is not executable, bail out.
  if (!getOrCreate<Executable>(op->getBlock())->isLive())
    return;

  // Get the result lattices.
  SmallVector<AbstractSparseLattice *> resultLattices;
  resultLattices.reserve(op->getNumResults());
  for (Value result : op->getResults()) {
    AbstractSparseLattice *resultLattice = getLatticeElement(result);
    resultLattices.push_back(resultLattice);
  }

  // The results of a region branch operation are determined by control-flow.
  if (auto branch = dyn_cast<RegionBranchOpInterface>(op)) {
    return visitRegionSuccessors({branch}, branch,
                                 /*successorIndex=*/std::nullopt,
                                 resultLattices);
  }

  // The results of a call operation are determined by the callgraph.
  if (auto call = dyn_cast<CallOpInterface>(op)) {
    const auto *predecessors = getOrCreateFor<PredecessorState>(op, call);
    // If not all return sites are known, then conservatively assume we can't
    // reason about the data-flow.
    if (!predecessors->allPredecessorsKnown())
      return setAllToEntryStates(resultLattices);
    for (Operation *predecessor : predecessors->getKnownPredecessors())
      for (auto it : llvm::zip(predecessor->getOperands(), resultLattices))
        join(std::get<1>(it), *getLatticeElementFor(op, std::get<0>(it)));
    return;
  }

  // Grab the lattice elements of the operands.
  SmallVector<const AbstractSparseLattice *> operandLattices;
  operandLattices.reserve(op->getNumOperands());
  for (Value operand : op->getOperands()) {
    AbstractSparseLattice *operandLattice = getLatticeElement(operand);
    operandLattice->useDefSubscribe(this);
    operandLattices.push_back(operandLattice);
  }

  // Invoke the operation transfer function.
  visitOperationImpl(op, operandLattices, resultLattices);
}

void AbstractSparseForwardDataFlowAnalysis::visitBlock(Block *block) {
  // Exit early on blocks with no arguments.
  if (block->getNumArguments() == 0)
    return;

  // If the block is not executable, bail out.
  if (!getOrCreate<Executable>(block)->isLive())
    return;

  // Get the argument lattices.
  SmallVector<AbstractSparseLattice *> argLattices;
  argLattices.reserve(block->getNumArguments());
  for (BlockArgument argument : block->getArguments()) {
    AbstractSparseLattice *argLattice = getLatticeElement(argument);
    argLattices.push_back(argLattice);
  }

  // The argument lattices of entry blocks are set by region control-flow or the
  // callgraph.
  if (block->isEntryBlock()) {
    // Check if this block is the entry block of a callable region.
    auto callable = dyn_cast<CallableOpInterface>(block->getParentOp());
    if (callable && callable.getCallableRegion() == block->getParent()) {
      const auto *callsites = getOrCreateFor<PredecessorState>(block, callable);
      // If not all callsites are known, conservatively mark all lattices as
      // having reached their pessimistic fixpoints.
      if (!callsites->allPredecessorsKnown())
        return setAllToEntryStates(argLattices);
      for (Operation *callsite : callsites->getKnownPredecessors()) {
        auto call = cast<CallOpInterface>(callsite);
        for (auto it : llvm::zip(call.getArgOperands(), argLattices))
          join(std::get<1>(it), *getLatticeElementFor(block, std::get<0>(it)));
      }
      return;
    }

    // Check if the lattices can be determined from region control flow.
    if (auto branch = dyn_cast<RegionBranchOpInterface>(block->getParentOp())) {
      return visitRegionSuccessors(
          block, branch, block->getParent()->getRegionNumber(), argLattices);
    }

    // Otherwise, we can't reason about the data-flow.
    return visitNonControlFlowArgumentsImpl(block->getParentOp(),
                                            RegionSuccessor(block->getParent()),
                                            argLattices, /*firstIndex=*/0);
  }

  // Iterate over the predecessors of the non-entry block.
  for (Block::pred_iterator it = block->pred_begin(), e = block->pred_end();
       it != e; ++it) {
    Block *predecessor = *it;

    // If the edge from the predecessor block to the current block is not live,
    // bail out.
    auto *edgeExecutable =
        getOrCreate<Executable>(getProgramPoint<CFGEdge>(predecessor, block));
    edgeExecutable->blockContentSubscribe(this);
    if (!edgeExecutable->isLive())
      continue;

    // Check if we can reason about the data-flow from the predecessor.
    if (auto branch =
            dyn_cast<BranchOpInterface>(predecessor->getTerminator())) {
      SuccessorOperands operands =
          branch.getSuccessorOperands(it.getSuccessorIndex());
      for (auto [idx, lattice] : llvm::enumerate(argLattices)) {
        if (Value operand = operands[idx]) {
          join(lattice, *getLatticeElementFor(block, operand));
        } else {
          // Conservatively consider internally produced arguments as entry
          // points.
          setAllToEntryStates(lattice);
        }
      }
    } else {
      return setAllToEntryStates(argLattices);
    }
  }
}

void AbstractSparseForwardDataFlowAnalysis::visitRegionSuccessors(
    ProgramPoint point, RegionBranchOpInterface branch,
    std::optional<unsigned> successorIndex,
    ArrayRef<AbstractSparseLattice *> lattices) {
  const auto *predecessors = getOrCreateFor<PredecessorState>(point, point);
  assert(predecessors->allPredecessorsKnown() &&
         "unexpected unresolved region successors");

  for (Operation *op : predecessors->getKnownPredecessors()) {
    // Get the incoming successor operands.
    std::optional<OperandRange> operands;

    // Check if the predecessor is the parent op.
    if (op == branch) {
      operands = branch.getEntrySuccessorOperands(successorIndex);
      // Otherwise, try to deduce the operands from a region return-like op.
    } else if (auto regionTerminator =
                   dyn_cast<RegionBranchTerminatorOpInterface>(op)) {
      operands = regionTerminator.getSuccessorOperands(successorIndex);
    }

    if (!operands) {
      // We can't reason about the data-flow.
      return setAllToEntryStates(lattices);
    }

    ValueRange inputs = predecessors->getSuccessorInputs(op);
    assert(inputs.size() == operands->size() &&
           "expected the same number of successor inputs as operands");

    unsigned firstIndex = 0;
    if (inputs.size() != lattices.size()) {
      if (llvm::dyn_cast_if_present<Operation *>(point)) {
        if (!inputs.empty())
          firstIndex = cast<OpResult>(inputs.front()).getResultNumber();
        visitNonControlFlowArgumentsImpl(
            branch,
            RegionSuccessor(
                branch->getResults().slice(firstIndex, inputs.size())),
            lattices, firstIndex);
      } else {
        if (!inputs.empty())
          firstIndex = cast<BlockArgument>(inputs.front()).getArgNumber();
        Region *region = point.get<Block *>()->getParent();
        visitNonControlFlowArgumentsImpl(
            branch,
            RegionSuccessor(region, region->getArguments().slice(
                                        firstIndex, inputs.size())),
            lattices, firstIndex);
      }
    }

    for (auto it : llvm::zip(*operands, lattices.drop_front(firstIndex)))
      join(std::get<1>(it), *getLatticeElementFor(point, std::get<0>(it)));
  }
}

const AbstractSparseLattice *
AbstractSparseForwardDataFlowAnalysis::getLatticeElementFor(ProgramPoint point,
                                                            Value value) {
  AbstractSparseLattice *state = getLatticeElement(value);
  addDependency(state, point);
  return state;
}

void AbstractSparseForwardDataFlowAnalysis::setAllToEntryStates(
    ArrayRef<AbstractSparseLattice *> lattices) {
  for (AbstractSparseLattice *lattice : lattices)
    setToEntryState(lattice);
}

void AbstractSparseForwardDataFlowAnalysis::join(
    AbstractSparseLattice *lhs, const AbstractSparseLattice &rhs) {
  propagateIfChanged(lhs, lhs->join(rhs));
}

//===----------------------------------------------------------------------===//
// AbstractSparseBackwardDataFlowAnalysis
//===----------------------------------------------------------------------===//

AbstractSparseBackwardDataFlowAnalysis::AbstractSparseBackwardDataFlowAnalysis(
    DataFlowSolver &solver, SymbolTableCollection &symbolTable)
    : DataFlowAnalysis(solver), symbolTable(symbolTable) {
  registerPointKind<CFGEdge>();
}

LogicalResult
AbstractSparseBackwardDataFlowAnalysis::initialize(Operation *top) {
  return initializeRecursively(top);
}

LogicalResult
AbstractSparseBackwardDataFlowAnalysis::initializeRecursively(Operation *op) {
  visitOperation(op);
  for (Region &region : op->getRegions()) {
    for (Block &block : region) {
      getOrCreate<Executable>(&block)->blockContentSubscribe(this);
      // Initialize ops in reverse order, so we can do as much initial
      // propagation as possible without having to go through the
      // solver queue.
      for (auto it = block.rbegin(); it != block.rend(); it++)
        if (failed(initializeRecursively(&*it)))
          return failure();
    }
  }
  return success();
}

LogicalResult
AbstractSparseBackwardDataFlowAnalysis::visit(ProgramPoint point) {
  if (Operation *op = llvm::dyn_cast_if_present<Operation *>(point))
    visitOperation(op);
  else if (llvm::dyn_cast_if_present<Block *>(point))
    // For backward dataflow, we don't have to do any work for the blocks
    // themselves. CFG edges between blocks are processed by the BranchOp
    // logic in `visitOperation`, and entry blocks for functions are tied
    // to the CallOp arguments by visitOperation.
    return success();
  else
    return failure();
  return success();
}

SmallVector<AbstractSparseLattice *>
AbstractSparseBackwardDataFlowAnalysis::getLatticeElements(ValueRange values) {
  SmallVector<AbstractSparseLattice *> resultLattices;
  resultLattices.reserve(values.size());
  for (Value result : values) {
    AbstractSparseLattice *resultLattice = getLatticeElement(result);
    resultLattices.push_back(resultLattice);
  }
  return resultLattices;
}

SmallVector<const AbstractSparseLattice *>
AbstractSparseBackwardDataFlowAnalysis::getLatticeElementsFor(
    ProgramPoint point, ValueRange values) {
  SmallVector<const AbstractSparseLattice *> resultLattices;
  resultLattices.reserve(values.size());
  for (Value result : values) {
    const AbstractSparseLattice *resultLattice =
        getLatticeElementFor(point, result);
    resultLattices.push_back(resultLattice);
  }
  return resultLattices;
}

static MutableArrayRef<OpOperand> operandsToOpOperands(OperandRange &operands) {
  return MutableArrayRef<OpOperand>(operands.getBase(), operands.size());
}

void AbstractSparseBackwardDataFlowAnalysis::visitOperation(Operation *op) {
  // If we're in a dead block, bail out.
  if (!getOrCreate<Executable>(op->getBlock())->isLive())
    return;

  SmallVector<AbstractSparseLattice *> operandLattices =
      getLatticeElements(op->getOperands());
  SmallVector<const AbstractSparseLattice *> resultLattices =
      getLatticeElementsFor(op, op->getResults());

  // Block arguments of region branch operations flow back into the operands
  // of the parent op
  if (auto branch = dyn_cast<RegionBranchOpInterface>(op)) {
    visitRegionSuccessors(branch, operandLattices);
    return;
  }

  if (auto branch = dyn_cast<BranchOpInterface>(op)) {
    // Block arguments of successor blocks flow back into our operands.

    // We remember all operands not forwarded to any block in a BitVector.
    // We can't just cut out a range here, since the non-forwarded ops might
    // be non-contiguous (if there's more than one successor).
    BitVector unaccounted(op->getNumOperands(), true);

    for (auto [index, block] : llvm::enumerate(op->getSuccessors())) {
      SuccessorOperands successorOperands = branch.getSuccessorOperands(index);
      OperandRange forwarded = successorOperands.getForwardedOperands();
      if (!forwarded.empty()) {
        MutableArrayRef<OpOperand> operands = op->getOpOperands().slice(
            forwarded.getBeginOperandIndex(), forwarded.size());
        for (OpOperand &operand : operands) {
          unaccounted.reset(operand.getOperandNumber());
          if (std::optional<BlockArgument> blockArg =
                  detail::getBranchSuccessorArgument(
                      successorOperands, operand.getOperandNumber(), block)) {
            meet(getLatticeElement(operand.get()),
                 *getLatticeElementFor(op, *blockArg));
          }
        }
      }
    }
    // Operands not forwarded to successor blocks are typically parameters
    // of the branch operation itself (for example the boolean for if/else).
    for (int index : unaccounted.set_bits()) {
      OpOperand &operand = op->getOpOperand(index);
      visitBranchOperand(operand);
    }
    return;
  }

  // For function calls, connect the arguments of the entry blocks
  // to the operands of the call op.
  if (auto call = dyn_cast<CallOpInterface>(op)) {
    Operation *callableOp = call.resolveCallable(&symbolTable);
    if (auto callable = dyn_cast_or_null<CallableOpInterface>(callableOp)) {
      Region *region = callable.getCallableRegion();
      if (region && !region->empty()) {
        Block &block = region->front();
        for (auto [blockArg, operand] :
             llvm::zip(block.getArguments(), operandLattices)) {
          meet(operand, *getLatticeElementFor(op, blockArg));
        }
      }
      return;
    }
  }

  // When the region of an op implementing `RegionBranchOpInterface` has a
  // terminator implementing `RegionBranchTerminatorOpInterface` or a
  // return-like terminator, the region's successors' arguments flow back into
  // the "successor operands" of this terminator.
  //
  // A successor operand with respect to an op implementing
  // `RegionBranchOpInterface` is an operand that is forwarded to a region
  // successor's input. There are two types of successor operands: the operands
  // of this op itself and the operands of the terminators of the regions of
  // this op.
  if (auto terminator = dyn_cast<RegionBranchTerminatorOpInterface>(op)) {
    if (auto branch = dyn_cast<RegionBranchOpInterface>(op->getParentOp())) {
      visitRegionSuccessorsFromTerminator(terminator, branch);
      return;
    }
  }

  if (op->hasTrait<OpTrait::ReturnLike>()) {
    // Going backwards, the operands of the return are derived from the
    // results of all CallOps calling this CallableOp.
    if (auto callable = dyn_cast<CallableOpInterface>(op->getParentOp())) {
      const PredecessorState *callsites =
          getOrCreateFor<PredecessorState>(op, callable);
      if (callsites->allPredecessorsKnown()) {
        for (Operation *call : callsites->getKnownPredecessors()) {
          SmallVector<const AbstractSparseLattice *> callResultLattices =
              getLatticeElementsFor(op, call->getResults());
          for (auto [op, result] :
               llvm::zip(operandLattices, callResultLattices))
            meet(op, *result);
        }
      } else {
        // If we don't know all the callers, we can't know where the
        // returned values go. Note that, in particular, this will trigger
        // for the return ops of any public functions.
        setAllToExitStates(operandLattices);
      }
      return;
    }
  }

  visitOperationImpl(op, operandLattices, resultLattices);
}

void AbstractSparseBackwardDataFlowAnalysis::visitRegionSuccessors(
    RegionBranchOpInterface branch,
    ArrayRef<AbstractSparseLattice *> operandLattices) {
  Operation *op = branch.getOperation();
  SmallVector<RegionSuccessor> successors;
  SmallVector<Attribute> operands(op->getNumOperands(), nullptr);
  branch.getEntrySuccessorRegions(operands, successors);

  // All operands not forwarded to any successor. This set can be non-contiguous
  // in the presence of multiple successors.
  BitVector unaccounted(op->getNumOperands(), true);

  for (RegionSuccessor &successor : successors) {
    Region *region = successor.getSuccessor();
    OperandRange operands =
        region ? branch.getEntrySuccessorOperands(region->getRegionNumber())
               : branch.getEntrySuccessorOperands({});
    MutableArrayRef<OpOperand> opoperands = operandsToOpOperands(operands);
    ValueRange inputs = successor.getSuccessorInputs();
    for (auto [operand, input] : llvm::zip(opoperands, inputs)) {
      meet(getLatticeElement(operand.get()), *getLatticeElementFor(op, input));
      unaccounted.reset(operand.getOperandNumber());
    }
  }
  // All operands not forwarded to regions are typically parameters of the
  // branch operation itself (for example the boolean for if/else).
  for (int index : unaccounted.set_bits()) {
    visitBranchOperand(op->getOpOperand(index));
  }
}

void AbstractSparseBackwardDataFlowAnalysis::
    visitRegionSuccessorsFromTerminator(
        RegionBranchTerminatorOpInterface terminator,
        RegionBranchOpInterface branch) {
  assert(isa<RegionBranchTerminatorOpInterface>(terminator) &&
         "expected a `RegionBranchTerminatorOpInterface` op");
  assert(terminator->getParentOp() == branch.getOperation() &&
         "expected `branch` to be the parent op of `terminator`");

  SmallVector<Attribute> operandAttributes(terminator->getNumOperands(),
                                           nullptr);
  SmallVector<RegionSuccessor> successors;
  terminator.getSuccessorRegions(operandAttributes, successors);
  // All operands not forwarded to any successor. This set can be
  // non-contiguous in the presence of multiple successors.
  BitVector unaccounted(terminator->getNumOperands(), true);

  for (const RegionSuccessor &successor : successors) {
    ValueRange inputs = successor.getSuccessorInputs();
    Region *region = successor.getSuccessor();
    OperandRange operands = terminator.getSuccessorOperands(
        region ? region->getRegionNumber() : std::optional<unsigned>{});
    MutableArrayRef<OpOperand> opOperands = operandsToOpOperands(operands);
    for (auto [opOperand, input] : llvm::zip(opOperands, inputs)) {
      meet(getLatticeElement(opOperand.get()),
           *getLatticeElementFor(terminator, input));
      unaccounted.reset(const_cast<OpOperand &>(opOperand).getOperandNumber());
    }
  }
  // Visit operands of the branch op not forwarded to the next region.
  // (Like e.g. the boolean of `scf.conditional`)
  for (int index : unaccounted.set_bits()) {
    visitBranchOperand(terminator->getOpOperand(index));
  }
}

const AbstractSparseLattice *
AbstractSparseBackwardDataFlowAnalysis::getLatticeElementFor(ProgramPoint point,
                                                             Value value) {
  AbstractSparseLattice *state = getLatticeElement(value);
  addDependency(state, point);
  return state;
}

void AbstractSparseBackwardDataFlowAnalysis::setAllToExitStates(
    ArrayRef<AbstractSparseLattice *> lattices) {
  for (AbstractSparseLattice *lattice : lattices)
    setToExitState(lattice);
}

void AbstractSparseBackwardDataFlowAnalysis::meet(
    AbstractSparseLattice *lhs, const AbstractSparseLattice &rhs) {
  propagateIfChanged(lhs, lhs->meet(rhs));
}
