/* Lama SM Bytecode interpreter */

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
// #include "runtime/gc.h"
#include "runtime/runtime_common.h"
extern "C" void *Belem (void *p, int i);
extern "C" void *Bsta (void *v, int i, void *x); 
extern "C" void *Bstring (void *p);
extern "C" int Llength (void *p);
extern "C" void* alloc_array(int);


extern "C" size_t *__gc_stack_top, *__gc_stack_bottom;
extern "C" void __init();

using u32 = uint32_t;
using i32 = int32_t;


// #define UNBOXED(x) (((u32)(x)) & 0x0001)
#define BOXED(x) (((u32)(x)) & 0x0001)
// #define UNBOX(x) (((u32)(x)) >> 1)
// #define BOX(x) ((((u32)(x)) << 1) | 0x0001)



template <typename T>
struct stack {
  std::array<T, 100000> data;
  u32 stack_begin = 100;
  u32 stack_pointer = stack_begin;
  u32 base_pointer = stack_begin;
  u32 n_args = 2; // default

  stack() {
    __gc_stack_bottom = (size_t*)(data.data() + stack_pointer);
    __gc_stack_top = (size_t*)(data.data());
  }

  void push(T value) {
    __gc_stack_bottom++;
    data[stack_pointer++] = value;
    assert((void*)__gc_stack_bottom == (void*)(data.data() + stack_pointer));
  }

  T pop() {
    if (stack_pointer == 0) {
      fprintf(stderr, "negative stack\n");
      exit(-1);
    }
    __gc_stack_bottom--;
    return data[--stack_pointer];
  }
  T top() {
    return data[stack_pointer - 1];
  }

  void print_ptrs() {
    fprintf(stderr, "rbp=%d rsp=%d\n", base_pointer, stack_pointer);
  }

  void print_content() {
    print_ptrs();
    for (i32 i = stack_pointer; i >= 0; --i ) {
      fprintf(stderr, "%.2d: ", i);
      if (BOXED(data[i])) {
        fprintf(stderr, "B(%u)\n", UNBOX(data[i]));
      } else {
        fprintf(stderr, "%x\n", data[i]);
      }
    }
  }
};

void *myBarray (int n, stack<u32>& ops_stack) {
  data* r = (data *)alloc_array(n);
  for (i32 i = n - 1; i >= 0; --i) {
    i32 elem = ops_stack.pop();
    ((int *)r->contents)[i] = elem;
  }
  return r->contents;
}

extern "C" void *alloc_sexp (int members);
extern "C" int   LtagHash (char *);
void *myBsexp (int n, stack<u32>& ops_stack, char* name) {
  va_list args;
  int     i;
  int     ai;
  size_t *p;
  data   *r;
  int fields_cnt   = n;
  r                = (data *)alloc_sexp(fields_cnt);
  ((sexp *)r)->tag = 0;


  for (i = n; i > 0; --i) {
    ai = ops_stack.pop();
    ((int *)r->contents)[i] = ai;
  }

  ((sexp *)r)->tag = UNBOX(LtagHash(name));

  return (int *)r->contents;
}


void *__start_custom_data;
void *__stop_custom_data;

void unsupported() {
  fprintf(stderr, "unsupported");
  exit(-1);
}

/* The unpacked representation of bytecode file */
#pragma
struct __attribute__((packed)) bytefile {
  char *string_ptr;     /* A pointer to the beginning of the string table */
  int *public_ptr;      /* A pointer to the beginning of publics table    */
  char *code_ptr;       /* A pointer to the bytecode itself               */
  int *global_ptr;      /* A pointer to the global area                   */
  int stringtab_size;   /* The size (in bytes) of the string table        */
  int global_area_size; /* The size (in words) of global area             */
  int public_symbols_number; /* The number of public symbols */
  char buffer[100];
};

/* Gets a string from a string table by an index */
char *get_string(bytefile *f, int pos) { return &f->string_ptr[pos]; }

/* Gets a name for a public symbol */
char *get_public_name(bytefile *f, int i) {
  return get_string(f, f->public_ptr[i * 2]);
}

/* Gets an offset for a publie symbol */
int get_public_offset(bytefile *f, int i) { return f->public_ptr[i * 2 + 1]; }

/* Reads a binary bytecode file by name and unpacks it */
bytefile *read_file(char *fname) {
  FILE *f = fopen(fname, "rb");
  long size;
  bytefile *file;

  if (f == nullptr) {
    printf("%s\n", strerror(errno));
    exit(-1);
  }

  if (fseek(f, 0, SEEK_END) == -1) {
    printf("%s\n", strerror(errno));
    exit(-1);
  }
  size = ftell(f);

  file = (bytefile *)malloc(sizeof(int) * 4 + size + 100);

  if (file == nullptr) {
    printf("*** FAILURE: unable to allocate memory.\n");
    exit(-1);
  }

  rewind(f);

  if (size != fread(&file->stringtab_size, 1, size, f)) {
    printf("%s\n", strerror(errno));
    exit(-1);
  }

  fclose(f);

  file->string_ptr =
      &file->buffer[file->public_symbols_number * 2 * sizeof(int)];
  file->public_ptr = (int *)file->buffer;
  file->code_ptr = &file->string_ptr[file->stringtab_size];
  file->global_ptr = (int *)malloc(file->global_area_size * sizeof(int));

  return file;
}


/* Disassembles the bytecode pool */
void interpret(FILE *f, bytefile *bf) {
#define INT (ip += sizeof(int), *(int *)(ip - sizeof(int)))
#define BYTE *ip++
#define STRING get_string(bf, INT)
#define FAIL                                                                   \
  {                                                                            \
    printf("ERROR: invalid opcode %d-%d\n", h, l);                             \
    exit(0);                                                                   \
  }

  char *ip = bf->code_ptr;
  char const *ops[] = {
      "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "&&", "!!"};
  auto ops_array = std::array{
      +[](u32 l, u32 r) -> u32 { return l + r; },
      +[](u32 l, u32 r) -> u32 { return l - r; },
      +[](u32 l, u32 r) -> u32 { return l * r; },
      +[](u32 l, u32 r) -> u32 { return l / r; },
      +[](u32 l, u32 r) -> u32 { return l % r; },
      +[](u32 l, u32 r) -> u32 { 
        u32 result = i32(l) < i32(r);
        // fprintf(stderr, "wiithin binop: %d < % d = %llu\n", (i32)l, (i32)r, result);
        return i32(l) < i32(r); },
      +[](u32 l, u32 r) -> u32 { return i32(l) <= i32(r); },
      +[](u32 l, u32 r) -> u32 { return i32(l) >  i32(r); },
      +[](u32 l, u32 r) -> u32 { return i32(l) >= i32(r); },
      +[](u32 l, u32 r) -> u32 { return i32(l) == i32(r); },
      +[](u32 l, u32 r) -> u32 { return i32(l) != i32(r); },
      +[](u32 l, u32 r) -> u32 { return l && r; },
      +[](u32 l, u32 r) -> u32 { return l || r; },
  };
  char const *const pats[] = {"=str", "#string", "#array", "#sexp",
                              "#ref", "#val",    "#fun"};
  char const *const lds[] = {"LD", "LDA", "ST"};
  auto operands_stack = stack<u32>{};
  assert((void*)__gc_stack_bottom == (void*)(operands_stack.data.data() + operands_stack.stack_pointer));
  __gc_stack_bottom = operands_stack.data.data() + operands_stack.stack_pointer;
  // pseudo first two args
  operands_stack.push(BOX(0));
  operands_stack.push(BOX(0));
  u32 const GLOBAL = 1;
  u32 const LOCAL =  2;
  u32 const ARG = 3;
  auto create_reference = [&](u32 index, u32 kind) -> u32 {
    if (kind == GLOBAL) {
      return (u32)&operands_stack.data[operands_stack.stack_begin - 1 - index];
    } else if (kind == LOCAL) {
      return (u32)&operands_stack.data[operands_stack.base_pointer + 1 + index];
    } else if (kind == ARG) {
      auto stack_arg_index = operands_stack.base_pointer - 2 - operands_stack.n_args + index;
      return (u32)&operands_stack.data[stack_arg_index];
    } else {
      unsupported();
      return 0;
    }
  };
  auto write_reference = [&](u32 reference, u32 value) -> void {
    *((u32*)reference) = value;
  };
  __init();

  do {
    char x = BYTE, h = (x & 0xF0) >> 4, l = x & 0x0F;
    fprintf(stderr, "0x%.8x:\t", unsigned(ip - bf->code_ptr - 1));
    switch (h) {
    case 15:
      goto stop;

    /* BINOP */
    case 0:
      fprintf(stderr, "BINOP\t%s", ops[l - 1]);
      if (l - 1 <= ops_array.size()) {
        u32 t2 = UNBOX(operands_stack.pop());
        u32 t1 = UNBOX(operands_stack.pop());
        u32 result = ops_array[l - 1](t1, t2);
        operands_stack.push(BOX(result));
      } else {
        unsupported();
      }
      break;

    case 1:
      switch (l) {
      case 0: {
        auto arg = INT;
        operands_stack.push(BOX(arg));
        fprintf(stderr, "CONST\t%d", arg);
        break;
      }

      case 1: {
        char* string = STRING;
        fprintf(stderr, "STRING\t%s", string);
        // i32 n = std::strlen(string);
        char* obj_string = (char*)Bstring((void*)string);
        // fprintf(stderr, "obj_string=%p", (void*)obj_string);
        // data* obj_string = (data*)alloc_string(n + 1);
        fprintf(stderr, "str is %s\n", obj_string);
        operands_stack.push(u32(obj_string));
        // unsupported();
        break;
      }

      case 2: {
        char* tag = STRING;
        int n = INT;
        fprintf(stderr, "SEXP\t%s ", tag);
        operands_stack.print_content();
        fprintf(stderr, "%d", n);
        auto value = myBsexp(n, operands_stack, tag);
        operands_stack.push(u32(value));
        break;
      }

      case 3:
        fprintf(f, "STI");
        unsupported();
        break;

      case 4: {
        fprintf(stderr, "STA");
        auto value = (void*) operands_stack.pop();
        auto i = (int) operands_stack.pop();
        auto x = (void*) operands_stack.pop();
        operands_stack.push((u32)Bsta(value, i, x));
        break;
      }

      case 5: {
        auto jump_location = INT;
        fprintf(stderr, "JMP\t0x%.8x", jump_location);
        ip = bf->code_ptr + jump_location;
        break;
      }

      case 6: {
        fprintf(stderr, "END");
        if (operands_stack.base_pointer != operands_stack.stack_begin + 3) {
          u32 ret_value = operands_stack.pop(); // preserve the boxing kind
          u32 top_n_args = operands_stack.n_args;
          operands_stack.stack_pointer = operands_stack.base_pointer + 1;
          operands_stack.base_pointer = UNBOX(operands_stack.pop());
          operands_stack.n_args = UNBOX(operands_stack.pop());
          u32 ret_ip = operands_stack.pop();
          operands_stack.stack_pointer -= top_n_args;
          __gc_stack_bottom = operands_stack.data.data() + operands_stack.stack_pointer;
          operands_stack.push(ret_value);
          ip = (char*) ret_ip;
        } else {
          goto stop;
        }
        break;
      }

      case 7:
        fprintf(f, "RET");
        unsupported();
        break;

      case 8:
        fprintf(stderr, "DROP");
        if (operands_stack.stack_pointer == 0) {
          printf("Error: negative stack\n");
          exit(-1);
        }
        operands_stack.pop();
        break;

      case 9:
        fprintf(f, "DUP");
        unsupported();
        break;

      case 10:
        fprintf(f, "SWAP");
        unsupported();
        break;

      case 11:{
        fprintf(stderr, "ELEM");
        auto index = (int)operands_stack.pop();
        auto obj = (void*)operands_stack.pop();
        u32 elem = (u32)Belem(obj, index);
        operands_stack.push(elem);
        break;
      }

      default:
        FAIL;
      }
      break;
    case 2:
    case 3:
    case 4:
      fprintf(stderr, "%s\t", lds[h - 2]);
      switch (l) {
      case 0: {
        int n = INT;
        fprintf(stderr, "G(%d)", n);
        if (h == 4) {
          // ST
          operands_stack.data[operands_stack.stack_begin - 1 - n] = operands_stack.top();
          break;
        } else if (h == 2) {
          // LD
          operands_stack.push(operands_stack.data[operands_stack.stack_begin - 1 - n]);
          break;
        } else if (h == 3) {
          // LDA
          auto ref = create_reference(n, GLOBAL);
          operands_stack.push(ref);
          operands_stack.push(ref);
          break;
        }
        unsupported();
        break;
      }
      case 1: { // locals
        int n = INT;
        fprintf(stderr, "L(%d)", n);
        if (h == 4) {
          // ST
          auto top = operands_stack.top();
          operands_stack.data[operands_stack.base_pointer + 1 + n] = top;
          break;
        } else if (h == 2) {
          // LD
          operands_stack.push(operands_stack.data[operands_stack.base_pointer + 1 + n]);
          break;
        } else if (h == 3) {
          // LDA
          auto ref = BOX(create_reference(n, LOCAL));
          operands_stack.push(ref);
          operands_stack.push(ref);
          break;
        }
        unsupported();
        break;
      }
      case 2: { // arg
        int n = INT;
        fprintf(stderr, "A(%d)", n);
        if (h == 4) {
          // ST
          auto top = operands_stack.top();
          auto stack_arg_index = operands_stack.base_pointer - 2 - operands_stack.n_args + n;
          operands_stack.data[stack_arg_index] = top;
          break;
        } else if (h == 2) {
          // LD
          auto stack_arg_index = operands_stack.base_pointer - 2 - operands_stack.n_args + n;
          operands_stack.push(operands_stack.data[stack_arg_index]);
          break;
        } else if (h == 3) {
          // LDA
          auto ref = BOX(create_reference(n, ARG));
          operands_stack.push(ref);
          operands_stack.push(ref);
          break;
        }
        unsupported();
        break;
      }
      case 3:
        fprintf(f, "C(%d)", INT);
        unsupported();
        break;
      default:
        FAIL;
      }
      break;

    case 5:
      switch (l) {
      case 0: {
        auto jump_location = INT;
        fprintf(stderr, "CJMPz\t0x%.8x", jump_location);
        auto top = UNBOX(operands_stack.pop());
        if (top == 0) {
          ip = bf->code_ptr + jump_location;
        }
        break;
      }

      case 1: {
        auto jump_location = INT;
        fprintf(stderr, "CJMPnz\t0x%.8x", jump_location);
        auto top = UNBOX(operands_stack.pop());
        if (top != 0) {
          ip = bf->code_ptr + jump_location;
        }
        break;
      }

      case 2: {
        int n_args = INT;
        int n_locals = INT;
        fprintf(stderr, "BEGIN\t%d ", n_args);
        fprintf(stderr, "%d", n_locals);
        operands_stack.push(BOX(operands_stack.n_args));
        operands_stack.push(BOX(operands_stack.base_pointer));
        operands_stack.n_args = n_args;
        operands_stack.base_pointer = operands_stack.stack_pointer - 1;
        operands_stack.stack_pointer += (n_locals + 1);
        __gc_stack_bottom += (n_locals + 1);
        break;  
      }

      case 3:
        fprintf(f, "CBEGIN\t%d ", INT);
        fprintf(f, "%d", INT);
        unsupported();
        break;

      case 4:
        fprintf(f, "CLOSURE\t0x%.8x", INT);
        {
          unsupported();
          int n = INT;
          for (int i = 0; i < n; i++) {
            switch (BYTE) {
            case 0:
              fprintf(f, "G(%d)", INT);
              unsupported();
              break;
            case 1:
              fprintf(f, "L(%d)", INT);
              unsupported();
              break;
            case 2:
              fprintf(f, "A(%d)", INT);
              unsupported();
              break;
            case 3:
              fprintf(f, "C(%d)", INT);
              unsupported();
              break;
            default:
              FAIL;
            }
          }
        };
        break;

      case 5:
        fprintf(f, "CALLC\t%d", INT);
        unsupported();
        break;

      case 6: {
        int loc = INT;
        fprintf(stderr, "CALL\t0x%.8x ", loc);
        fprintf(stderr, "%d", INT);
        operands_stack.push(u32(ip));
        ip = bf->code_ptr + loc;
        break;
      }

      case 7:
        fprintf(f, "TAG\t%s ", STRING);
        fprintf(f, "%d", INT);
        unsupported();
        break;

      case 8:
        fprintf(f, "ARRAY\t%d", INT);
        unsupported();
        break;

      case 9:
        fprintf(f, "FAIL\t%d", INT);
        fprintf(f, "%d", INT);
        unsupported();
        break;

      case 10:
        fprintf(stderr, "LINE\t%d", INT);
        break;

      default:
        FAIL;
      }
      break;

    case 6:
      fprintf(f, "PATT\t%s", pats[l]);
      unsupported();
      break;

    case 7: {
      switch (l) {
      case 0: {
        fprintf(stderr, "CALL\tLread");
        int result = 0;
        // printf("> ");
        fflush(stdout);
        scanf("%d", &result);
        operands_stack.push(BOX(result));
        break;
      }

      case 1: {
        // operands_stack.print_content();
        u32 value = UNBOX(operands_stack.pop());
        fprintf(stderr, "CALL\tLwrite");
        // fprintf(stdout, "\nout = %d\n", int32_t(value));
        fprintf(stdout, "%d\n", i32(value));
        operands_stack.push(BOX(0));
        break;
      }
      case 2: {
        fprintf(stderr, "CALL\tLlength");
        int value = (int)operands_stack.pop();
        char* str = (char*) value;
        int result = Llength((void*)value);
        operands_stack.push((u32)result);
        break;
      }

      case 3:
        fprintf(f, "CALL\tLstring");
        unsupported();
        break;

      case 4: {
        i32 n = INT;
        fprintf(stderr, "CALL\tBarray\t%d", n);
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

    fprintf(stderr, "\n");
  } while (1);
stop:
  fprintf(stderr, "<end>\n");
}

/* Dumps the contents of the file */
void dump_file(FILE *f, bytefile *bf) {
  int i;

  fprintf(stderr, "String table size       : %d\n", bf->stringtab_size);
  fprintf(stderr, "Global area size        : %d\n", bf->global_area_size);
  fprintf(stderr, "Number of public symbols: %d\n", bf->public_symbols_number);
  fprintf(stderr, "Public symbols          :\n");

  for (i = 0; i < bf->public_symbols_number; i++)
    fprintf(stderr, "   0x%.8x: %s\n", get_public_offset(bf, i),
            get_public_name(bf, i));

  fprintf(stderr, "Code:\n");
  interpret(f, bf);
}

int main(int argc, char *argv[]) {
  bytefile *f = read_file(argv[1]);
  dump_file(stdout, f);
  return 0;
}