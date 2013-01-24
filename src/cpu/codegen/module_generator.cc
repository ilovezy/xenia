/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <xenia/cpu/codegen/module_generator.h>

#include <llvm/DIBuilder.h>
#include <llvm/Linker.h>
#include <llvm/PassManager.h>
#include <llvm/DebugInfo.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <xenia/cpu/ppc.h>
#include <xenia/cpu/codegen/function_generator.h>

#include "cpu/cpu-private.h"


using namespace llvm;
using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::codegen;
using namespace xe::cpu::sdb;
using namespace xe::kernel;


ModuleGenerator::ModuleGenerator(
    xe_memory_ref memory, ExportResolver* export_resolver,
    UserModule* module, SymbolDatabase* sdb,
    LLVMContext* context, Module* gen_module) {
  memory_ = xe_memory_retain(memory);
  export_resolver_ = export_resolver;
  module_ = module;
  sdb_ = sdb;
  context_ = context;
  gen_module_ = gen_module;
  di_builder_ = NULL;
}

ModuleGenerator::~ModuleGenerator() {
  for (std::map<uint32_t, CodegenFunction*>::iterator it =
       functions_.begin(); it != functions_.end(); ++it) {
    delete it->second;
  }

  delete di_builder_;
  xe_memory_release(memory_);
}

int ModuleGenerator::Generate() {
  std::string error_message;

  // Setup a debug info builder.
  // This is used when creating any debug info. We may want to go more
  // fine grained than this, but for now it's something.
  xechar_t dir[2048];
  XEIGNORE(xestrcpy(dir, XECOUNT(dir), module_->path()));
  xechar_t* slash = xestrrchr(dir, '/');
  if (slash) {
    *(slash + 1) = 0;
  }
  di_builder_ = new DIBuilder(*gen_module_);
  di_builder_->createCompileUnit(
      0,
      StringRef(module_->name()),
      StringRef(dir),
      StringRef("xenia"),
      true,
      StringRef(""),
      0);
  cu_ = (MDNode*)di_builder_->getCU();

  // Add export wrappers.
  //

  // Add all functions.
  // We do two passes - the first creates the function signature and global
  // value (so that we can call it), the second actually builds the function.
  std::vector<FunctionSymbol*> functions;
  if (!sdb_->GetAllFunctions(functions)) {
    for (std::vector<FunctionSymbol*>::iterator it = functions.begin();
         it != functions.end(); ++it) {
      FunctionSymbol* fn = *it;
      switch (fn->type) {
      case FunctionSymbol::User:
        PrepareFunction(fn);
        break;
      case FunctionSymbol::Kernel:
        if (fn->kernel_export && fn->kernel_export->IsImplemented()) {
          AddPresentImport(fn);
        } else {
          AddMissingImport(fn);
        }
        break;
      default:
        XEASSERTALWAYS();
        break;
      }
    }
  }

  // Build out all the user functions.
  for (std::map<uint32_t, CodegenFunction*>::iterator it =
       functions_.begin(); it != functions_.end(); ++it) {
    BuildFunction(it->second);
  }

  di_builder_->finalize();

  return 0;
}

ModuleGenerator::CodegenFunction* ModuleGenerator::GetCodegenFunction(
    uint32_t address) {
  std::map<uint32_t, CodegenFunction*>::iterator it = functions_.find(address);
  if (it != functions_.end()) {
    return it->second;
  }
  return NULL;
}

Function* ModuleGenerator::CreateFunctionDefinition(const char* name) {
  Module* m = gen_module_;
  LLVMContext& context = m->getContext();

  std::vector<Type*> args;
  args.push_back(PointerType::getUnqual(Type::getInt8Ty(context)));
  Type* return_type = Type::getVoidTy(context);

  FunctionType* ft = FunctionType::get(return_type,
                                       ArrayRef<Type*>(args), false);
  Function* f = cast<Function>(m->getOrInsertFunction(
      StringRef(name), ft));
  f->setVisibility(GlobalValue::DefaultVisibility);

  // Indicate that the function will never be unwound with an exception.
  // If we ever support native exception handling we may need to remove this.
  f->doesNotThrow();

  // May be worth trying the X86_FastCall, as we only need state in a register.
  //f->setCallingConv(CallingConv::Fast);
  f->setCallingConv(CallingConv::C);

  Function::arg_iterator fn_args = f->arg_begin();
  // 'state'
  Value* fn_arg = fn_args++;
  fn_arg->setName("state");
  f->setDoesNotAlias(1);
  // 'state' should try to be in a register, if possible.
  // TODO(benvanik): verify that's a good idea.
  // f->getArgumentList().begin()->addAttr(
  //     Attribute::get(context, AttrBuilder().addAttribute(Attribute::InReg)));

  return f;
};

void ModuleGenerator::AddMissingImport(FunctionSymbol* fn) {
  Module *m = gen_module_;
  LLVMContext& context = m->getContext();

  // Create the function (and setup args/attributes/etc).
  Function* f = CreateFunctionDefinition(fn->name);

  // TODO(benvanik): log errors.
  BasicBlock* block = BasicBlock::Create(context, "entry", f);
  IRBuilder<> builder(block);
  builder.CreateRetVoid();

  OptimizeFunction(m, f);

  //GlobalAlias *alias = new GlobalAlias(f->getType(), GlobalValue::InternalLinkage, name, f, m);
  // printf("   F %.8X %.8X %.3X (%3d) %s %s\n",
  //        info->value_address, info->thunk_address, info->ordinal,
  //        info->ordinal, implemented ? "  " : "!!", name);
  // For values:
  // printf("   V %.8X          %.3X (%3d) %s %s\n",
  //        info->value_address, info->ordinal, info->ordinal,
  //        implemented ? "  " : "!!", name);
}

void ModuleGenerator::AddPresentImport(FunctionSymbol* fn) {
  // Module *m = gen_module_;
  // LLVMContext& context = m->getContext();

  // TODO(benvanik): add import thunk code.
}

void ModuleGenerator::PrepareFunction(FunctionSymbol* fn) {
  // Module* m = gen_module_;
  // LLVMContext& context = m->getContext();

  // Create the function (and setup args/attributes/etc).
  Function* f = CreateFunctionDefinition(fn->name);

  // Setup our codegen wrapper to keep all the pointers together.
  CodegenFunction* cgf = new CodegenFunction();
  cgf->symbol = fn;
  cgf->function_type = f->getFunctionType();
  cgf->function = f;
  functions_.insert(std::pair<uint32_t, CodegenFunction*>(
      fn->start_address, cgf));
}

void ModuleGenerator::BuildFunction(CodegenFunction* cgf) {
  FunctionSymbol* fn = cgf->symbol;

  printf("%s:\n", fn->name);

  // Setup the generation context.
  FunctionGenerator fgen(
      memory_, sdb_, fn, context_, gen_module_, cgf->function);

  // Run through and generate each basic block.
  fgen.GenerateBasicBlocks();

  // Run the optimizer on the function.
  // Doing this here keeps the size of the IR small and speeds up the later
  // passes.
  OptimizeFunction(gen_module_, cgf->function);
}

void ModuleGenerator::OptimizeFunction(Module* m, Function* fn) {
  FunctionPassManager pm(m);
  if (FLAGS_optimize_ir_functions) {
    PassManagerBuilder pmb;
    pmb.OptLevel      = 3;
    pmb.SizeLevel     = 0;
    pmb.Inliner       = createFunctionInliningPass();
    pmb.Vectorize     = true;
    pmb.LoopVectorize = true;
    pmb.populateFunctionPassManager(pm);
  }
  pm.add(createVerifierPass());
  pm.run(*fn);
}
