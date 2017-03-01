/* Copyright 2017 Peter Goodman (peter@trailofbits.com), all rights reserved. */

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/SmallVector.h>

#include <llvm/IR/Argument.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <llvm/Transforms/Scalar.h>

#include "mcsema/Arch/Register.h"
#include "mcsema/BC/DeadRegElimination.h"

namespace {

using RegSet = std::bitset<128>;
using OffsetMap = std::unordered_map<llvm::Value *, unsigned>;

struct BlockState {
  // 1 if this block uses the register without defining it first, 0 if we
  // have no information. This is the set of registers where the incoming
  // values from predecessors is needed.
  RegSet live_on_entry;

  // 1 if this block defines the register, 0 if we have no information
  // (even if it is loaded). We can use this to kill any `live_on_entry` regs
  // coming from successors, before merging in the `live_on_entry` for a
  // data-flow propagation.
  RegSet killed_in_block;
};

struct FunctionState {
  std::unordered_map<llvm::Instruction *, unsigned> offsets;
};

// Tracks register
static std::vector<size_t> gByteOffsetToRegOffset;
static std::vector<unsigned> gByteOffsetToBeginOffset;
static std::vector<unsigned> gByteOffsetToRegSize;

static std::unordered_map<llvm::BasicBlock *, BlockState> gBlockState;

// Alias scopes, used to mark accesses to the reg state struct and memory as
// being within disjoint alias scopes.
static llvm::MDNode *gRegStateScope = nullptr;
static llvm::MDNode *gMemoryScope = nullptr;

// Get the index sequence of a GEP instruction. For GEPs that access the system
// register state, this allows us to index into the `system_regs` map in order
// to find the correct system register. In cases where we're operating on a
// bitcast system register, this lets us find the offset into that register.
static size_t GetOffsetFromBasePtr(const llvm::GetElementPtrInst *gep_inst,
                                   bool &failed) {
  llvm::APInt offset(64, 0);
  llvm::DataLayout data_layout(gep_inst->getModule());
  const auto found_offset = gep_inst->accumulateConstantOffset(
      data_layout, offset);
  failed = !found_offset;
  return offset.getZExtValue();
}

static OffsetMap GetOffsets(llvm::Function *func) {
  OffsetMap offset;

  llvm::Argument *state_ptr = &*func->arg_begin();
  offset[state_ptr] = 0;

  // Identify and label loads/stores to the state structure.
  for (auto made_progress = true; made_progress; ) {
    made_progress = false;
    for (auto &block : *func) {
      for (auto &inst : block) {
        if (offset.count(&inst)) {
          continue;
        }

        if (auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst)) {
          auto base = gep->getPointerOperand();
          if (!offset.count(base)) {
            continue;
          }

          bool failed = true;
          auto gep_offset = GetOffsetFromBasePtr(gep, failed);
          if (!failed) {
            offset[gep] = gep_offset + offset[base];
            made_progress = true;
          }

        } else if (auto cast = llvm::dyn_cast<llvm::BitCastInst>(&inst)) {
          auto base = cast->getOperand(0);
          if (offset.count(base)) {
            offset[cast] = offset[base];
            made_progress = true;
          }

        } else if (auto load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
          auto ptr = load->getPointerOperand();
          if (offset.count(ptr)) {
            offset[load] = offset[ptr];
            made_progress = true;
          }

        } else if (auto store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
          auto ptr = store->getPointerOperand();
          if (offset.count(ptr)) {
            offset[store] = offset[ptr];
            made_progress = true;
          }

        } else if (auto phi = llvm::dyn_cast<llvm::PHINode>(&inst)) {
          if (!phi->getType()->isPointerTy()) {
            continue;
          }
          for (auto &op : phi->incoming_values()) {
            auto ptr = op.get();
            if (offset.count(ptr)) {
              offset[phi] = offset[ptr];
              made_progress = true;
              break;
            }
          }
        }
      }
    }
  }

  return offset;
}

// Perform block-local optimizations, including dead store and dead load
// elimination.
static void LocalOptimizeBlock(llvm::BasicBlock *block, OffsetMap &map) {
  llvm::DataLayout layout(block->getModule());
  std::unordered_map<size_t, llvm::LoadInst *> load_forwarding;

  BlockState state;
  state.live_on_entry.reset();
  state.killed_in_block.reset();

  RegSet local_dead;
  local_dead.reset();

  std::unordered_set<llvm::Instruction *> to_remove;
  for (auto inst_rev = block->rbegin(); inst_rev != block->rend(); ++inst_rev) {
    auto inst = &*inst_rev;

    // Call out to another function; need to be really conservative here until
    // we apply a global alias analysis.
    //
    // TODO(pag): Apply some ABI stuff here when dealing with calls to externals
    //            or calls through pointers.
    if (llvm::isa<llvm::CallInst>(inst)) {
      state.live_on_entry.reset();
      state.killed_in_block.reset();
      local_dead.reset();
      load_forwarding.clear();
    }

    if (!map.count(inst)) {
      continue;
    }

    auto offset = map[inst];
    size_t reg_num = gByteOffsetToRegOffset[offset];
    auto reg_size = gByteOffsetToRegSize[offset];

    if (auto load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
      auto &next_load = load_forwarding[reg_num];
      auto size = layout.getTypeStoreSize(load->getType());

      // Load-to-load forwarding.
      if (next_load && next_load->getType() == load->getType()) {
        next_load->replaceAllUsesWith(load);
        to_remove.insert(next_load);
      }

      next_load = load;
      state.live_on_entry.set(reg_num);
      local_dead.reset(reg_num);

    } else if (auto store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
      auto stored_val = inst->getOperand(0);
      auto stored_val_type = stored_val->getType();
      auto size = layout.getTypeStoreSize(stored_val_type);
      auto &next_load = load_forwarding[reg_num];

      // Dead store elimination.
      if (local_dead.test(reg_num)) {
        to_remove.insert(store);

      // Partial store, possible false write-after-read dependency, has to
      // revive the register.
      } else if (size != reg_size) {
        state.live_on_entry.set(reg_num);
        local_dead.reset(reg_num);

      // Full store, kills the reg.
      } else {
        state.live_on_entry.reset(reg_num);
        state.killed_in_block.set(reg_num);
        local_dead.set(reg_num);

        // Store-to-load forwarding.
        if (next_load && next_load->getType() == stored_val_type) {
          next_load->replaceAllUsesWith(stored_val);
          to_remove.insert(next_load);
        }
      }

      next_load = nullptr;
    }
  }

  for (auto dead_inst : to_remove) {
    dead_inst->eraseFromParent();
  }

  gBlockState[block] = state;
}

}  // namespace

void InitDeadRegisterEliminator(llvm::Module *module, size_t num_funcs,
                                size_t num_blocks) {
  gBlockState.reserve(num_blocks);

  llvm::DataLayout layout(module);

  auto state_type = ArchRegStateStructType();
  unsigned reg_offset = 0;
  unsigned total_size = 0;

  for (const auto field_type : state_type->elements()) {
    auto store_size = layout.getTypeStoreSize(field_type);
    for (size_t i = 0; i < store_size; ++i) {
      gByteOffsetToRegOffset.push_back(reg_offset);
      gByteOffsetToRegSize.push_back(store_size);
      gByteOffsetToBeginOffset.push_back(total_size);
    }
    total_size += store_size;
    ++reg_offset;
  }
}

void OptimizeFunction(llvm::Function *func) {
  llvm::legacy::FunctionPassManager func_pass_manager(func->getParent());
  func_pass_manager.add(llvm::createCFGSimplificationPass());
  func_pass_manager.add(llvm::createPromoteMemoryToRegisterPass());
  func_pass_manager.add(llvm::createReassociatePass());
  func_pass_manager.add(llvm::createInstructionCombiningPass());
  func_pass_manager.add(llvm::createDeadStoreEliminationPass());
  func_pass_manager.add(llvm::createDeadCodeEliminationPass());

  auto offsets = GetOffsets(func);
  for (auto &block : *func) {
    LocalOptimizeBlock(&block, offsets);
  }

  func_pass_manager.doInitialization();
  func_pass_manager.run(*func);
  func_pass_manager.doFinalization();

}