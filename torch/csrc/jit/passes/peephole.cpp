#include "torch/csrc/jit/passes/peephole.h"

#include "torch/csrc/jit/symbolic_variable.h"

#include "torch/csrc/jit/passes/dead_code_elimination.h"

namespace torch { namespace jit {

// The intent for this optimization pass is to catch all of the small, easy to
// catch peephole optimizations you might be interested in doing.
//
// Right now, it does:
//    - Eliminate no-op 'expand' nodes
//    - Simply x.t().t() to x
//
// TODO: Decide what kind of fixed point strategy we will have
void PeepholeOptimize(Block * block) {
  for (auto it = block->nodes().begin(); it != block->nodes().end(); ++it) {
    auto* node = *it;

    for (Block * sub_block : node->blocks()) {
        PeepholeOptimize(sub_block);
    }

    // XXX: remember that if you want to simplify an expression by combining multiple nodes
    // into a different one, then you need to check that they all belong to the given block
    if (node->matches("aten::expand(Tensor self, int[] size, *, int implicit) -> Tensor",
        /*with_const=*/attr::size)) {
      // x.expand(x.size()) == x
      if (auto input_type = node->namedInput(attr::self)->type()->cast<CompleteTensorType>()) {
        auto expanded_sizes = node->get<std::vector<int64_t>>(attr::size);
        if (expanded_sizes == input_type->sizes()) {
          node->output()->replaceAllUsesWith(node->namedInput(attr::self));
        }
      }
    } else if (node->matches("aten::t(Tensor self) -> Tensor")) {
      // x.t().t() == x
      Node *input_node = node->input()->node();
      if (input_node->matches("aten::t(Tensor self) -> Tensor")) {
        node->output()->replaceAllUsesWith(input_node->input());
      }
    } else if (node->matches("aten::type_as(Tensor self, Tensor other) -> Tensor")) {
      // x.type_as(y) == x iff x.type() == y.type()
      auto self_type = node->input(0)->type()->cast<CompleteTensorType>();
      auto other_type = node->input(1)->type()->cast<CompleteTensorType>();
      if (self_type && other_type &&
          self_type->scalarType() == other_type->scalarType() &&
          self_type->device() == other_type->device()) {
        node->output()->replaceAllUsesWith(node->input(0));
      }
    } else if (node->matches("aten::add(Tensor self, Tensor other, *, Scalar alpha) -> Tensor",
               /*with_const=*/attr::alpha)) {
      // z + x.mm(y) == z.addmm(x, y) == x.mm(y) + z
      if (node->get<at::Scalar>(attr::alpha).value().toDouble() == 1.) {
        // Look for mm from both sides of the add
        for (size_t mm_side = 0; mm_side < 2; mm_side++) {
          if (node->input(mm_side)->node()->matches("aten::mm(Tensor self, Tensor mat2) -> Tensor")) {
            WithInsertPoint guard(node);

            auto mm_node = node->input(mm_side)->node();
            SymbolicVariable add_mat(node->input(1 - mm_side));
            SymbolicVariable mat1(mm_node->input(0));
            SymbolicVariable mat2(mm_node->input(1));
            SymbolicVariable addmm_value = add_mat.addmm(mat1, mat2);

            // Copy shape information from output node
            ((Value*)addmm_value)->copyMetadata(node->output());
            node->output()->replaceAllUsesWith(addmm_value);
          }
        }
      }
    } else if(node->kind() == prim::TensorToNum || node->kind() == prim::ImplicitTensorToNum) {
      Node* input_node = node->input()->node();
      if (input_node->kind() == prim::NumToTensor) {
        node->output()->replaceAllUsesWith(input_node->input());
      }
    }
  }
}

void PeepholeOptimize(std::shared_ptr<Graph>& graph) {
  PeepholeOptimize(graph->block());
  // Eliminate dead code created by any peephole passes we've just done
  EliminateDeadCode(graph->block());
}

}}
