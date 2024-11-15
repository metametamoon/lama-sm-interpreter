/* Lama SM Bytecode interpreter */

#include "runtime/runtime_common.h"
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

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

#define BOXED(x) (((u32)(x)) & 0x0001)

static i32 constexpr N_GLOBAL = 1000;
static i32 constexpr STACK_SIZE = 100000;

static inline void error(char const *format, ...) {
  fprintf(stderr, "error: ");
  va_list argptr;
  va_start(argptr, format);
  vfprintf(stderr, format, argptr);
  fprintf(stderr, "\n");
  exit(-1);
  va_end(argptr);
}

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
    if ((void *)data.data() >= (void *)__gc_stack_top) {
      error("error: stack overflow");
    }
    *(__gc_stack_top--) = value;
  }

  T pop() {
    if (__gc_stack_top == stack_begin) {
      error("negative stack\n");
    }
    return *(++__gc_stack_top);
  }
  T top() { return *(__gc_stack_top + 1); }

  void print_ptrs() {
    fprintf(stderr, "rbp=%d rsp=%d\n", __gc_stack_bottom - base_pointer,
            __gc_stack_bottom - __gc_stack_top);
  }

  void print_content() {
    print_ptrs();
    // for (i32 i = stack_pointer; i >= 0; --i) {
    //   fprintf(stderr, "%.2d: ", i);
    //   if (BOXED(data[i])) {
    //     fprintf(stderr, "B(%u)\n", UNBOX(data[i]));
    //   } else {
    //     fprintf(stderr, "%x\n", data[i]);
    //   }
    // }
  }
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
  va_list args;
  int i;
  int ai;
  size_t *p;
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

/* The unpacked representation of bytecode file */
#pragma
struct __attribute__((packed)) bytefile {
  char *stringtab_ptr; /* A pointer to the beginning of the string table */
  char *last_stringtab_zero;
  int *public_ptr; /* A pointer to the beginning of publics table    */
  char *code_ptr;  /* A pointer to the bytecode itself               */
  void *code_end;
  int stringtab_size;   /* The size (in bytes) of the string table        */
  int global_area_size; /* The size (in words) of global area             */
  int public_symbols_number; /* The number of public symbols */
  char buffer[0];
};

/* Gets a string from a string table by an index */
static inline char const *get_string(bytefile const *f, int pos) {
  // validate its is an ok string
  char *ptr = &f->stringtab_ptr[pos];
  if (ptr > f->last_stringtab_zero) {
    fprintf(stderr, "Bad string read at offset %d (string did not terminate)\n",
            pos);
    exit(-1);
  }
  return ptr;
}

/* Gets a name for a public symbol */
static inline char const *get_public_name(bytefile *f, int i) {
  return get_string(f, f->public_ptr[i * 2]);
}

/* Gets an offset for a publie symbol */
static inline int get_public_offset(bytefile *f, int i) {
  if (!(i < f->public_symbols_number)) {
    fprintf(stderr, "Trying to read out of bounds public member at %d", i);
    exit(-1);
  }
  return f->public_ptr[i * 2 + 1];
}

/* Reads a binary bytecode file by name and unpacks it */
static inline bytefile const *read_file(char *fname) {
  FILE *f = fopen(fname, "rb");
  long size;
  bytefile *file;

  if (f == nullptr) {
    error("%s\n", strerror(errno));
  }

  if (fseek(f, 0, SEEK_END) == -1) {
    error("%s\n", strerror(errno));
  }
  size = ftell(f);

  file = (bytefile *)malloc(sizeof(int) * 4 + size + 100);

  if (file == nullptr) {
    error("*** FAILURE: unable to allocate memory.\n");
  }

  rewind(f);

  if (size != fread(&file->stringtab_size, 1, size, f)) {
    error("%s\n", strerror(errno));
  }

  if (file->public_symbols_number < 0) {
    error("unreasonable number of public symbols (an error?): %d\n",
          file->public_symbols_number);
  }
  if (file->stringtab_size < 0) {
    error("unreasonable size of stringtab (an error?): %d\n",
          file->public_symbols_number);
  }
  if (file->global_area_size < 0) {
    error("unreasonable size of global aread (an error?): %d\n",
          file->public_symbols_number);
  }

  fclose(f);

  file->stringtab_ptr =
      &file->buffer[file->public_symbols_number * 2 * sizeof(int)];
  file->public_ptr = (int *)file->buffer;
  file->code_ptr = &file->stringtab_ptr[file->stringtab_size];
  file->code_end = (char *)&file->stringtab_size + size;
  for (file->last_stringtab_zero = file->code_ptr - 1;
       file->last_stringtab_zero > file->stringtab_ptr;
       --file->last_stringtab_zero) {
    if (*file->last_stringtab_zero == 0)
      break;
  }
  return file;
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

static inline bool check_address(bytefile const *f, char *addr) {
  return (f->code_ptr <= addr) && (addr < f->code_end);
}

static inline void print_location(bytefile const *bf, char *next_ip) {
  fprintf(stderr, "at 0x%.8x:\n", unsigned((next_ip - 4) - bf->code_ptr - 1));
}

using u8 = std::uint8_t;
static u8 constexpr HBINOP = 0;
static u8 constexpr HMISC1 = 1;
static u8 constexpr HLD = 2;
static u8 constexpr HLDA = 3;
static u8 constexpr HST = 4;
static u8 constexpr HMISC2 = 5;
static u8 constexpr HPATT = 6;
static u8 constexpr HCALL = 7;

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

static inline void located_error(bytefile const *bf, char *next_ip,
                                 const char *format, ...) {
  fprintf(stderr, "error\n");
  print_location(bf, next_ip);
  va_list argptr;
  va_start(argptr, format);
  vfprintf(stderr, format, argptr);
  fprintf(stderr, "\n");
  exit(-1);
  va_end(argptr);
}

/* Disassembles the bytecode pool */
static inline void interpret(FILE *f, bytefile const *bf) {
#define FAIL                                                                   \
  {                                                                            \
    printf("ERROR: invalid opcode %d-%d\n", h, l);                             \
    exit(0);                                                                   \
  }

  char *ip = bf->code_ptr;
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

  static char const *const ops[] = {
      "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "&&", "!!"};
  static char const *const pats[] = {"=str", "#string", "#array", "#sexp",
                                     "#ref", "#val",    "#fun"};
  static char const *const lds[] = {"LD", "LDA", "ST"};
  auto operands_stack = stack<u32>{};
  // pseudo first two args
  operands_stack.push(BOX(0));
  operands_stack.push(BOX(0));
  u32 const GLOBAL = 1;
  u32 const LOCAL = 2;
  u32 const ARG = 3;
  u32 const CAPTURED = 4;
  auto create_reference = [&](u32 index, u32 kind, char *next_ip) -> u32 {
    switch (kind) {
    case GLOBAL: {
      if (index > N_GLOBAL) {
        located_error(bf, next_ip, "indexing global at %d > N_GLOBALS(%d)",
                      index, N_GLOBAL);
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
  };
  auto write_reference = [&](u32 reference, u32 value) -> void {
    *((u32 *)reference) = value;
  };
  __init();
  bool in_closure = false;
  auto check_is_begin = [](bytefile const *bf, char *ip) {
    if (!check_address(bf, ip)) {
      return false;
    }
    u8 x = *ip;
    u8 h = (x & 0xF0) >> 4, l = x & 0x0F;
    return h == 5 && (l == 3 || l == 2);
  };

  do {
    if (ip >= bf->code_end) {
      error("execution unexpectedly got out of code section\n");
    }
    u8 x = read_byte(), h = (x & 0xF0) >> 4, l = x & 0x0F;
    debug(stderr, "0x%.8x:\t", unsigned(ip - bf->code_ptr - 1));
    switch ((HCode)h) {
    case HCode::STOP:
      goto stop;

    case HCode::BINOP: {
      debug(stderr, "BINOP\t%s", ops[l - 1]);
      if (l - 1 < (i32)BinopLabel::BINOP_LAST) {
        u32 t2 = UNBOX(operands_stack.pop());
        u32 t1 = UNBOX(operands_stack.pop());
        u32 result = (u32)arithm_op((i32)t1, (i32)t2, (BinopLabel)(l - 1));
        operands_stack.push(BOX(result));
      } else {
        FAIL;
      }
      break;
    }

    case HCode::MISC1: {
      switch ((Misc1LCode)l) {
      case Misc1LCode::CONST: {
        auto arg = read_int();
        operands_stack.push(BOX(arg));
        debug(stderr, "CONST\t%d", arg);
        break;
      }

      case Misc1LCode::STR: {
        char const *string = read_string();
        debug(stderr, "STRING\t%s", string);
        char *obj_string = (char *)Bstring((void *)string);
        operands_stack.push(u32(obj_string));
        break;
      }

      case Misc1LCode::SEXP: {
        char const *tag = read_string();
        int n = read_int();
        debug(stderr, "SEXP\t%s ", tag);
        debug(stderr, "%d", n);
        auto value = myBsexp(n, operands_stack, tag);
        operands_stack.push(u32(value));
        break;
      }

      case Misc1LCode::STI: {
        debug(f, "STI");
        u32 value = operands_stack.pop();
        u32 reference = operands_stack.pop();
        write_reference(reference, value);
        operands_stack.push(value);
        break;
      }

      case Misc1LCode::STA: {
        debug(stderr, "STA");
        auto value = (void *)operands_stack.pop();
        auto i = (int)operands_stack.pop();
        auto x = (void *)operands_stack.pop();
        operands_stack.push((u32)Bsta(value, i, x));
        break;
      }

      case Misc1LCode::JMP: {
        auto jump_location = read_int();
        debug(stderr, "JMP\t0x%.8x", jump_location);
        char *old_ip = ip;
        ip = bf->code_ptr + jump_location;
        if (!check_address(bf, ip)) {
          print_location(bf, old_ip);
          fprintf(stderr, "trying to jump out of the code area to offset %d",
                  ip - bf->code_ptr);
        }

        break;
      }

      case Misc1LCode::END:
      case Misc1LCode::RET: {
        if (h == 7) {
          debug(stderr, "RET");
        } else {
          debug(stderr, "END");
        }
        if (operands_stack.base_pointer != operands_stack.stack_begin - 3) {
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
          ip = (char *)ret_ip;
          in_closure = false;
        } else {
          in_closure = false;
          goto stop;
        }
        break;
      }

      case Misc1LCode::DROP:
        debug(stderr, "DROP");
        operands_stack.pop();
        break;

      case Misc1LCode::DUP: {
        debug(stderr, "DUP");
        u32 v = operands_stack.top();
        operands_stack.push(v);
        break;
      }

      case Misc1LCode::SWAP: {
        debug(f, "SWAP");
        auto fst = operands_stack.pop();
        auto snd = operands_stack.pop();
        operands_stack.push(fst);
        operands_stack.push(snd);
        break;
      }

      case Misc1LCode::ELEM: {
        debug(stderr, "ELEM");
        auto index = (int)operands_stack.pop();
        auto obj = (void *)operands_stack.pop();
        u32 elem = (u32)Belem(obj, index);
        operands_stack.push(elem);
        break;
      }

      default:
        FAIL;
      }
      break;
    }
    case HCode::LD: { // LD
      debug(stderr, "%s\t", lds[h - 2]);
      i32 const index = read_int();
      u32 kind = l + 1;
      auto value = *(u32 *)create_reference(index, kind, ip);
      operands_stack.push(value);
      break;
    }
    case HCode::LDA: {
      debug(stderr, "%s\t", lds[h - 2]);
      i32 index = read_int();
      u32 kind = l + 1;
      auto ref = create_reference(index, kind, ip);
      operands_stack.push(ref);
      operands_stack.push(ref);
      break;
    }
    case HCode::ST: { // ST
      debug(stderr, "%s\t", lds[h - 2]);
      i32 index = read_int();
      u32 kind = l + 1;
      auto top = operands_stack.top();
      write_reference(create_reference(index, kind, ip), top);
      break;
    }

    case HCode::MISC2: {
      switch ((Misc2LCode)l) {
      case Misc2LCode::CJMPZ: {
        auto jump_location = read_int();
        debug(stderr, "CJMPz\t0x%.8x", jump_location);
        auto top = UNBOX(operands_stack.pop());
        if (top == 0) {
          char *old_ip = ip;
          ip = bf->code_ptr + jump_location;
          if (!check_address(bf, ip)) {
            print_location(bf, old_ip);
            fprintf(stderr, "trying to jump out of the code area to offset %d",
                    ip - bf->code_ptr);
          }
        }
        break;
      }

      case Misc2LCode::CJMPNZ: {
        auto jump_location = read_int();
        debug(stderr, "CJMPnz\t0x%.8x", jump_location);
        auto top = UNBOX(operands_stack.pop());
        if (top != 0) {
          char *old_ip = ip;
          ip = bf->code_ptr + jump_location;
          if (!check_address(bf, ip)) {
            print_location(bf, old_ip);
            fprintf(stderr, "trying to jump out of the code area to offset %d",
                    ip - bf->code_ptr);
          }
        }
        break;
      }

      case Misc2LCode::BEGIN:
      case Misc2LCode::CBEGIN: {
        int n_args = read_int();
        int n_locals = read_int();
        if (l == 3) {
          debug(stderr, "C");
        }
        debug(stderr, "BEGIN\t%d ", n_args);
        debug(stderr, "%d", n_locals);
        operands_stack.push(BOX(operands_stack.n_args));
        operands_stack.push((u32)operands_stack.base_pointer);
        operands_stack.n_args = n_args;
        operands_stack.base_pointer = __gc_stack_top + 1;
        __gc_stack_top -= (n_locals + 1);
        memset((void *)__gc_stack_top, 0, (n_locals + 1) * sizeof(size_t));
        break;
      }

      case Misc2LCode::CLOSURE: {
        int addr = read_int();
        debug(stderr, "CLOSURE\t0x%.8x", addr);
        if (addr < 0 || addr > ((char *)bf->code_end - bf->code_ptr)) {
          located_error(bf, ip, "closure points outside of the code area\n");
        }
        if (!check_is_begin(bf, bf->code_ptr + addr)) {
          located_error(bf, ip, "closure does not point at begin\n");
        }
        int n = read_int();
        for (int i = 0; i < n; i++) {
          switch (read_byte()) {
          case 0: {
            int index = read_int();
            debug(stderr, "G(%d)", index);
            operands_stack.push((u32) *
                                (u32 *)create_reference(index, GLOBAL, ip));
            break;
          }
          case 1: {
            int index = read_int();
            debug(stderr, "L(%d)", index);
            operands_stack.push((u32) *
                                (u32 *)create_reference(index, LOCAL, ip));
            break;
          }
          case 2: {
            int index = read_int();
            debug(stderr, "A(%d)", index);
            operands_stack.push(
                (u32)(*((u32 *)create_reference(index, ARG, ip))));
            break;
          }
          case 3: {
            int index = read_int();
            debug(stderr, "C(%d)", index);
            operands_stack.push(
                (u32)(*((u32 *)create_reference(index, CAPTURED, ip))));
            break;
          }
          default:
            FAIL;
          }
        }
        u32 v = (u32)myBclosure(n, operands_stack, (void *)addr);
        operands_stack.push(v);
        break;
      };

      case Misc2LCode::CALLC: {
        int n_arg = read_int();
        debug(stderr, "CALLC\t%d", n_arg);
        // if (n_arg == 0) {
        u32 closure = *(__gc_stack_top + 1 + n_arg);
        u32 addr = (u32)(((i32 *)closure)[0]);
        operands_stack.push(u32(ip));
        ip = bf->code_ptr + addr;
        in_closure = true;
        break;
      }

      case Misc2LCode::CALL: {
        int loc = read_int();
        int n = read_int();
        debug(stderr, "CALL\t0x%.8x ", loc);
        debug(stderr, "%d", n);
        operands_stack.push(u32(ip));
        if (!check_is_begin(bf, bf->code_ptr + loc)) {
          located_error(bf, ip, "CALL does not call a function\n");
          ;
        }
        ip = bf->code_ptr + loc;
        break;
      }

      case Misc2LCode::TAG: {
        const char *name = read_string();
        int n = read_int();
        debug(stderr, "TAG\t%s ", name);
        debug(stderr, "%d", n);
        u32 v =
            Btag((void *)operands_stack.pop(), LtagHash((char *)name), BOX(n));
        operands_stack.push(v);
        break;
      }

      case Misc2LCode::ARRAY: {
        int size = read_int();
        debug(stderr, "ARRAY\t%d", size);
        size_t is_array_n =
            (size_t)Barray_patt((void *)operands_stack.pop(), BOX(size));
        operands_stack.push(is_array_n);
        break;
      }

      case Misc2LCode::FAILURE:
        fprintf(f, "FAIL\t%d", read_int());
        fprintf(f, "%d", read_int());
        exit(-1);
        break;

      case Misc2LCode::LINE: {
        int line = read_int();
        debug(stderr, "LINE\t%d", line);
        break;
      }

      default:
        FAIL;
      }
      break;
    }
    case HCode::PATT: {
      debug(stderr, "PATT\t%s", pats[l]);
      if (l == 0) { // =str
        auto arg = (void *)operands_stack.pop();
        auto eq = (void *)operands_stack.pop();
        operands_stack.push((u32)Bstring_patt(arg, eq));
      } else if (l < (i32)Patt::LAST) {
        auto arg = operands_stack.pop();
        operands_stack.push(patts_match((void *)arg, (Patt)l));
      } else {
        fprintf(stderr, "Unsupported patt specializer: %d", l);
        FAIL;
      }
      break;
    }
    case HCode::CALL: {
      switch ((Call)l) {
      case Call::READ: {
        debug(stderr, "CALL\tLread");
        operands_stack.push(Lread());
        break;
      }

      case Call::WRITE: {
        u32 value = UNBOX(operands_stack.pop());
        debug(stderr, "CALL\tLwrite");
        fprintf(stdout, "%d\n", i32(value));
        operands_stack.push(BOX(0));
        break;
      }
      case Call::LLENGTH: {
        debug(stderr, "CALL\tLlength");
        int value = (int)operands_stack.pop();
        char *str = (char *)value;
        int result = Llength((void *)value);
        operands_stack.push((u32)result);
        break;
      }

      case Call::LSTRING: {
        debug(stderr, "CALL\tLstring");
        operands_stack.push((u32)Lstring(((void *)operands_stack.pop())));
        break;
      }

      case Call::BARRAY: {
        i32 n = read_int();
        debug(stderr, "CALL\tBarray\t%d", n);
        auto arr = myBarray(n, operands_stack);
        operands_stack.push(u32(arr));
        break;
      }

      default:
        FAIL;
      }
    } break;

    default:
      FAIL;
    }

    debug(stderr, "\n");
  } while (1);
stop:
  debug(stderr, "<end>\n");
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
  interpret(f, bf);
}

int main(int argc, char *argv[]) {
  bytefile const *f = read_file(argv[1]);
  dump_file(stdout, f);
  return 0;
}
