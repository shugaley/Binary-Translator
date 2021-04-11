
#include "Translator.h"

#include <iostream>
#include <stack>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Value.h"

namespace {

enum NumInstructions {
    NUM_PUSH = 0,
    NUM_PUSH_R,
    NUM_POP_R,
    NUM_MOV,
    NUM_MOV_R,
    NUM_MOV_RP,
    NUM_MOV_PR,
    NUM_CALL,
    NUM_RET,
    NUM_EXIT,
    NUM_WRITE,
    NUM_READ,

    NUM_ADD,
    NUM_SUB,
    NUM_IMUL,
    NUM_IDIV,
    NUM_ADD_R,
    NUM_SUB_R,
    NUM_IMUL_R,
    NUM_IDIV_R,
    NUM_INC,
    NUM_DEC,

    NUM_CMP,
    NUM_CMP_R,
    NUM_JMP,
    NUM_JG,
    NUM_JGE,
    NUM_JL,
    NUM_JLE,
    NUM_JE,
    NUM_JNE,
    N_INST,
};

enum IdInstructions {
    PUSH = 0x68,
    PUSH_R = 0x50,
    POP_R = 0x58,
    MOV = 0xB8,
    MOV_R = 0xBB,
    MOV_RP = 0xBC,
    MOV_PR = 0xBD,
    CALL = 0xE8,
    RET = 0xC3,
    EXIT = 0xFF,
    WRITE = 0xE6,
    READ = 0xE4,

    ADD = 0x01,
    SUB = 0x29,
    IMUL = 0x6B,
    IDIV = 0xF6,
    ADD_R = 0x03,
    SUB_R = 0x2B,
    IMUL_R = 0xAF,
    IDIV_R = 0xF7,
    INC = 0x40,
    DEC = 0x48,

    CMP = 0x3B,
    CMP_R = 0x39,
    JMP = 0xE9,
    JG = 0x8F,
    JGE = 0x8D,
    JL = 0x8C,
    JLE = 0x8E,
    JE = 0x75,
    JNE = 0x85,
};

enum Registers {
    RAX,
    RBX,
    RCX,
    RDX,
    NREGS,
};

bool IsRegRegInst(int inst)
{
    if (inst == ADD_R  || inst == SUB_R  ||
        inst == IMUL_R || inst == IDIV_R ||
        inst == CMP_R  || inst == MOV_R  ||
        inst == MOV_RP || inst == MOV_PR)
        return true;

    return false;
}

bool IsJumpInst(int inst)
{
    if (inst == JMP || inst == JG || inst == JGE || inst == JL ||
        inst == JLE || inst == JE || inst == JNE)
        return true;
    return false;
}
} // namespace

class Translator::Impl {
private:
    std::string pathToInputFile_;

    unsigned char* bytecode_ = nullptr;
    size_t sizeByteCode_ = 0;
    size_t PC_ = 0;
    std::string output_;

    llvm::LLVMContext context_;
    llvm::Module* module_       = nullptr;
    llvm::Function* curFunc_    = nullptr;
    llvm::IRBuilder<>* builder_ = nullptr;

    std::stack<llvm::Value *> stackIR_;
    llvm::Value* curCmpValue_   = nullptr;

    // llvm::GlobalVariable* regs_ = nullptr;
    // llvm::ArrayType* regsType_  = nullptr;

    struct GlobalArray {
        size_t size = 0;
        std::string name{};
        llvm::ArrayType* type       = nullptr;
        llvm::GlobalVariable* array = nullptr;
    };

    GlobalArray regs_ {
        .size = NREGS,
        .name = "regs_",
    };

    GlobalArray array_ {
        .size = 10,
        .name = "array_",
    };

    GlobalArray benchmarkResult_ {
        .size = N_INST + 1,
        .name = "nTacts",
    };

    // TODO GlobalArray

    struct BranchBB {
        llvm::BasicBlock* trueBB  = nullptr;
        size_t truePC = 0;

        llvm::BasicBlock* falseBB = nullptr;
        size_t falsePC = 0;
    };

    std::map <size_t, BranchBB> branchBBs_;
    std::map <size_t, llvm::Function*> functions_;

    llvm::Function* CreateFunc();
    BranchBB CreateBranchBB(size_t truePC, size_t falsePC);

    void ReadBytecode();
    void AllocByteCodeBuf();
    void CreateGlobalArray(GlobalArray& GA);

    void TranslateByteCode();
    void TranslateByteCodeExpression();
    void TranslateByteCodeJumps();
    void TranslateByteCodeCmp();
    void TranslateByteCodeIO();
    void TranslateByteCodeStack();
    void TranslateByteCodeCall();
    void TranslateByteCodeRet();
    void TranslateByteCodeExit();

    llvm::Function *GetFunction(size_t PC) const;
    llvm::BasicBlock* GetBB(size_t PC) const;
    void MovePC();

    int TranslateIDInst(const int keyID);
    void AddTact(const int numInst);
    void PrintBenchmarkResult();

public:
    Impl(char *const pathToInputFile) : pathToInputFile_(pathToInputFile) {}

    ~Impl()
    {
        delete[] bytecode_;
    }

    void Translate();
    void Preprocess();

    friend void Translator::Dump() const;

}; // class Translator::Impl

void Translator::Impl::Preprocess()
{
    ReadBytecode();

    // Create basic
    module_  = new llvm::Module("top", context_);
    builder_ = new llvm::IRBuilder(context_);

    llvm::FunctionType *funcType =
            llvm::FunctionType::get(builder_->getInt32Ty(), false);
    llvm::Function *mainFunc =
            llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                   "main", module_);

    llvm::BasicBlock *entryBB = llvm::BasicBlock::Create(context_, "entryBB",
                                                         mainFunc);

    builder_->SetInsertPoint(entryBB);
    curFunc_ = mainFunc;

    for (; PC_ < sizeByteCode_; MovePC()) {
        if (bytecode_[PC_] == CALL)
            if (GetFunction(PC_ + (char)bytecode_[PC_ + 1]) == nullptr)
                functions_.insert(std::make_pair
                    (PC_ + (char)bytecode_[PC_ + 1], CreateFunc()));

        if (IsJumpInst(bytecode_[PC_]))
            branchBBs_.insert(std::make_pair
                (PC_, CreateBranchBB(PC_ + (char)bytecode_[PC_ + 1], PC_ + 2)));
    }

    curFunc_ = mainFunc;
    PC_ = 0;
}

void Translator::Impl::Translate()
{
    CreateGlobalArray(regs_);
    CreateGlobalArray(array_);
    CreateGlobalArray(benchmarkResult_);
    TranslateByteCode();
}

void Translator::Impl::TranslateByteCode()
{
    llvm::Function* tmpFunc = nullptr;
    llvm::BasicBlock* tmpBB = nullptr;
    while (PC_ < sizeByteCode_) {

        tmpFunc = GetFunction(PC_);
        if (tmpFunc != nullptr)
            curFunc_ = tmpFunc;

        tmpBB = GetBB(PC_);
        if (tmpBB != nullptr)
            builder_->SetInsertPoint(tmpBB);

        AddTact(bytecode_[PC_]);

        switch (bytecode_[PC_]) {
        case ADD_R:
        case ADD:
        case SUB_R:
        case SUB:
        case IMUL_R:
        case IMUL:
        case IDIV_R:
        case IDIV:
        case INC:
        case DEC:
        case MOV:
        case MOV_R:
        case MOV_RP:
        case MOV_PR:
            TranslateByteCodeExpression();
            break;

        case JMP:
        case JG:
        case JGE:
        case JL:
        case JLE:
        case JE:
        case JNE:
            TranslateByteCodeJumps();
            break;

        case CMP:
        case CMP_R:
            TranslateByteCodeCmp();
            break;

        case WRITE:
        case READ:
            TranslateByteCodeIO();
            break;

        case PUSH:
        case PUSH_R:
        case POP_R:
            TranslateByteCodeStack();
            break;

        case CALL:
            TranslateByteCodeCall();
            break;

        case RET:
            TranslateByteCodeRet();
            break;

        case EXIT:
            TranslateByteCodeExit();
            break;

        default:
            throw std::runtime_error("TranslateByteCode():"
                                     "Unidefined instruction" +
                                     std::to_string(bytecode_[PC_]));
        }
    }
}

llvm::Function* Translator::Impl::CreateFunc()
{
    static size_t numFunc = 0;
    numFunc++;

    llvm::FunctionType *funcType =
        llvm::FunctionType::get(builder_->getVoidTy(), false);
    llvm::Function* function =
        llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                "Function" + std::to_string(numFunc), module_);

    curFunc_ = function;
    size_t startFuncPC = PC_ + (char)bytecode_[PC_ + 1];
    branchBBs_.insert(std::make_pair(startFuncPC, CreateBranchBB(startFuncPC,
                                                                 startFuncPC)));

    return function;
}

Translator::Impl::BranchBB Translator::Impl::CreateBranchBB(size_t truePC,
                                                            size_t falsePC)
{
    static size_t numBB = 0;
    numBB++;

    BranchBB branchBB;

    llvm::BasicBlock* tmpTrueBB = GetBB(truePC);
    if (tmpTrueBB == nullptr)
        branchBB.trueBB =
            llvm::BasicBlock::Create(context_, "trueBB" + std::to_string(numBB),
                                     curFunc_);
    else
        branchBB.trueBB = tmpTrueBB;
    branchBB.truePC = truePC;

    if (truePC == falsePC || bytecode_[PC_] == JMP)
        return branchBB;

    llvm::BasicBlock *tmpFalseBB = GetBB(falsePC);
    if (tmpFalseBB == nullptr)
        branchBB.falseBB =
            llvm::BasicBlock::Create(context_,
                                     "falseBB" + std::to_string(numBB),
                                     curFunc_);
    else
        branchBB.falseBB = tmpFalseBB;
    branchBB.falsePC = falsePC;

    return branchBB;
}

void Translator::Impl::TranslateByteCodeExpression()
{

    llvm::Value* pArg_1 = builder_->CreateConstGEP2_32(regs_.type, regs_.array,
                                                       0, bytecode_[PC_ + 1]);
    llvm::Value* arg_1 = builder_->CreateLoad(pArg_1);

    llvm::Value* arg_2 = nullptr;
    if (IsRegRegInst(bytecode_[PC_])) {
        llvm::Value *pArg_2 = builder_->CreateConstGEP2_32(regs_.type,
                                                           regs_.array, 0,
                                                           bytecode_[PC_ + 2]);
        arg_2 = builder_->CreateLoad(pArg_2);
    }
    else
        arg_2 = llvm::ConstantInt::get(builder_->getInt32Ty(),
                                       (char)bytecode_[PC_ + 2]);

    llvm::Value* res = nullptr;
    switch (bytecode_[PC_]) {
    case MOV_RP: {
        llvm::Value* pArg_2 = builder_->CreateGEP(array_.type, array_.array,
                                                arg_2, array_.name);
        res = builder_->CreateLoad(pArg_2);
        break;
    }
    case MOV_PR:
        pArg_1 = builder_->CreateGEP(array_.type, array_.array,
                                     arg_1, array_.name);
        // Load?
        res = arg_2;
        break;

    case ADD:
    case ADD_R:
        res = builder_->CreateAdd(arg_1, arg_2);
        break;

    case SUB:
    case SUB_R:
        res = builder_->CreateSub(arg_1, arg_2);
        break;

    case IMUL:
    case IMUL_R:
        res = builder_->CreateMul(arg_1, arg_2);
        break;

    case IDIV:
    case IDIV_R:
        res = builder_->CreateSDiv(arg_1, arg_2);
        break;

    case INC:
        arg_2 = llvm::ConstantInt::get(builder_->getInt32Ty(), 1);
        res = builder_->CreateAdd(arg_1, arg_2);
        break;

    case DEC:
        arg_2 = llvm::ConstantInt::get(builder_->getInt32Ty(), 1);
        res = builder_->CreateSub(arg_1, arg_2);
        break;

    case MOV:
    case MOV_R:
        res = arg_2;
        break;

    default:
        throw std::runtime_error("TranslateByteCodeExpression():"
                                 "Unidefined instruction" +
                                 std::to_string(bytecode_[PC_]));
    }

    builder_->CreateStore(res, pArg_1);

    MovePC();
}

void Translator::Impl::TranslateByteCodeJumps()
{
    AddTact(N_INST); //TODO Change arg to more beautiful
    llvm::BasicBlock* trueBB = GetBB(PC_ + (char)bytecode_[PC_ + 1]);
    llvm::BasicBlock *falseBB = GetBB(PC_ + 2);
    if (bytecode_[PC_] == JMP)
        builder_->CreateBr(trueBB);
    else
        builder_->CreateCondBr(curCmpValue_, trueBB, falseBB);

    MovePC();
}

void Translator::Impl::TranslateByteCodeCmp()
{
    llvm::Value *pArg_1 = builder_->CreateConstGEP2_32(regs_.type, regs_.array,
                                                       0, bytecode_[PC_ + 1]);
    llvm::Value *arg_1 = builder_->CreateLoad(pArg_1);

    llvm::Value *arg_2 = nullptr;
    if (IsRegRegInst(bytecode_[PC_]))
    {
        llvm::Value *pArg_2 = builder_->CreateConstGEP2_32(regs_.type,
                                                           regs_.array, 0,
                                                           bytecode_[PC_ + 2]);
        arg_2 = builder_->CreateLoad(pArg_2);
    }
    else
        arg_2 = llvm::ConstantInt::get(builder_->getInt32Ty(),
                                       (char)bytecode_[PC_ + 2]);

    llvm::CmpInst::Predicate predicate;
    switch (bytecode_[PC_ + 3]) {
        case JMP: return;
        case JG:  predicate = llvm::CmpInst::Predicate::ICMP_SGT;  break;
        case JGE: predicate = llvm::CmpInst::Predicate::ICMP_SGE;  break;
        case JL:  predicate = llvm::CmpInst::Predicate::ICMP_SLT;  break;
        case JLE: predicate = llvm::CmpInst::Predicate::ICMP_SLE;  break;
        case JE:  predicate = llvm::CmpInst::Predicate::ICMP_EQ;   break;
        case JNE: predicate = llvm::CmpInst::Predicate::ICMP_NE;   break;
        default:
            throw std::runtime_error("TranslateByteCodeCmp():"
                                     "Jump instruction is not found");
    }

    curCmpValue_ = builder_->CreateCmp(predicate, arg_1, arg_2, "resCmp");
    MovePC();
}

void Translator::Impl::TranslateByteCodeIO()
{
    std::vector<llvm::Type *> funcArgsTypes;
    funcArgsTypes.push_back(builder_->getInt8PtrTy());
    llvm::FunctionType *funcType =
                                 llvm::FunctionType::get(builder_->getInt32Ty(),
                                                         funcArgsTypes, true);

    std::string formatStr("%d");
    llvm::FunctionCallee func;
    llvm::Value* arg = nullptr;
    llvm::Value *pArg = builder_->CreateConstGEP2_32(regs_.type, regs_.array, 0,
                                                     bytecode_[PC_ + 1]);
    llvm::FunctionCallee printfReg = module_->getOrInsertFunction("printf",
                                                                  funcType);
    if (bytecode_[PC_] == WRITE) {
        arg = builder_->CreateLoad(pArg);
        func = printfReg;
        formatStr += "\n";
    }

    if (bytecode_[PC_] == READ) {
        arg = pArg;
        func = module_->getOrInsertFunction("scanf", funcType);
    }

    std::string regStr;
    switch (bytecode_[PC_ + 1]) {
    case RAX:
        regStr += "RAX: = ";
        break;
    case RBX:
        regStr += "RBX: = ";
        break;
    case RCX:
        regStr += "RCX: = ";
        break;
    case RDX:
        regStr += "RDX: = ";
        break;

    default:
        throw std::runtime_error("TranslateByteCodeIO():"
                                 "Undefined instruction" +
                                 std::to_string(bytecode_[PC_ + 1]));
    }
    llvm::Value *formatRegStrVal =
        builder_->CreateGlobalStringPtr(regStr, "IORegister");
    llvm::Value *formatStrVal =
        builder_->CreateGlobalStringPtr(formatStr, "IOformat");

    std::vector<llvm::Value *> args;
    args.push_back(formatRegStrVal);
    builder_->CreateCall(printfReg, args);

    args.clear();

    args.push_back(formatStrVal);
    args.push_back(arg);

    builder_->CreateCall(func, args);

    MovePC();
}

void Translator::Impl::TranslateByteCodeStack()
{
    llvm::Value* pArg = nullptr;
    llvm::Value* arg = nullptr;

    switch (bytecode_[PC_]) {
    case PUSH:
        arg = llvm::ConstantInt::get(builder_->getInt32Ty(),
                                     (char)bytecode_[PC_ + 1]);
        stackIR_.push(arg);
        break;

    case PUSH_R:
        pArg = builder_->CreateConstGEP2_32(regs_.type, regs_.array, 0,
                                            bytecode_[PC_ + 1]);
        arg = builder_->CreateLoad(pArg);
        stackIR_.push(arg);
        break;

    case POP_R:
        pArg = builder_->CreateConstGEP2_32(regs_.type, regs_.array, 0,
                                            bytecode_[PC_ + 1]);
        arg = stackIR_.top();
        builder_->CreateStore(arg, pArg);
        stackIR_.pop();
        break;

    default:
        throw std::runtime_error("TranslateByteCodeStack():"
                                 "Unidefined instruction"
                                 + std::to_string(bytecode_[PC_]));
    }

    MovePC();
}

void Translator::Impl::TranslateByteCodeCall()
{
    AddTact(N_INST);
    llvm::Function* function = GetFunction(PC_ + (char)bytecode_[PC_ + 1]);
    builder_->CreateCall(function);

    MovePC();
}

void Translator::Impl::TranslateByteCodeRet()
{
    builder_->CreateRetVoid();
    MovePC();
}

void Translator::Impl::TranslateByteCodeExit()
{
    PrintBenchmarkResult();
    builder_->CreateRet(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
    MovePC();
}

Translator::Translator(char *const pathToInputFile) :
    pImpl_(std::make_unique<Impl>(pathToInputFile)){};

Translator::~Translator() = default;

Translator::Translator(Translator &&) = default;

void Translator::Impl::ReadBytecode()
{
    FILE *inputFile = fopen(pathToInputFile_.c_str(), "rb");

    if (!inputFile)
        throw std::runtime_error("Simulator: Can`t open input file");

    fseek(inputFile, 0, SEEK_END);
    sizeByteCode_ = ftell(inputFile);
    fseek(inputFile, 0, SEEK_SET);

    AllocByteCodeBuf();

    fread(bytecode_, 1, sizeByteCode_, inputFile);

    fclose(inputFile);
}

void Translator::Impl::AllocByteCodeBuf()
{
    bytecode_ = new unsigned char[sizeByteCode_];
}

void Translator::Impl::CreateGlobalArray(GlobalArray& GA)
{
    GA.type = llvm::ArrayType::get(builder_->getInt32Ty(),
                                            GA.size);
    module_->getOrInsertGlobal(GA.name, GA.type);
    GA.array = module_->getNamedGlobal(GA.name);

    std::vector<llvm::Constant *> temp;
    for (size_t i = 0; i < GA.size; i++)
        temp.push_back(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));

    GA.array->setInitializer(llvm::ConstantArray::get(GA.type, temp));
}

void Translator::Impl::MovePC()
{
    switch (bytecode_[PC_]) {
    case ADD_R:
    case ADD:
    case SUB_R:
    case SUB:
    case IMUL_R:
    case IMUL:
    case IDIV_R:
    case IDIV:
    case MOV:
    case MOV_R:
    case CMP:
    case CMP_R:
        PC_ += 3;
        break;

    case JMP:
    case JG:
    case JGE:
    case JL:
    case JLE:
    case JE:
    case JNE:
    case WRITE:
    case READ:
    case PUSH:
    case PUSH_R:
    case POP_R:
    case CALL:
    case INC:
    case DEC:
        PC_ += 2;
        break;

    case RET:
    case EXIT:
        PC_ += 1;
        break;

    default:
        throw std::runtime_error("TranslateByteCode():Unidefined instruction" +
                                 std::to_string(bytecode_[PC_]));
    }
}

int Translator::Impl::TranslateIDInst(const int keyID)
{
    switch(keyID) {
    case ADD_R:         return NUM_ADD_R;
    case ADD:           return NUM_ADD;
    case SUB_R:         return NUM_SUB_R;
    case SUB:           return NUM_SUB;
    case IMUL_R:        return IMUL_R;
    case IMUL:          return NUM_IMUL;
    case IDIV_R:        return NUM_IDIV_R;
    case IDIV:          return NUM_IDIV;
    case INC:           return NUM_INC;
    case DEC:           return NUM_DEC;
    case MOV:           return NUM_MOV;
    case MOV_R:         return NUM_MOV_R;
    case MOV_RP:        return NUM_MOV_PR;
    case MOV_PR:        return NUM_MOV_PR;
    case JMP:           return NUM_JMP;
    case JG:            return NUM_JG;
    case JGE:           return NUM_JGE;
    case JL:            return NUM_JL;
    case JLE:           return NUM_JLE;
    case JE:            return NUM_JE;
    case JNE:           return NUM_JNE;
    case CMP:           return NUM_CMP;
    case CMP_R:         return NUM_CMP_R;
    case WRITE:         return NUM_WRITE;
    case READ:          return NUM_READ;
    case PUSH:          return NUM_PUSH;
    case PUSH_R:        return NUM_PUSH_R;
    case POP_R:         return NUM_POP_R;
    case CALL:          return NUM_CALL;
    case RET:           return NUM_RET;
    case EXIT:          return NUM_EXIT;
    case N_INST:        return N_INST;

    default:
        throw std::runtime_error("TranslateIDInst():"
                                 "Unidefined instruction " +
                                 std::to_string(keyID));
    }
}

void Translator::Impl::AddTact(const int numInst)
{
    llvm::Value *pArg_1 = builder_->CreateConstGEP2_32(benchmarkResult_.type, benchmarkResult_.array,
                                                       0, TranslateIDInst(numInst));
    llvm::Value *arg_1 = builder_->CreateLoad(pArg_1);
    llvm::Value *arg_2 = llvm::ConstantInt::get(builder_->getInt32Ty(), 1);
    builder_->CreateStore(builder_->CreateAdd(arg_1, arg_2), pArg_1);
}

void Translator::Impl::PrintBenchmarkResult()
{
    std::vector<llvm::Type *> funcArgsTypes;
    funcArgsTypes.push_back(builder_->getInt8PtrTy());
    llvm::FunctionType *funcType =
        llvm::FunctionType::get(builder_->getInt32Ty(),
                                funcArgsTypes, true);

    llvm::FunctionCallee func = module_->getOrInsertFunction("printf",
                                                             funcType);

    const std::string formatStr("\n\n[Result of Benchmark]\nVer 1.0.0\n"
                                "Number of tacts of each instruction:\n"
                                "\tPush - %d\n"
                                "\tPush_R - %d\n"
                                "\tPop_R - %d\n"
                                "\tMov - %d\n"
                                "\tMov_R - %d\n"
                                "\tMov_RP - %d\n"
                                "\tCall - %d\n"
                                "\tRet - %d\n"
                                "\tExit - %d\n"
                                "\tWrite - %d\n"
                                "\tRead - %d\n"
                                "\tAdd - %d\n"
                                "\tSub - %d\n"
                                "\tIMul - %d\n"
                                "\tIDiv - %d\n"
                                "\tAdd_R - %d\n"
                                "\tSub_R - %d\n"
                                "\tIMul_R - %d\n"
                                "\tIDiv_R - %d\n"
                                "\tInc - %d\n"
                                "\tDec - %d\n"
                                "\tCmp - %d\n"
                                "\tCmp_R - %d\n"
                                "\tJmp - %d\n"
                                "\tJG - %d\n"
                                "\tJGE - %d\n"
                                "\tJL - %d\n"
                                "\tJLE - %d\n"
                                "\tJE - %d\n"
                                "\tJNE - %d\n"
                                "\nNumber of jumps - %d\n"
                                "\n[End!]\n\n");
    llvm::Value *formatStrVal =
        builder_->CreateGlobalStringPtr(formatStr, "BenchmarkResult");

    std::vector<llvm::Value *> args;
    args.push_back(formatStrVal);
    for (size_t numInst = 0; numInst < N_INST + 1; numInst++)
    {
        llvm::Value *pArg = builder_->CreateConstGEP2_32(benchmarkResult_.type,
                                                         benchmarkResult_.array, 0,
                                                         numInst);
        llvm::Value *arg = builder_->CreateLoad(pArg);
        args.push_back(arg);
    }

    builder_->CreateCall(func, args);
}

llvm::BasicBlock* Translator::Impl::GetBB(size_t PC) const
{
    for (auto branchBB : branchBBs_) {
        if (branchBB.second.truePC == PC)
            return branchBB.second.trueBB;
        if (branchBB.second.falsePC == PC)
            return branchBB.second.falseBB;
    }

    return nullptr;
}

llvm::Function* Translator::Impl::GetFunction(size_t PC) const
{
    auto function = functions_.find(PC);
    if (function != functions_.end())
        return function->second;

    return nullptr;
}

void Translator::Translate()
{
    pImpl_->Preprocess();
    pImpl_->Translate();
}

void Translator::Dump() const
{
    std::cout << ";#[LLVM_IR]:\n";
    std::string s;
    llvm::raw_string_ostream os(s);
    pImpl_->module_->print(os, nullptr);
    os.flush();
    std::cout << s;
}