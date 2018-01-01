/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Build with `make -f JoinLoopTestMakefile`, compare the output
// with the one generated by the `generate_loop_ref.py` script.

#include "JoinLoop.h"

#include <glog/logging.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_os_ostream.h>

#include <memory>
#include <vector>

extern "C" void print_iterators(const int64_t i, const int64_t j, const int64_t k) {
  printf("%ld, %ld, %ld\n", i, j, k);
}

namespace {

llvm::LLVMContext g_global_context;

void verify_function_ir(const llvm::Function* func) {
  std::stringstream err_ss;
  llvm::raw_os_ostream err_os(err_ss);
  if (llvm::verifyFunction(*func, &err_os)) {
    func->dump();
    LOG(FATAL) << err_ss.str();
  }
}

llvm::Value* emit_external_call(const std::string& fname,
                                llvm::Type* ret_type,
                                const std::vector<llvm::Value*> args,
                                llvm::Module* module,
                                llvm::IRBuilder<>& builder) {
  std::vector<llvm::Type*> arg_types;
  for (const auto arg : args) {
    arg_types.push_back(arg->getType());
  }
  auto func_ty = llvm::FunctionType::get(ret_type, arg_types, false);
  auto func_p = module->getOrInsertFunction(fname, func_ty);
  CHECK(func_p);
  llvm::Value* result = builder.CreateCall(func_p, args);
  // check the assumed type
  CHECK_EQ(result->getType(), ret_type);
  return result;
}

llvm::Function* create_loop_test_function(llvm::LLVMContext& context,
                                          llvm::Module* module,
                                          const std::vector<JoinLoop>& join_loops) {
  std::vector<llvm::Type*> argument_types;
  const auto ft = llvm::FunctionType::get(llvm::Type::getVoidTy(context), argument_types, false);
  const auto func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "loop_test_func", module);
  const auto entry_bb = llvm::BasicBlock::Create(context, "entry", func);
  const auto exit_bb = llvm::BasicBlock::Create(context, "exit", func);
  llvm::IRBuilder<> builder(context);
  builder.SetInsertPoint(exit_bb);
  builder.CreateRetVoid();
  const auto loop_body_bb = JoinLoop::codegen(
      join_loops,
      [&builder, module](const std::vector<llvm::Value*>& iterators) {
        const auto loop_body_bb =
            llvm::BasicBlock::Create(builder.getContext(), "loop_body", builder.GetInsertBlock()->getParent());
        builder.SetInsertPoint(loop_body_bb);
        const std::vector<llvm::Value*> args(iterators.begin() + 1, iterators.end());
        emit_external_call("print_iterators", llvm::Type::getVoidTy(builder.getContext()), args, module, builder);
        return loop_body_bb;
      },
      nullptr,
      exit_bb,
      builder);
  builder.SetInsertPoint(entry_bb);
  builder.CreateBr(loop_body_bb);
  verify_function_ir(func);
  return func;
}

std::unique_ptr<llvm::Module> create_loop_test_module() {
  return llvm::make_unique<llvm::Module>("Nested loops JIT", g_global_context);
}

std::pair<void*, std::unique_ptr<llvm::ExecutionEngine>> native_codegen(std::unique_ptr<llvm::Module>& module,
                                                                        llvm::Function* func) {
  llvm::ExecutionEngine* execution_engine{nullptr};

  auto init_err = llvm::InitializeNativeTarget();
  CHECK(!init_err);

  llvm::InitializeAllTargetMCs();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  std::string err_str;
  llvm::EngineBuilder eb(std::move(module));
  eb.setErrorStr(&err_str);
  eb.setEngineKind(llvm::EngineKind::JIT);
  llvm::TargetOptions to;
  to.EnableFastISel = true;
  eb.setTargetOptions(to);
  execution_engine = eb.create();
  CHECK(execution_engine);

  execution_engine->finalizeObject();
  auto native_code = execution_engine->getPointerToFunction(func);

  CHECK(native_code);
  return {native_code, std::unique_ptr<llvm::ExecutionEngine>(execution_engine)};
}

std::vector<JoinLoop> generate_descriptors(const unsigned mask,
                                           const unsigned cond_mask,
                                           const std::vector<int64_t>& upper_bounds) {
  std::vector<JoinLoop> join_loops;
  size_t cond_idx{0};
  for (size_t i = 0; i < upper_bounds.size(); ++i) {
    if (mask & (1 << i)) {
      const bool cond_is_true = cond_mask & (1 << cond_idx);
      join_loops.emplace_back(JoinLoopKind::Singleton,
                              JoinType::INNER,
                              [i, cond_is_true](const std::vector<llvm::Value*>& v) {
                                CHECK_EQ(i + 1, v.size());
                                CHECK(!v.front());
                                JoinLoopDomain domain{0};
                                domain.slot_lookup_result = cond_is_true ? ll_int(int64_t(99), g_global_context)
                                                                         : ll_int(int64_t(-1), g_global_context);
                                return domain;
                              },
                              nullptr,
                              nullptr,
                              "i" + std::to_string(i));
      ++cond_idx;
    } else {
      const auto upper_bound = upper_bounds[i];
      join_loops.emplace_back(JoinLoopKind::UpperBound,
                              JoinType::INNER,
                              [i, upper_bound](const std::vector<llvm::Value*>& v) {
                                CHECK_EQ(i + 1, v.size());
                                CHECK(!v.front());
                                JoinLoopDomain domain{0};
                                domain.upper_bound = ll_int<int64_t>(upper_bound, g_global_context);
                                return domain;
                              },
                              nullptr,
                              nullptr,
                              "i" + std::to_string(i));
    }
  }
  return join_loops;
}

}  // namespace

int main() {
  std::vector<int64_t> upper_bounds{5, 3, 9};
  for (unsigned mask = 0; mask < static_cast<unsigned>(1 << upper_bounds.size()); ++mask) {
    const unsigned mask_bitcount = __builtin_popcount(mask);
    for (unsigned cond_mask = 0; cond_mask < static_cast<unsigned>(1 << mask_bitcount); ++cond_mask) {
      auto module = create_loop_test_module();
      const auto join_loops = generate_descriptors(mask, cond_mask, upper_bounds);
      const auto function = create_loop_test_function(g_global_context, module.get(), join_loops);
      const auto& func_and_ee = native_codegen(module, function);
      reinterpret_cast<int64_t (*)()>(func_and_ee.first)();
    }
  }
  return 0;
}
