#include <iostream>
#include <cassert>
#include "seq/seq.h"

using namespace seq;
using namespace llvm;

static types::Type *argsType()
{
	return types::ArrayType::get(types::Str);
}

SeqModule::SeqModule() :
    BaseFunc(), scope(new Block()), argsVar(argsType()), initFunc(nullptr), strlenFunc(nullptr)
{
}

Block *SeqModule::getBlock()
{
	return scope;
}

Var *SeqModule::getArgVar()
{
	return &argsVar;
}

void SeqModule::resolveTypes()
{
	scope->resolveTypes();
}

static Function *makeCanonicalMainFunc(Function *realMain, Function *strlen)
{
#define LLVM_I32() IntegerType::getInt32Ty(context)
	LLVMContext& context = realMain->getContext();
	Module *module = realMain->getParent();

	types::ArrayType *arrType = types::ArrayType::get(types::Str);

	auto *func = cast<Function>(
	               module->getOrInsertFunction(
	                 "main",
	                 LLVM_I32(),
	                 LLVM_I32(),
	                 PointerType::get(IntegerType::getInt8PtrTy(context), 0)));

	auto argiter = func->arg_begin();
	Value *argc = argiter++;
	Value *argv = argiter;

	BasicBlock *entry = BasicBlock::Create(context, "entry", func);
	BasicBlock *loop = BasicBlock::Create(context, "loop", func);

	IRBuilder<> builder(entry);
	Value *len = builder.CreateZExt(argc, seqIntLLVM(context));
	Value *ptr = types::Str->alloc(len, entry);
	Value *arr = arrType->make(ptr, len, entry);
	builder.CreateBr(loop);

	builder.SetInsertPoint(loop);
	PHINode *control = builder.CreatePHI(IntegerType::getInt32Ty(context), 2, "i");
	Value *next = builder.CreateAdd(control, ConstantInt::get(LLVM_I32(), 1), "next");
	Value *cond = builder.CreateICmpSLT(control, argc);

	BasicBlock *body = BasicBlock::Create(context, "body", func);
	BranchInst *branch = builder.CreateCondBr(cond, body, body);  // we set false-branch below

	builder.SetInsertPoint(body);
	Value *arg = builder.CreateLoad(builder.CreateGEP(argv, control));
	Value *argLen = builder.CreateZExtOrTrunc(builder.CreateCall(strlen, arg), seqIntLLVM(context));
	Value *str = types::Str->make(arg, argLen, body);
	arrType->indexStore(nullptr, arr, control, str, body);
	builder.CreateBr(loop);

	control->addIncoming(ConstantInt::get(LLVM_I32(), 0), entry);
	control->addIncoming(next, body);

	BasicBlock *exit = BasicBlock::Create(context, "exit", func);
	branch->setSuccessor(1, exit);

	builder.SetInsertPoint(exit);
	builder.CreateCall(realMain, {arr});
	builder.CreateRet(ConstantInt::get(LLVM_I32(), 0));

	return func;
#undef LLVM_I32
}

void SeqModule::codegen(Module *module)
{
	if (func)
		return;

	resolveTypes();
	LLVMContext& context = module->getContext();
	this->module = module;

	types::Type *argsType = nullptr;
	Value *args = nullptr;
	argsType = types::ArrayType::get(types::Str);

	func = cast<Function>(
	         module->getOrInsertFunction(
	           "seq.main",
	           Type::getVoidTy(context),
	           argsType->getLLVMType(context)));

	auto argiter = func->arg_begin();
	args = argiter;

	/* preamble */
	preambleBlock = BasicBlock::Create(context, "preamble", func);
	IRBuilder<> builder(preambleBlock);

	initFunc = cast<Function>(module->getOrInsertFunction("seq_init", Type::getVoidTy(context)));
	initFunc->setCallingConv(CallingConv::C);
	builder.CreateCall(initFunc);

	strlenFunc = cast<Function>(module->getOrInsertFunction("strlen", Type::getIntNTy(context, 8*sizeof(size_t)), IntegerType::getInt8PtrTy(context)));
	strlenFunc->setCallingConv(CallingConv::C);

	assert(argsType != nullptr);
	argsVar.store(this, args, preambleBlock);

	BasicBlock *entry = BasicBlock::Create(context, "entry", func);
	BasicBlock *block = entry;

	scope->codegen(block);

	builder.SetInsertPoint(block);
	builder.CreateRetVoid();

	builder.SetInsertPoint(preambleBlock);
	builder.CreateBr(entry);

	func = makeCanonicalMainFunc(func, strlenFunc);
}

void SeqModule::execute(const std::vector<std::string>& args, bool debug)
{
	LLVMContext& context = getLLVMContext();
	InitializeNativeTarget();
	InitializeNativeTargetAsmPrinter();

	std::unique_ptr<Module> owner(new Module("seq", context));
	Module *module = owner.get();
	module->setTargetTriple(EngineBuilder().selectTarget()->getTargetTriple().str());
	module->setDataLayout(EngineBuilder().selectTarget()->createDataLayout());

	codegen(module);

	if (verifyModule(*module, &errs())) {
		if (debug)
			errs() << *module;
		assert(0);
	}

	std::unique_ptr<legacy::PassManager> pm(new legacy::PassManager());
	std::unique_ptr<legacy::FunctionPassManager> fpm(new legacy::FunctionPassManager(module));

	unsigned optLevel = 3;
	unsigned sizeLevel = 0;
	PassManagerBuilder builder;

	if (!debug) {
		builder.OptLevel = optLevel;
		builder.SizeLevel = sizeLevel;
		builder.Inliner = createFunctionInliningPass(optLevel, sizeLevel, false);
		builder.DisableUnitAtATime = false;
		builder.DisableUnrollLoops = false;
		builder.LoopVectorize = true;
		builder.SLPVectorize = true;
	}

	builder.MergeFunctions = true;
	addCoroutinePassesToExtensionPoints(builder);
	builder.populateModulePassManager(*pm);
	builder.populateFunctionPassManager(*fpm);

	fpm->doInitialization();
	for (Function &f : *module)
		fpm->run(f);
	fpm->doFinalization();

	pm->run(*module);

	if (verifyModule(*module, &errs())) {
		if (debug)
			errs() << *module;
		assert(0);
	}

	if (debug)
		errs() << *module;

	EngineBuilder EB(std::move(owner));
	EB.setMCJITMemoryManager(make_unique<SectionMemoryManager>());
	EB.setUseOrcMCJITReplacement(true);
	ExecutionEngine *eng = EB.create();

	assert(initFunc);
	assert(strlenFunc);
	eng->addGlobalMapping(initFunc, (void *)seq_init);
	eng->addGlobalMapping(strlenFunc, (void *)strlen);

	typedef void (*Main)(const int, const char **);
	auto main = (Main)eng->getPointerToFunction(func);
	auto argc = (int)args.size();
	auto *argv = new const char *[argc];
	for (int i = 0; i < argc; i++)
		argv[i] = args[i].c_str();
	main(argc, argv);
}

LLVMContext& seq::getLLVMContext()
{
	static LLVMContext context;
	return context;
}