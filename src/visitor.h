#include <cstdint>

using i32 = std::int32_t;
using u8 = std::uint8_t;

template <typename T>
class Visitor {
public:
  virtual ~Visitor() = default;
  virtual T visit_binop(u8* decode_next_ip, u8 index) = 0;
  virtual T visit_const(u8* decode_next_ip, i32 constant) = 0;
  virtual T visit_str(u8* decode_next_ip, char const*) = 0;
  virtual T visit_sexp(u8* decode_next_ip, char const* tag, i32 args) = 0;
  virtual T visit_sti(u8* decode_next_ip) = 0;
  virtual T visit_sta(u8* decode_next_ip) = 0;
  virtual T visit_jmp(u8* decode_next_ip, i32 jump_location) = 0;
  virtual T visit_end_ret(u8* decode_next_ip) = 0;
  virtual T visit_drop(u8* decode_next_ip) = 0;
  virtual T visit_dup(u8* decode_next_ip) = 0;
  virtual T visit_swap(u8* decode_next_ip) = 0;
  virtual T visit_elem(u8* decode_next_ip) = 0;
  virtual T visit_ld(u8* decode_next_ip, u8 arg_kind, i32 index) = 0;
  virtual T visit_lda(u8* decode_next_ip, u8 arg_kind, i32 index) = 0;
  virtual T visit_st(u8* decode_next_ip, u8 arg_kind, i32 index) = 0;
  virtual T visit_cjmp(u8* decode_next_ip, u8 is_negated, i32 jump_location) = 0;
  virtual T visit_begin(u8* decode_next_ip, u8 is_closure_begin, i32 n_args, i32 n_locals) = 0;
  virtual T visit_closure(u8* decode_next_ip, i32 addr, i32 n, u8* args_begin) = 0;
  virtual T visit_call_closure(u8* decode_next_ip, i32 n_arg) = 0;
  virtual T visit_call(u8* decode_next_ip, i32 loc, i32 n_arg) = 0;
  virtual T visit_tag(u8* decode_next_ip, char const* name, i32 n_arg) = 0;
  virtual T visit_array(u8* decode_next_ip, i32 size) = 0;
  virtual T visit_fail(u8* decode_next_ip, i32 arg1, i32 arg2) = 0;
  virtual T visit_line(u8* decode_next_ip, i32 line_number) = 0;
  virtual T visit_patt(u8* decode_next_ip, u8 patt_kind) = 0;
  virtual T visit_call_lread(u8* decode_next_ip) = 0;
  virtual T visit_call_lwrite(u8* decode_next_ip) = 0;
  virtual T visit_call_llength(u8* decode_next_ip) = 0;
  virtual T visit_call_lstring(u8* decode_next_ip) = 0;
  virtual T visit_call_barray(u8* decode_next_ip, i32 arg) = 0;
  virtual T visit_stop(u8* decode_next_ip) = 0;
};
