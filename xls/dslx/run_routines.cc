// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/run_routines.h"

#include <random>

#include "xls/dslx/bindings.h"
#include "xls/dslx/command_line_utils.h"
#include "xls/dslx/error_printer.h"
#include "xls/dslx/ir_converter.h"
#include "xls/dslx/mangle.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/typecheck.h"
#include "xls/ir/random_value.h"

namespace xls::dslx {
namespace {
// A few constants relating to the number of spaces to use in text formatting
// our test-runner output.
constexpr int kUnitSpaces = 7;
constexpr int kQuickcheckSpaces = 15;
}  // namespace

absl::StatusOr<IrJit*> JitComparator::GetOrCompileJitFunction(
    std::string ir_name, xls::Function* ir_function) {
  auto it = jit_cache_.find(ir_name);
  if (it != jit_cache_.end()) {
    return it->second.get();
  }
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<IrJit> jit, IrJit::Create(ir_function));
  IrJit* result = jit.get();
  jit_cache_[ir_name] = std::move(jit);
  return result;
}

absl::Status JitComparator::RunComparison(
    Package* ir_package, dslx::Function* f, absl::Span<InterpValue const> args,
    const SymbolicBindings* symbolic_bindings, const InterpValue& got) {
  XLS_RET_CHECK(ir_package != nullptr);

  XLS_ASSIGN_OR_RETURN(
      std::string ir_name,
      MangleDslxName(f->identifier(), f->GetFreeParametricKeySet(), f->owner(),
                     symbolic_bindings));

  auto get_result = ir_package->GetFunction(ir_name);

  // The (converted) IR package does not include specializations of parametric
  // functions that are only called from test code, so not finding the function
  // may be benign.
  //
  // TODO(amfv): 2021-03-18 Extend IR conversion to include those functions.
  if (!get_result.ok()) {
    XLS_LOG(WARNING) << "Could not find " << ir_name
                     << " function for JIT comparison";
    return absl::OkStatus();
  }

  xls::Function* ir_function = get_result.value();

  XLS_ASSIGN_OR_RETURN(IrJit * jit,
                       GetOrCompileJitFunction(ir_name, ir_function));

  XLS_ASSIGN_OR_RETURN(std::vector<Value> ir_args,
                       InterpValue::ConvertValuesToIr(args));

  XLS_ASSIGN_OR_RETURN(Value jit_value, jit->Run(ir_args));

  // Convert the interpreter value to an IR value so we can compare it.
  //
  // Note this conversion is lossy, but that's ok because we're just looking for
  // mismatches.
  XLS_ASSIGN_OR_RETURN(Value interp_ir_value, got.ConvertToIr());

  if (interp_ir_value != jit_value) {
    return absl::InternalError(absl::StrFormat(
        "JIT produced a different value from the interpreter for %s; JIT: %s "
        "interpreter: %s",
        ir_function->name(), jit_value.ToString(), interp_ir_value.ToString()));
  }
  return absl::OkStatus();
}

static bool TestMatchesFilter(absl::string_view test_name,
                              absl::optional<absl::string_view> test_filter) {
  if (!test_filter.has_value()) {
    return true;
  }
  // TODO(leary): 2019-08-28 Implement wildcards.
  return test_name == *test_filter;
}

absl::StatusOr<QuickCheckResults> DoQuickCheck(xls::Function* xls_function,
                                               std::string ir_name,
                                               JitComparator* jit_comparator,
                                               int64_t seed,
                                               int64_t num_tests) {
  XLS_ASSIGN_OR_RETURN(IrJit * jit, jit_comparator->GetOrCompileJitFunction(
                                        std::move(ir_name), xls_function));

  QuickCheckResults results;
  std::minstd_rand rng_engine(seed);

  for (int i = 0; i < num_tests; i++) {
    results.arg_sets.push_back(
        RandomFunctionArguments(xls_function, &rng_engine));
    XLS_ASSIGN_OR_RETURN(xls::Value result, jit->Run(results.arg_sets.back()));
    results.results.push_back(result);
    if (result.IsAllZeros()) {
      // We were able to falsify the xls_function (predicate), bail out early
      // and present this evidence.
      break;
    }
  }

  return results;
}

static absl::Status RunQuickCheck(JitComparator* jit_comparator,
                                  Package* ir_package, QuickCheck* quickcheck,
                                  TypeInfo* type_info, int64_t seed) {
  Function* fn = quickcheck->f();
  XLS_ASSIGN_OR_RETURN(
      std::string ir_name,
      MangleDslxName(fn->identifier(), fn->GetFreeParametricKeySet(),
                     fn->owner()));
  XLS_ASSIGN_OR_RETURN(xls::Function * ir_function,
                       ir_package->GetFunction(ir_name));

  XLS_ASSIGN_OR_RETURN(
      QuickCheckResults qc_results,
      DoQuickCheck(ir_function, std::move(ir_name), jit_comparator, seed,
                   quickcheck->test_count()));
  const auto& [arg_sets, results] = qc_results;
  XLS_ASSIGN_OR_RETURN(Bits last_result, results.back().GetBitsWithStatus());
  if (!last_result.IsZero()) {
    // Did not find a falsifying example.
    return absl::OkStatus();
  }

  const std::vector<Value>& last_argset = arg_sets.back();
  XLS_ASSIGN_OR_RETURN(FunctionType * fn_type,
                       type_info->GetItemAs<FunctionType>(fn));
  const std::vector<std::unique_ptr<ConcreteType>>& params = fn_type->params();

  std::vector<InterpValue> dslx_argset;
  for (int64_t i = 0; i < params.size(); ++i) {
    const ConcreteType& arg_type = *params[i];
    const Value& value = last_argset[i];
    XLS_ASSIGN_OR_RETURN(InterpValue interp_value,
                         ValueToInterpValue(value, &arg_type));
    dslx_argset.push_back(interp_value);
  }
  std::string dslx_argset_str = absl::StrJoin(
      dslx_argset, ", ", [](std::string* out, const InterpValue& v) {
        absl::StrAppend(out, v.ToString());
      });
  return FailureErrorStatus(
      fn->span(),
      absl::StrFormat("Found falsifying example after %d tests: [%s]",
                      results.size(), dslx_argset_str));
}

using HandleError = const std::function<void(
    const absl::Status&, absl::string_view test_name, bool is_quickcheck)>;

static absl::Status RunQuickChecksIfJitEnabled(
    Module* entry_module, TypeInfo* type_info, JitComparator* jit_comparator,
    Package* ir_package, absl::optional<int64_t> seed,
    const HandleError& handle_error) {
  if (jit_comparator == nullptr) {
    std::cerr << "[ SKIPPING QUICKCHECKS  ] (JIT is disabled)";
    return absl::OkStatus();
  }
  if (!seed.has_value()) {
    // Note: we *want* to *provide* non-determinism by default. See
    // https://abseil.io/docs/cpp/guides/random#stability-of-generated-sequences
    // for rationale.
    seed = static_cast<int64_t>(getpid()) * static_cast<int64_t>(time(nullptr));
  }
  std::cerr << absl::StreamFormat("[ SEED %*d ]", kQuickcheckSpaces + 1, *seed)
            << std::endl;
  for (QuickCheck* quickcheck : entry_module->GetQuickChecks()) {
    const std::string& test_name = quickcheck->identifier();
    std::cerr << "[ RUN QUICKCHECK        ] " << test_name
              << " count: " << quickcheck->test_count() << std::endl;
    absl::Status status =
        RunQuickCheck(jit_comparator, ir_package, quickcheck, type_info, *seed);
    if (!status.ok()) {
      handle_error(status, test_name, /*is_quickcheck=*/true);
    } else {
      std::cerr << "[                    OK ] " << test_name << std::endl;
    }
  }
  std::cerr << absl::StreamFormat(
                   "[=======================] %d quickcheck(s) ran.",
                   entry_module->GetQuickChecks().size())
            << std::endl;
  return absl::OkStatus();
}

absl::StatusOr<bool> ParseAndTest(absl::string_view program,
                                  absl::string_view module_name,
                                  absl::string_view filename,
                                  absl::Span<const std::string> dslx_paths,
                                  absl::optional<absl::string_view> test_filter,
                                  bool trace_all, JitComparator* jit_comparator,
                                  absl::optional<int64_t> seed) {
  int64_t ran = 0;
  int64_t failed = 0;
  int64_t skipped = 0;

  auto handle_error = [&](const absl::Status& status,
                          absl::string_view test_name, bool is_quickcheck) {
    XLS_VLOG(1) << "Handling error; status: " << status
                << " test_name: " << test_name;
    absl::StatusOr<PositionalErrorData> data_or =
        GetPositionalErrorData(status);
    std::string suffix;
    if (data_or.ok()) {
      const auto& data = data_or.value();
      XLS_CHECK_OK(PrintPositionalError(data.span, data.GetMessageWithType(),
                                        std::cerr));
    } else {
      // If we can't extract positional data we log the error and put the error
      // status into the "failed" prompted.
      XLS_LOG(ERROR) << "Internal error: " << status;
      suffix = absl::StrCat(": internal error: ", status.ToString());
    }
    std::string spaces((is_quickcheck ? kQuickcheckSpaces : kUnitSpaces), ' ');
    std::cerr << absl::StreamFormat("[ %sFAILED ] %s%s", spaces, test_name,
                                    suffix)
              << std::endl;
    failed += 1;
  };

  ImportData import_data;
  absl::StatusOr<TypecheckedModule> tm_or = ParseAndTypecheck(
      program, filename, module_name, &import_data, dslx_paths);
  if (!tm_or.ok()) {
    if (TryPrintError(tm_or.status())) {
      return true;
    }
    return tm_or.status();
  }
  Module* entry_module = tm_or.value().module;

  // If JIT comparisons are "on", we register a post-evaluation hook to compare
  // with the interpreter.
  std::unique_ptr<Package> ir_package;
  Interpreter::PostFnEvalHook post_fn_eval_hook;
  if (jit_comparator != nullptr) {
    XLS_ASSIGN_OR_RETURN(ir_package,
                         ConvertModuleToPackage(entry_module, &import_data,
                                                /*emit_positions=*/true,
                                                /*traverse_tests=*/true));
    post_fn_eval_hook = [&ir_package, jit_comparator](
                            Function* f, absl::Span<const InterpValue> args,
                            const SymbolicBindings* symbolic_bindings,
                            const InterpValue& got) {
      return jit_comparator->RunComparison(ir_package.get(), f, args,
                                           symbolic_bindings, got);
    };
  }

  auto typecheck_callback = [&import_data, &dslx_paths](Module* module) {
    return CheckModule(module, &import_data, dslx_paths);
  };

  Interpreter interpreter(entry_module, typecheck_callback, dslx_paths,
                          &import_data, /*trace_all=*/trace_all,
                          post_fn_eval_hook);

  // Run unit tests.
  for (const std::string& test_name : entry_module->GetTestNames()) {
    if (!TestMatchesFilter(test_name, test_filter)) {
      skipped += 1;
      continue;
    }

    ran += 1;
    std::cerr << "[ RUN UNITTEST  ] " << test_name << std::endl;
    absl::Status status = interpreter.RunTest(test_name);
    if (status.ok()) {
      std::cerr << "[            OK ]" << std::endl;
    } else {
      handle_error(status, test_name, /*is_quickcheck=*/false);
    }
  }

  std::cerr << absl::StreamFormat(
                   "[===============] %d test(s) ran; %d failed; %d skipped.",
                   ran, failed, skipped)
            << std::endl;

  // Run quickchecks, but only if the JIT is enabled.
  if (!entry_module->GetQuickChecks().empty()) {
    XLS_RETURN_IF_ERROR(RunQuickChecksIfJitEnabled(
        entry_module, interpreter.current_type_info(), jit_comparator,
        ir_package.get(), seed, handle_error));
  }

  return failed != 0;
}

}  // namespace xls::dslx