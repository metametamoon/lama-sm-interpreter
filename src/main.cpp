/* Lama SM Bytecode interpreter */

#include "bytefile.h"
#include "runtime/runtime_common.h"
#include "visitor.h"
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <optional>
#include <stdio.h>
#include <string.h>
#include <string>

extern "C" void *Belem(void *p, int i);
extern "C" void *Bsta(void *v, int i, void *x);
extern "C" void *Bstring(void *p);
extern "C" int Llength(void *p);
extern "C" int Lread();
extern "C" void *alloc_array(int);
extern "C" int Btag(void *d, int t, int n);
extern "C" void *Lstring(void *p);
extern "C" int Bstring_patt(void *x, void *y);
extern "C" int Bclosure_tag_patt(void *x);
extern "C" int Bboxed_patt(void *x);
extern "C" int Bunboxed_patt(void *x);
extern "C" int Barray_tag_patt(void *x);
extern "C" int Bstring_tag_patt(void *x);
extern "C" int Bsexp_tag_patt(void *x);

extern "C" int Barray_patt(void *d, int n);

extern "C" size_t *__gc_stack_top, *__gc_stack_bottom;
extern "C" void __init();

using u32 = uint32_t;
using i32 = int32_t;
using u8 = uint8_t;

#define BOXED(x) (((u32)(x)) & 0x0001)

static i32 constexpr N_GLOBAL = 1000;
static i32 constexpr STACK_SIZE = 100000;

// stored on the stack (see std::array)
template <typename T> struct stack {
  std::array<T, STACK_SIZE> data; // zero-initialized on the stack
  size_t *stack_begin = nullptr;
  // size_t* stack_pointer = nullptr; replaced by __gc_stack_top
  size_t *base_pointer = nullptr;
  u32 n_args = 2; // default

  stack() {
    __gc_stack_bottom = (size_t *)(data.data() + STACK_SIZE);
    stack_begin = __gc_stack_bottom - N_GLOBAL;
    base_pointer = stack_begin;
    __gc_stack_top = stack_begin;
  }

  void push(T value) {
    // if ((void *)data.data() >= (void *)__gc_stack_top) {
    //   error("error: stack overflow");
    // }
    *(__gc_stack_top--) = value;
  }

  T pop() {
    // if (__gc_stack_top == stack_begin) {
    //   error("negative stack\n");
    // }
    return *(++__gc_stack_top);
  }
  T top() { return *(__gc_stack_top + 1); }

  void print_ptrs() {
    fprintf(stderr, "rbp=%d rsp=%d\n", __gc_stack_bottom - base_pointer,
            __gc_stack_bottom - __gc_stack_top);
  }

  void print_content() { print_ptrs(); }
};

static inline void *myBarray(int n, stack<u32> &ops_stack) {
  data *r = (data *)alloc_array(n);
  for (i32 i = n - 1; i >= 0; --i) {
    i32 elem = ops_stack.pop();
    ((int *)r->contents)[i] = elem;
  }
  return r->contents;
}

extern "C" void *alloc_sexp(int members);
extern "C" int LtagHash(char *);
static inline void *myBsexp(int n, stack<u32> &ops_stack, char const *name) {
  int i;
  int ai;
  data *r;
  int fields_cnt = n;
  r = (data *)alloc_sexp(fields_cnt);
  ((sexp *)r)->tag = 0;

  for (i = n; i > 0; --i) {
    ai = ops_stack.pop();
    ((int *)r->contents)[i] = ai;
  }

  ((sexp *)r)->tag =
      UNBOX(LtagHash((char *)name)); // cast for runtime compatibility

  return (int *)r->contents;
}

extern "C" void *alloc_closure(int);
static inline void *myBclosure(int n, stack<u32> &ops_stack, void *addr) {
  int i, ai;
  data *r;
  r = (data *)alloc_closure(n + 1);
  ((void **)r->contents)[0] = addr;

  for (i = n; i >= 1; --i) {
    ai = ops_stack.pop();
    ((int *)r->contents)[i] = ai;
  }

  return r->contents;
}

void *__start_custom_data;
void *__stop_custom_data;

// #define DEBUG

#ifdef DEBUG
#define debug(...) fprintf(__VA_ARGS__)
#else
#define debug(...)
#endif

/* Gets a string from a string table by an index */
static inline char const *get_string(bytefile const *f, int pos) {
  // validate its is an ok string
  char *ptr = (char *)&f->stringtab_ptr[pos];
  if (ptr > (char *)f->last_stringtab_zero) {
    fprintf(stderr, "Bad string read at offset %d (string did not terminate)\n",
            pos);
    exit(-1);
  }
  return ptr;
}

enum class BinopLabel {
  ADD = 0,
  SUB = 1,
  MUL = 2,
  DIV = 3,
  MOD = 4,
  LT = 5,
  LEQ = 6,
  GT = 7,
  GEQ = 8,
  EQ = 9,
  NEQ = 10,
  AND = 11,
  OR = 12,
  BINOP_LAST
};

// doing my best to not clah with macros
enum class Patt {
  STR_EQ_TAG = 0,
  STR_TAG = 1,
  ARR_TAG = 2,
  SEXPR_TAG = 3,
  BOXED = 4,
  UNBOXED = 5,
  CLOS_TAG = 6,
  LAST
};

static inline u32 patts_match(void *arg, Patt label) {
#define PATT_CASE(enum_kind, function)                                         \
  case enum_kind:                                                              \
    return function(arg);
  switch (label) {
    PATT_CASE(Patt::STR_TAG, Bstring_tag_patt)
    PATT_CASE(Patt::ARR_TAG, Barray_tag_patt)
    PATT_CASE(Patt::SEXPR_TAG, Bsexp_tag_patt)
    PATT_CASE(Patt::BOXED, Bboxed_patt)
    PATT_CASE(Patt::UNBOXED, Bunboxed_patt)
    PATT_CASE(Patt::CLOS_TAG, Bclosure_tag_patt)
  default:
    error("bad patt specializer: %d\n", (i32)label);
    return 0;
  }
}

static inline i32 arithm_op(i32 l, i32 r, BinopLabel label) {
#define BINOP_CASE(enum_kind, binop)                                           \
  case enum_kind:                                                              \
    return l binop r;
  switch (label) {
    BINOP_CASE(BinopLabel::ADD, +)
    BINOP_CASE(BinopLabel::SUB, -)
    BINOP_CASE(BinopLabel::MUL, *)
    BINOP_CASE(BinopLabel::DIV, /)
    BINOP_CASE(BinopLabel::MOD, %)
    BINOP_CASE(BinopLabel::LT, <)
    BINOP_CASE(BinopLabel::LEQ, <=)
    BINOP_CASE(BinopLabel::GT, >)
    BINOP_CASE(BinopLabel::GEQ, >=)
    BINOP_CASE(BinopLabel::EQ, ==)
    BINOP_CASE(BinopLabel::NEQ, !=)
    BINOP_CASE(BinopLabel::AND, &&)
    BINOP_CASE(BinopLabel::OR, ||)
  default:
    error("unsupported op label: %d", (i32)label);
    return -1;
  }
}

static inline bool check_address(bytefile const *f, u8 *addr) {
  return (f->code_ptr <= addr) && (addr < f->code_end);
}

using u8 = std::uint8_t;
enum class HCode : u8 {
  BINOP = 0,
  MISC1 = 1,
  LD = 2,
  LDA = 3,
  ST = 4,
  MISC2 = 5,
  PATT = 6,
  CALL = 7,
  STOP = 15,
};

enum class Misc1LCode : u8 {
  CONST = 0,
  STR = 1,
  SEXP = 2,
  STI = 3,
  STA = 4,
  JMP = 5,
  END = 6,
  RET = 7,
  DROP = 8,
  DUP = 9,
  SWAP = 10,
  ELEM = 11,
};

enum class Misc2LCode : u8 {
  CJMPZ = 0,
  CJMPNZ = 1,
  BEGIN = 2,
  CBEGIN = 3,
  CLOSURE = 4,
  CALLC = 5,
  CALL = 6,
  TAG = 7,
  ARRAY = 8,
  FAILURE = 9,
  LINE = 10,
  ELEM = 11,
};

enum class Call : u8 {
  READ = 0,
  WRITE = 1,
  LLENGTH = 2,
  LSTRING = 3,
  BARRAY = 4,
};

template <typename T> struct VisitResult {
  u8 *next_ip;
  T value;
};

template <typename T>
static inline VisitResult<T> visit_instruction(bytefile const *bf, u8 *ip,
                                               Visitor<T> &visitor) {
#define FAIL                                                                   \
  {                                                                            \
    printf("ERROR: invalid opcode %d-%d\n", h, l);                             \
    exit(0);                                                                   \
  }
#define RET(x) return VisitResult<T>{ip, x};
  auto read_int = [&ip, &bf]() {
    check_address(bf, ip);
    ip += sizeof(int);
    return *(int *)(ip - sizeof(int));
  };

  auto read_byte = [&ip, &bf]() {
    check_address(bf, ip);
    return *ip++;
  };

  auto read_string = [&read_int, &ip, &bf]() {
    check_address(bf, ip);
    return get_string(bf, read_int());
  };

  if (ip >= bf->code_end) {
    error("execution unexpectedly got out of code section\n");
  }
  u8 x = read_byte(), h = (x & 0xF0) >> 4, l = x & 0x0F;
  debug(stderr, "0x%.8x:\t", unsigned(ip - bf->code_ptr - 1));
  switch ((HCode)h) {
  case HCode::STOP: {
    RET(visitor.visit_stop(ip));
  }

  case HCode::BINOP: {
    if (l - 1 >= (i32)BinopLabel::BINOP_LAST) {
      FAIL;
    }
    RET(visitor.visit_binop(ip, l - 1));
    break;
  }

  case HCode::MISC1: {
    switch ((Misc1LCode)l) {
    case Misc1LCode::CONST: {
      auto arg = read_int();
      RET(visitor.visit_const(ip, arg));
      break;
    }

    case Misc1LCode::STR: {
      char const *string = read_string();
      RET(visitor.visit_str(ip, string));
      break;
    }

    case Misc1LCode::SEXP: {
      char const *tag = read_string();
      int n = read_int();
      RET(visitor.visit_sexp(ip, tag, n));
      break;
    }

    case Misc1LCode::STI: {
      RET(visitor.visit_sti(ip));
      break;
    }

    case Misc1LCode::STA: {
      RET(visitor.visit_sta(ip));
      break;
    }

    case Misc1LCode::JMP: {
      auto jump_location = read_int();
      RET(visitor.visit_jmp(ip, jump_location));
      break;
    }

    case Misc1LCode::END:
    case Misc1LCode::RET: {
      if (h == 7) {
      } else {
      }
      RET(visitor.visit_end_ret(ip));
      break;
    }

    case Misc1LCode::DROP:
      RET(visitor.visit_drop(ip));
      break;

    case Misc1LCode::DUP: {
      RET(visitor.visit_dup(ip));
      break;
    }

    case Misc1LCode::SWAP: {
      RET(visitor.visit_swap(ip));
      break;
    }

    case Misc1LCode::ELEM: {
      RET(visitor.visit_elem(ip));
      break;
    }

    default:
      FAIL;
    }
    break;
  }
  case HCode::LD: { // LD
    i32 const index = read_int();
    u32 kind = l + 1;
    RET(visitor.visit_ld(ip, kind, index));
    break;
  }
  case HCode::LDA: {
    i32 index = read_int();
    u32 kind = l + 1;
    RET(visitor.visit_lda(ip, kind, index));
    break;
  }
  case HCode::ST: { // ST
    i32 index = read_int();
    u32 kind = l + 1;
    RET(visitor.visit_st(ip, kind, index));
    break;
  }

  case HCode::MISC2: {
    switch ((Misc2LCode)l) {
    case Misc2LCode::CJMPZ: {
      auto jump_location = read_int();
      RET(visitor.visit_cjmp(ip, false, jump_location));
      break;
    }

    case Misc2LCode::CJMPNZ: {
      auto jump_location = read_int();
      RET(visitor.visit_cjmp(ip, true, jump_location));
      break;
    }

    case Misc2LCode::BEGIN:
    case Misc2LCode::CBEGIN: {
      int n_args = read_int();
      int n_locals = read_int();
      if (l == 3) {
      }
      RET(visitor.visit_begin(ip, l == 3, n_args, n_locals));
      break;
    }

    case Misc2LCode::CLOSURE: {
      int addr = read_int();
      int n = read_int();
      u8 *arg_begin = ip;
      ip += (sizeof(u8) + sizeof(i32)) * n;
      RET(visitor.visit_closure(ip, addr, n, arg_begin));
      break;
    };

    case Misc2LCode::CALLC: {
      int n_arg = read_int();
      RET(visitor.visit_call_closure(ip, n_arg));
      break;
    }

    case Misc2LCode::CALL: {
      int loc = read_int();
      int n_arg = read_int();
      RET(visitor.visit_call(ip, loc, n_arg));
      break;
    }

    case Misc2LCode::TAG: {
      const char *name = read_string();
      int n = read_int();
      RET(visitor.visit_tag(ip, name, n));
      break;
    }

    case Misc2LCode::ARRAY: {
      int size = read_int();
      RET(visitor.visit_array(ip, size));
      break;
    }

    case Misc2LCode::FAILURE: {
      i32 arg1 = read_int();
      i32 arg2 = read_int();
      RET(visitor.visit_fail(ip, arg1, arg2));
      break;
    }

    case Misc2LCode::LINE: {
      int line = read_int();
      RET(visitor.visit_line(ip, line));
      break;
    }
    default:
      FAIL;
    }
    break;
  }
  case HCode::PATT: {
    RET(visitor.visit_patt(ip, l));
    break;
  }
  case HCode::CALL: {
    switch ((Call)l) {
    case Call::READ: {
      RET(visitor.visit_call_lread(ip));
      break;
    }

    case Call::WRITE: {
      RET(visitor.visit_call_lwrite(ip));
      break;
    }
    case Call::LLENGTH: {
      RET(visitor.visit_call_llength(ip));
      break;
    }

    case Call::LSTRING: {
      RET(visitor.visit_call_lstring(ip));
      break;
    }

    case Call::BARRAY: {
      i32 n = read_int();
      RET(visitor.visit_call_barray(ip, n));
      break;
    }
    default:
      FAIL;
    }
  } break;

  default:
    FAIL;
  }
}

struct ExecResult {
  u8 *exec_next_ip;
  std::optional<std::string> error = std::nullopt;
};

u32 const GLOBAL = 1;
u32 const LOCAL = 2;
u32 const ARG = 3;
u32 const CAPTURED = 4;

bool static inline check_is_begin(bytefile const *bf, u8 *ip) {
  if (!check_address(bf, ip)) {
    return false;
  }
  u8 x = *ip;
  u8 h = (x & 0xF0) >> 4, l = x & 0x0F;
  return h == 5 && (l == 3 || l == 2);
}

static char const *const ops[] = {
    "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "&&", "!!"};

static char const *const pats[] = {"=str", "#string", "#array", "#sexp",
                                   "#ref", "#val",    "#fun"};

class ExecutingVisitor final : public Visitor<ExecResult> {
public:
  ExecutingVisitor(bytefile const *bf) : bf(bf) { __init(); }
  bytefile const *bf;
  bool in_closure = false;
  stack<u32> operands_stack = stack<u32>{};
  u32 create_reference(u32 index, u32 kind) {
    switch (kind) {
    case GLOBAL: {
      if (index > N_GLOBAL) {
        fprintf(stderr, "querying out of bounds global");
        exit(-1);
      }
      return (u32)(operands_stack.stack_begin + 1 + index);
    }
    case LOCAL: {
      return (u32)(operands_stack.base_pointer - 1 - index);
    }
    case ARG: {
      auto result =
          operands_stack.base_pointer + 2 + operands_stack.n_args - index;
      return (u32)result;
    }
    case CAPTURED: {
      auto closure_ptr =
          operands_stack.base_pointer + 2 + operands_stack.n_args + 1;
      u32 *closure = (u32 *)*closure_ptr;
      return (u32)&closure[1 + index];
    }
    default: {
      error("unsupported reference kind");
      return 0;
    }
    }
  }

  void write_reference(u32 reference, u32 value) {
    *((u32 *)reference) = value;
  };

  inline ExecResult visit_binop(u8 *next_ip, u8 index) override {
    debug(stderr, "BINOP\t%s\n", ops[index]);
    u32 t2 = UNBOX(operands_stack.pop());
    u32 t1 = UNBOX(operands_stack.pop());
    u32 result = (u32)arithm_op((i32)t1, (i32)t2, (BinopLabel)index);
    operands_stack.push(BOX(result));
    return ExecResult{next_ip};
  };

  inline ExecResult visit_const(u8 *decode_next_ip, i32 arg) override {
    debug(stderr, "CONST\t%d\n", arg);
    operands_stack.push(BOX(arg));
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_str(u8 *decode_next_ip,
                              char const *literal) override {
    debug(stderr, "STRING\t%s", literal);
    char *obj_string = (char *)Bstring((void *)literal);
    operands_stack.push(u32(obj_string));
    return ExecResult{decode_next_ip};
  };

  inline ExecResult visit_sexp(u8 *decode_next_ip, char const *tag,
                               i32 args) override {
    debug(stderr, "SEXP\t%s %d\n", tag, args);
    auto value = myBsexp(args, operands_stack, tag);
    operands_stack.push(u32(value));
    return ExecResult{decode_next_ip};
  };

  inline ExecResult visit_sti(u8 *decode_next_ip) override {
    debug(stderr, "STI");
    u32 value = operands_stack.pop();
    u32 reference = operands_stack.pop();
    write_reference(reference, value);
    operands_stack.push(value);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_sta(u8 *decode_next_ip) override {
    debug(stderr, "STA");
    auto value = (void *)operands_stack.pop();
    auto i = (int)operands_stack.pop();
    auto x = (void *)operands_stack.pop();
    operands_stack.push((u32)Bsta(value, i, x));
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_jmp(u8 *decode_next_ip, i32 jump_location) override {
    debug(stderr, "JMP\t0x%.8x\n", jump_location);
    u8 *exec_next_ip = bf->code_ptr + jump_location;
    if (!check_address(bf, exec_next_ip)) {
      return ExecResult{nullptr, "trying to jump out of the code area"};
    }
    return ExecResult{exec_next_ip};
  };
  inline ExecResult visit_end_ret(u8 *decode_next_ip) override {
    debug(stderr, "END\n");

    if (operands_stack.base_pointer != operands_stack.stack_begin - 1) {
      u32 ret_value = operands_stack.pop(); // preserve the boxing kind
      u32 top_n_args = operands_stack.n_args;
      __gc_stack_top = operands_stack.base_pointer - 1;
      operands_stack.base_pointer = (size_t *)operands_stack.pop();
      operands_stack.n_args = UNBOX(operands_stack.pop());
      u32 ret_ip = operands_stack.pop();
      __gc_stack_top += top_n_args;
      if (in_closure) {
        operands_stack.pop();
      }
      operands_stack.push(ret_value);
      in_closure = false;
      return ExecResult{(u8 *)ret_ip};
    } else {
      in_closure = false;
      return ExecResult{nullptr};
    }
    return ExecResult{nullptr};
  };
  inline ExecResult visit_drop(u8 *decode_next_ip) override {
    debug(stderr, "DROP\n");
    operands_stack.pop();
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_dup(u8 *decode_next_ip) override {
    debug(stderr, "DUP\n");
    u32 v = operands_stack.top();
    operands_stack.push(v);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_swap(u8 *decode_next_ip) override {
    debug(stderr, "SWAP\n");
    auto fst = operands_stack.pop();
    auto snd = operands_stack.pop();
    operands_stack.push(fst);
    operands_stack.push(snd);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_elem(u8 *decode_next_ip) override {
    debug(stderr, "ELEM");
    auto index = (int)operands_stack.pop();
    auto obj = (void *)operands_stack.pop();
    u32 elem = (u32)Belem(obj, index);
    operands_stack.push(elem);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_ld(u8 *decode_next_ip, u8 arg_kind,
                             i32 index) override {
    debug(stderr, "LD\t\n");
    auto value = *(u32 *)create_reference(index, arg_kind);
    operands_stack.push(value);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_lda(u8 *decode_next_ip, u8 arg_kind,
                              i32 index) override {
    debug(stderr, "LDA\t");
    auto ref = create_reference(index, arg_kind);
    operands_stack.push(ref);
    operands_stack.push(ref);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_st(u8 *decode_next_ip, u8 arg_kind,
                             i32 index) override {
    debug(stderr, "ST\t\n");
    auto top = operands_stack.top();
    write_reference(create_reference(index, arg_kind), top);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_cjmp(u8 *decode_next_ip, u8 is_negated,
                               i32 jump_location) override {
    if (is_negated) {
      debug(stderr, "CJMPnz\t0x%.8x\n", jump_location);
    } else {
      debug(stderr, "CJMPz\t0x%.8x\n", jump_location);
    }
    auto top = UNBOX(operands_stack.pop());
    if (((top == 0) && !is_negated) || ((top != 0) && is_negated)) {
      u8 *ip = bf->code_ptr + jump_location;
      if (!check_address(bf, ip)) {
        return ExecResult{nullptr, "trying to jump out of the code area"};
      }
      return ExecResult{ip};
    }
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_begin(u8 *decode_next_ip, u8 is_closure_begin,
                                i32 n_args, i32 n_locals) override {
    if (is_closure_begin) {
      debug(stderr, "C");
    }
    debug(stderr, "BEGIN\t%d ", n_args);
    debug(stderr, "%d\n", n_locals);
    operands_stack.push(BOX(operands_stack.n_args));
    operands_stack.push((u32)operands_stack.base_pointer);
    operands_stack.n_args = n_args;
    operands_stack.base_pointer = __gc_stack_top + 1;
    __gc_stack_top -= (n_locals + 1);
    memset((void *)__gc_stack_top, 0, (n_locals + 1) * sizeof(size_t));
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_closure(u8 *decode_next_ip, i32 addr, i32 n,
                                  u8 *args_begin) override {
    auto read_int = [&args_begin]() {
      args_begin += sizeof(int);
      return *(int *)(args_begin - sizeof(int));
    };

    auto read_byte = [&args_begin]() { return *args_begin++; };

    debug(stderr, "CLOSURE\t0x%.8x", addr);
    if (addr < 0 || addr > (bf->code_end - bf->code_ptr)) {
      return ExecResult{nullptr, "closure points outside of the code "
                                 "area"};
    }
    if (!check_is_begin(bf, bf->code_ptr + addr)) {
      return ExecResult{nullptr, "closure does not point at begin\n"};
    }
    for (int i = 0; i < n; i++) {
      switch (read_byte()) {
      case 0: {
        int index = read_int();
        debug(stderr, "G(%d)", index);
        operands_stack.push((u32) * (u32 *)create_reference(index, GLOBAL));
        break;
      }
      case 1: {
        int index = read_int();
        debug(stderr, "L(%d)", index);
        operands_stack.push((u32) * (u32 *)create_reference(index, LOCAL));
        break;
      }
      case 2: {
        int index = read_int();
        debug(stderr, "A(%d)", index);
        operands_stack.push((u32)(*((u32 *)create_reference(index, ARG))));
        break;
      }
      case 3: {
        int index = read_int();
        debug(stderr, "C(%d)", index);
        operands_stack.push((u32)(*((u32 *)create_reference(index, CAPTURED))));
        break;
      }
      default: {
        return ExecResult{nullptr, "unsupported argument kind in closure"};
      }
      }
    }
    u32 v = (u32)myBclosure(n, operands_stack, (void *)addr);
    operands_stack.push(v);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_call_closure(u8 *decode_next_ip, i32 n_arg) override {
    debug(stderr, "CALLC\t%d", n_arg);
    u32 closure = *(__gc_stack_top + 1 + n_arg);
    u32 addr = (u32)(((i32 *)closure)[0]);
    operands_stack.push(u32(decode_next_ip));
    u8 *exec_next_ip = bf->code_ptr + addr;
    in_closure = true;
    return ExecResult{exec_next_ip};
  };
  inline ExecResult visit_call(u8 *decode_next_ip, i32 loc,
                               i32 n_arg) override {
    if (!check_is_begin(bf, bf->code_ptr + loc)) {
      return ExecResult{nullptr, "CALL does not call a function\n"};
    }
    operands_stack.push(u32(decode_next_ip));
    u8 *ip = bf->code_ptr + loc;
    return ExecResult{ip};
  };
  inline ExecResult visit_tag(u8 *decode_next_ip, char const *name,
                              i32 n_arg) override {
    debug(stderr, "TAG\t%s %d\n", name, n_arg);
    u32 v =
        Btag((void *)operands_stack.pop(), LtagHash((char *)name), BOX(n_arg));
    operands_stack.push(v);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_array(u8 *decode_next_ip, i32 size) override {
    debug(stderr, "ARRAY\t%d\n", size);
    size_t is_array_n =
        (size_t)Barray_patt((void *)operands_stack.pop(), BOX(size));
    operands_stack.push(is_array_n);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_fail(u8 *decode_next_ip, i32 arg1,
                               i32 arg2) override {
    return ExecResult{nullptr};
  };
  inline ExecResult visit_line(u8 *decode_next_ip, i32 line_number) override {
    debug(stderr, "LINE\t%d\n", line_number);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_patt(u8 *decode_next_ip, u8 patt_kind) override {
    debug(stderr, "PATT\t%s\n", pats[patt_kind]);
    if (patt_kind == 0) { // =str
      auto arg = (void *)operands_stack.pop();
      auto eq = (void *)operands_stack.pop();
      operands_stack.push((u32)Bstring_patt(arg, eq));
    } else if (patt_kind < (i32)Patt::LAST) {
      auto arg = operands_stack.pop();
      operands_stack.push(patts_match((void *)arg, (Patt)patt_kind));
    } else {
      return ExecResult{nullptr, "Unsupported patt specializer"};
      exit(-1);
    }
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_call_lread(u8 *decode_next_ip) override {
    debug(stderr, "CALL\tLread\n");
    operands_stack.push(Lread());
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_call_lwrite(u8 *decode_next_ip) override {
    u32 value = UNBOX(operands_stack.pop());
    debug(stderr, "CALL\tLwrite\n");
    fprintf(stdout, "%d\n", i32(value));
    operands_stack.push(BOX(0));
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_call_llength(u8 *decode_next_ip) override {
    debug(stderr, "CALL\tLlength\n");
    int value = (int)operands_stack.pop();
    int result = Llength((void *)value);
    operands_stack.push((u32)result);
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_call_lstring(u8 *decode_next_ip) override {
    debug(stderr, "CALL\tLstring\n");
    operands_stack.push((u32)Lstring(((void *)operands_stack.pop())));
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_call_barray(u8 *decode_next_ip, i32 n) override {
    debug(stderr, "CALL\tBarray\t%d\n", n);
    auto arr = myBarray(n, operands_stack);
    operands_stack.push(u32(arr));
    return ExecResult{decode_next_ip};
  };
  inline ExecResult visit_stop(u8 *decode_next_ip) override {
    return ExecResult{nullptr};
  }
  ~ExecutingVisitor() override = default;
};

static inline void myInterpret(bytefile const *bf) {
  fprintf(stderr, "my interpret here:\n");
  auto interpeter = ExecutingVisitor{bf};
  auto ip = bf->code_ptr;
  while (true) {
    auto result = visit_instruction(bf, ip, interpeter).value;
    if (result.exec_next_ip == nullptr) {
      if (result.error.has_value()) {
        fprintf(stderr, "err at %0x: %s\n", ip - bf->code_ptr,
                result.error.value().c_str());
      }
      break;
    } else {
      ip = result.exec_next_ip;
    }
  }
}

/* Dumps the contents of the file */
static inline void dump_file(FILE *f, bytefile const *bf) {
  int i;

  debug(stderr, "String table size       : %d\n", bf->stringtab_size);
  debug(stderr, "Global area size        : %d\n", bf->global_area_size);
  debug(stderr, "Number of public symbols: %d\n", bf->public_symbols_number);
  debug(stderr, "Public symbols          :\n");

  for (i = 0; i < bf->public_symbols_number; i++)
    debug(stderr, "   0x%.8x: %s\n", get_public_offset(bf, i),
          get_public_name(bf, i));

  debug(stderr, "Code:\n");
  // interpret(f, bf);
  myInterpret(bf);
}

int main(int argc, char *argv[]) {
  if (argc == 0) {
    return -1;
  }
  bytefile const *f = read_file(argv[1]);
  dump_file(stdout, f);
  return 0;
}
