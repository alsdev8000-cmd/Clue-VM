/*
 * ClueVM — register-based virtual machine for the Clue language
 * Bytecode files use the .club extension.
 *
 * 16 registers: RA-RH (general purpose), RI-RN (function args),
 *               RO (stack pointer), RP (return value)
 *
 * Types: Bool, Int8, Int16, Int32, Int64, Float32, Float64,
 *        Char, String, Nil
 *
 * Compile: gcc -O2 -o cluevm cluevm.c
 * Run:     ./cluevm
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─── Registers ─────────────────────────────────────────────────────────── */

#define NUM_REGS 16

#define RA  0
#define RB  1
#define RC  2
#define RD  3
#define RE  4
#define RF  5
#define RG  6
#define RH  7
#define RI  8   /* function arg 1 */
#define RJ  9   /* function arg 2 */
#define RK  10  /* function arg 3 */
#define RL  11  /* function arg 4 */
#define RM  12  /* function arg 5 */
#define RN  13  /* function arg 6 */
#define RO  14  /* stack pointer  */
#define RP  15  /* return value   */

/* ─── Value types ───────────────────────────────────────────────────────── */

typedef enum {
    TYPE_NIL,     /* nil — no value                    */
    TYPE_BOOL,    /* true / false  (stored as int8)    */
    TYPE_I8,      /* Int8                              */
    TYPE_I16,     /* Int16                             */
    TYPE_I32,     /* Int32                             */
    TYPE_I64,     /* Int64                             */
    TYPE_F32,     /* Float32                           */
    TYPE_F64,     /* Float64                           */
    TYPE_CHAR,    /* Char  (Unicode codepoint, int32)  */
    TYPE_STR,     /* String (heap-allocated, ref count)*/
} ClueType;

/* ── String heap object ── */
typedef struct ClueStr {
    char*    data;
    size_t   len;
    uint32_t refs;   /* reference count */
} ClueStr;

ClueStr* cluestr_new(const char* src) {
    ClueStr* s = (ClueStr*)malloc(sizeof(ClueStr));
    if (!s) { fprintf(stderr, "[ClueVM] out of memory\n"); exit(1); }
    s->len  = strlen(src);
    s->data = (char*)malloc(s->len + 1);
    if (!s->data) { fprintf(stderr, "[ClueVM] out of memory\n"); exit(1); }
    memcpy(s->data, src, s->len + 1);
    s->refs = 1;
    return s;
}

void cluestr_ref(ClueStr* s)   { if (s) s->refs++; }
void cluestr_unref(ClueStr* s) {
    if (!s) return;
    if (--s->refs == 0) { free(s->data); free(s); }
}

/* ── Tagged value ── */
typedef struct {
    ClueType type;
    union {
        int8_t    i8;
        int16_t   i16;
        int32_t   i32;
        int64_t   i64;
        float     f32;
        double    f64;
        int32_t   chr;    /* Unicode codepoint for Char  */
        int8_t    boolean;/* 0 = false, 1 = true         */
        ClueStr*  str;    /* heap string                 */
    } as;
} ClueVal;

/* Convenience constructors */
static inline ClueVal val_nil()            { ClueVal v; v.type = TYPE_NIL;  v.as.i64 = 0; return v; }
static inline ClueVal val_bool(int b)      { ClueVal v; v.type = TYPE_BOOL; v.as.boolean = (int8_t)(b ? 1 : 0); return v; }
static inline ClueVal val_i8(int8_t x)    { ClueVal v; v.type = TYPE_I8;   v.as.i8  = x; return v; }
static inline ClueVal val_i16(int16_t x)  { ClueVal v; v.type = TYPE_I16;  v.as.i16 = x; return v; }
static inline ClueVal val_i32(int32_t x)  { ClueVal v; v.type = TYPE_I32;  v.as.i32 = x; return v; }
static inline ClueVal val_i64(int64_t x)  { ClueVal v; v.type = TYPE_I64;  v.as.i64 = x; return v; }
static inline ClueVal val_f32(float x)    { ClueVal v; v.type = TYPE_F32;  v.as.f32 = x; return v; }
static inline ClueVal val_f64(double x)   { ClueVal v; v.type = TYPE_F64;  v.as.f64 = x; return v; }
static inline ClueVal val_char(int32_t c) { ClueVal v; v.type = TYPE_CHAR; v.as.chr = c; return v; }
static inline ClueVal val_str(ClueStr* s) { ClueVal v; v.type = TYPE_STR;  v.as.str = s; return v; }

/* ─── Opcodes ───────────────────────────────────────────────────────────── */

typedef enum {
    /* ── data movement ── */
    OP_MOV,       /* MOV   dest  src                   */
    OP_LOAD,      /* LOAD  dest  <imm>                 */
    OP_LOAD_NIL,  /* LOAD_NIL  dest                    */
    OP_LOAD_BOOL, /* LOAD_BOOL dest  0|1               */
    OP_LOAD_CHAR, /* LOAD_CHAR dest  <codepoint imm>   */
    OP_LOAD_STR,  /* LOAD_STR  dest  <string literal>  */

    /* ── arithmetic (Int & Float) ── */
    OP_ADD,       /* ADD  dest  ra  rb                 */
    OP_SUB,       /* SUB  dest  ra  rb                 */
    OP_MUL,       /* MUL  dest  ra  rb                 */
    OP_DIV,       /* DIV  dest  ra  rb                 */
    OP_MOD,       /* MOD  dest  ra  rb  (integers only)*/

    /* ── bitwise (integers only) ── */
    OP_AND,       /* AND  dest  ra  rb                 */
    OP_OR,        /* OR   dest  ra  rb                 */
    OP_XOR,       /* XOR  dest  ra  rb                 */
    OP_NOT,       /* NOT  dest  ra       (bitwise ~)   */
    OP_SHL,       /* SHL  dest  ra  rb                 */
    OP_SHR,       /* SHR  dest  ra  rb                 */

    /* ── logical (Bool) ── */
    OP_LAND,      /* LAND dest  ra  rb  (bool &&)      */
    OP_LOR,       /* LOR  dest  ra  rb  (bool ||)      */
    OP_LNOT,      /* LNOT dest  ra      (bool !)       */

    /* ── compare → Bool result ── */
    OP_CMP_EQ,
    OP_CMP_NEQ,
    OP_CMP_LT,
    OP_CMP_GT,
    OP_CMP_LE,
    OP_CMP_GE,

    /* ── nil check ── */
    OP_IS_NIL,    /* IS_NIL  dest  src  → Bool         */

    /* ── string ops ── */
    OP_STR_CAT,   /* STR_CAT  dest  ra  rb             */
    OP_STR_LEN,   /* STR_LEN  dest  src  → I64         */

    /* ── jump ── */
    OP_JMP,       /* JMP   offset                      */
    OP_JIF,       /* JIF   cond_reg  offset  (if true) */
    OP_JNIF,      /* JNIF  cond_reg  offset  (if false)*/

    /* ── call / return ── */
    OP_CALL,      /* CALL  func_index                  */
    OP_RET,       /* RET   (value already in RP)       */

    /* ── i/o ── */
    OP_PRINT,     /* PRINT  src_reg                    */
    OP_PRINT_LN,  /* PRINT_LN  src_reg  (+ newline)    */

    OP_HALT,
} Opcode;

/* ─── Instruction ───────────────────────────────────────────────────────── */

typedef struct {
    Opcode   opcode;
    uint8_t  dest;
    uint8_t  a;
    uint8_t  b;
    ClueVal  imm;       /* used by LOAD* instructions */
    int32_t  offset;    /* used by jump instructions  */
} Instruction;

/* ─── Call frame ────────────────────────────────────────────────────────── */

#define MAX_CALL_DEPTH 512

typedef struct {
    size_t  return_ip;
    ClueVal saved_regs[NUM_REGS];
} CallFrame;

/* ─── VM ────────────────────────────────────────────────────────────────── */

typedef struct {
    ClueVal   regs[NUM_REGS];
    CallFrame call_stack[MAX_CALL_DEPTH];
    int       call_depth;
} ClueVM;

/* ─── Function table ────────────────────────────────────────────────────── */

typedef struct {
    const char* name;
    size_t      entry;
} ClueFunc;

#define MAX_FUNCS 256

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static void vm_error(const char* msg) {
    fprintf(stderr, "[ClueVM error] %s\n", msg);
    exit(1);
}

/* Release string refs in a register bank */
static void regs_unref_strings(ClueVal* regs) {
    for (int i = 0; i < NUM_REGS; i++)
        if (regs[i].type == TYPE_STR)
            cluestr_unref(regs[i].as.str);
}

/* Assign to a register, managing string refs */
static void reg_set(ClueVal* regs, int idx, ClueVal v) {
    if (regs[idx].type == TYPE_STR) cluestr_unref(regs[idx].as.str);
    if (v.type == TYPE_STR)         cluestr_ref(v.as.str);
    regs[idx] = v;
}

static void print_val(ClueVal v) {
    switch (v.type) {
        case TYPE_NIL:  printf("nil");                          break;
        case TYPE_BOOL: printf("%s", v.as.boolean ? "true" : "false"); break;
        case TYPE_I8:   printf("%d",   (int)v.as.i8);          break;
        case TYPE_I16:  printf("%d",   (int)v.as.i16);         break;
        case TYPE_I32:  printf("%d",   v.as.i32);              break;
        case TYPE_I64:  printf("%lld", (long long)v.as.i64);   break;
        case TYPE_F32:  printf("%g",   (double)v.as.f32);      break;
        case TYPE_F64:  printf("%g",   v.as.f64);              break;
        case TYPE_CHAR: {
            /* UTF-8 encode the codepoint */
            int32_t cp = v.as.chr;
            if (cp < 0x80)        { printf("%c", (char)cp); }
            else if (cp < 0x800)  { printf("%c%c", 0xC0|(cp>>6), 0x80|(cp&0x3F)); }
            else if (cp < 0x10000){ printf("%c%c%c", 0xE0|(cp>>12), 0x80|((cp>>6)&0x3F), 0x80|(cp&0x3F)); }
            else                  { printf("%c%c%c%c", 0xF0|(cp>>18), 0x80|((cp>>12)&0x3F), 0x80|((cp>>6)&0x3F), 0x80|(cp&0x3F)); }
            break;
        }
        case TYPE_STR:  printf("%s", v.as.str ? v.as.str->data : "nil"); break;
    }
}

/* ─── Arithmetic helpers ─────────────────────────────────────────────────── */

#define ARITH_OP(NAME, OP)                                                   \
static ClueVal arith_##NAME(ClueVal a, ClueVal b) {                          \
    if (a.type != b.type) vm_error(#NAME ": type mismatch");                 \
    switch (a.type) {                                                         \
        case TYPE_I8:  return val_i8 (a.as.i8  OP b.as.i8);                 \
        case TYPE_I16: return val_i16(a.as.i16 OP b.as.i16);                \
        case TYPE_I32: return val_i32(a.as.i32 OP b.as.i32);                \
        case TYPE_I64: return val_i64(a.as.i64 OP b.as.i64);                \
        case TYPE_F32: return val_f32(a.as.f32 OP b.as.f32);                \
        case TYPE_F64: return val_f64(a.as.f64 OP b.as.f64);                \
        default: vm_error(#NAME ": unsupported type"); return val_nil();     \
    }                                                                         \
}

ARITH_OP(add, +)
ARITH_OP(sub, -)
ARITH_OP(mul, *)

static ClueVal arith_div(ClueVal a, ClueVal b) {
    if (a.type != b.type) vm_error("DIV: type mismatch");
    switch (a.type) {
        case TYPE_I8:  if (!b.as.i8)  vm_error("DIV: division by zero"); return val_i8 (a.as.i8  / b.as.i8);
        case TYPE_I16: if (!b.as.i16) vm_error("DIV: division by zero"); return val_i16(a.as.i16 / b.as.i16);
        case TYPE_I32: if (!b.as.i32) vm_error("DIV: division by zero"); return val_i32(a.as.i32 / b.as.i32);
        case TYPE_I64: if (!b.as.i64) vm_error("DIV: division by zero"); return val_i64(a.as.i64 / b.as.i64);
        case TYPE_F32: return val_f32(a.as.f32 / b.as.f32);
        case TYPE_F64: return val_f64(a.as.f64 / b.as.f64);
        default: vm_error("DIV: unsupported type"); return val_nil();
    }
}

static ClueVal arith_mod(ClueVal a, ClueVal b) {
    if (a.type != b.type) vm_error("MOD: type mismatch");
    switch (a.type) {
        case TYPE_I8:  if (!b.as.i8)  vm_error("MOD: division by zero"); return val_i8 (a.as.i8  % b.as.i8);
        case TYPE_I16: if (!b.as.i16) vm_error("MOD: division by zero"); return val_i16(a.as.i16 % b.as.i16);
        case TYPE_I32: if (!b.as.i32) vm_error("MOD: division by zero"); return val_i32(a.as.i32 % b.as.i32);
        case TYPE_I64: if (!b.as.i64) vm_error("MOD: division by zero"); return val_i64(a.as.i64 % b.as.i64);
        default: vm_error("MOD: only integer types supported"); return val_nil();
    }
}

/* ─── Bitwise helpers ────────────────────────────────────────────────────── */

#define BITWISE_OP(NAME, OP)                                                 \
static ClueVal bitwise_##NAME(ClueVal a, ClueVal b) {                        \
    if (a.type != b.type) vm_error(#NAME ": type mismatch");                 \
    switch (a.type) {                                                         \
        case TYPE_I8:  return val_i8 ((int8_t) (a.as.i8  OP b.as.i8));      \
        case TYPE_I16: return val_i16((int16_t)(a.as.i16 OP b.as.i16));     \
        case TYPE_I32: return val_i32(a.as.i32 OP b.as.i32);                \
        case TYPE_I64: return val_i64(a.as.i64 OP b.as.i64);                \
        default: vm_error(#NAME ": only integer types supported"); return val_nil(); \
    }                                                                         \
}

BITWISE_OP(and, &)
BITWISE_OP(or,  |)
BITWISE_OP(xor, ^)
BITWISE_OP(shl, <<)
BITWISE_OP(shr, >>)

static ClueVal bitwise_not(ClueVal a) {
    switch (a.type) {
        case TYPE_I8:  return val_i8 ((int8_t) ~a.as.i8);
        case TYPE_I16: return val_i16((int16_t)~a.as.i16);
        case TYPE_I32: return val_i32(~a.as.i32);
        case TYPE_I64: return val_i64(~a.as.i64);
        default: vm_error("NOT: only integer types supported"); return val_nil();
    }
}

/* ─── Compare helpers ────────────────────────────────────────────────────── */

#define CMP_NUMBERS(a, b, OP)                   \
    switch (a.type) {                            \
        case TYPE_I8:  return val_bool(a.as.i8  OP b.as.i8);  \
        case TYPE_I16: return val_bool(a.as.i16 OP b.as.i16); \
        case TYPE_I32: return val_bool(a.as.i32 OP b.as.i32); \
        case TYPE_I64: return val_bool(a.as.i64 OP b.as.i64); \
        case TYPE_F32: return val_bool(a.as.f32 OP b.as.f32); \
        case TYPE_F64: return val_bool(a.as.f64 OP b.as.f64); \
        case TYPE_CHAR:return val_bool(a.as.chr OP b.as.chr);  \
        default: break;                          \
    }

static ClueVal cmp_eq(ClueVal a, ClueVal b) {
    if (a.type == TYPE_NIL && b.type == TYPE_NIL) return val_bool(1);
    if (a.type == TYPE_NIL || b.type == TYPE_NIL) return val_bool(0);
    if (a.type != b.type) return val_bool(0);
    if (a.type == TYPE_BOOL) return val_bool(a.as.boolean == b.as.boolean);
    if (a.type == TYPE_STR) {
        if (!a.as.str || !b.as.str) return val_bool(a.as.str == b.as.str);
        return val_bool(strcmp(a.as.str->data, b.as.str->data) == 0);
    }
    CMP_NUMBERS(a, b, ==)
    vm_error("CMP_EQ: unsupported type"); return val_nil();
}

static ClueVal cmp_lt(ClueVal a, ClueVal b) {
    if (a.type != b.type) vm_error("CMP_LT: type mismatch");
    CMP_NUMBERS(a, b, <)
    vm_error("CMP_LT: unsupported type"); return val_nil();
}

static ClueVal cmp_gt(ClueVal a, ClueVal b) {
    if (a.type != b.type) vm_error("CMP_GT: type mismatch");
    CMP_NUMBERS(a, b, >)
    vm_error("CMP_GT: unsupported type"); return val_nil();
}

static ClueVal cmp_le(ClueVal a, ClueVal b) {
    if (a.type != b.type) vm_error("CMP_LE: type mismatch");
    CMP_NUMBERS(a, b, <=)
    vm_error("CMP_LE: unsupported type"); return val_nil();
}

static ClueVal cmp_ge(ClueVal a, ClueVal b) {
    if (a.type != b.type) vm_error("CMP_GE: type mismatch");
    CMP_NUMBERS(a, b, >=)
    vm_error("CMP_GE: unsupported type"); return val_nil();
}

/* ─── String ops ─────────────────────────────────────────────────────────── */

static ClueVal str_cat(ClueVal a, ClueVal b) {
    if (a.type != TYPE_STR || b.type != TYPE_STR)
        vm_error("STR_CAT: both operands must be String");
    const char* sa = a.as.str ? a.as.str->data : "";
    const char* sb = b.as.str ? b.as.str->data : "";
    size_t la = strlen(sa), lb = strlen(sb);
    char* buf = (char*)malloc(la + lb + 1);
    if (!buf) { fprintf(stderr, "[ClueVM] out of memory\n"); exit(1); }
    memcpy(buf, sa, la);
    memcpy(buf + la, sb, lb + 1);
    ClueStr* s = cluestr_new(buf);
    free(buf);
    return val_str(s);
}

/* ─── Execute ───────────────────────────────────────────────────────────── */

void cluevm_run(ClueVM* vm,
                Instruction* program, size_t prog_len,
                ClueFunc*    funcs,   size_t func_count,
                size_t       entry_ip)
{
    size_t ip = entry_ip;

    while (ip < prog_len) {
        Instruction* ins = &program[ip];

        switch (ins->opcode) {

            /* ── Data movement ── */
            case OP_MOV:
                reg_set(vm->regs, ins->dest, vm->regs[ins->a]);
                ip++; break;

            case OP_LOAD:
                reg_set(vm->regs, ins->dest, ins->imm);
                ip++; break;

            case OP_LOAD_NIL:
                reg_set(vm->regs, ins->dest, val_nil());
                ip++; break;

            case OP_LOAD_BOOL:
                reg_set(vm->regs, ins->dest, val_bool(ins->imm.as.boolean));
                ip++; break;

            case OP_LOAD_CHAR:
                reg_set(vm->regs, ins->dest, val_char(ins->imm.as.chr));
                ip++; break;

            case OP_LOAD_STR: {
                ClueStr* s = cluestr_new(ins->imm.as.str ? ins->imm.as.str->data : "");
                reg_set(vm->regs, ins->dest, val_str(s));
                cluestr_unref(s); /* reg_set already reffed it */
                ip++; break;
            }

            /* ── Arithmetic ── */
            case OP_ADD: reg_set(vm->regs, ins->dest, arith_add(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_SUB: reg_set(vm->regs, ins->dest, arith_sub(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_MUL: reg_set(vm->regs, ins->dest, arith_mul(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_DIV: reg_set(vm->regs, ins->dest, arith_div(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_MOD: reg_set(vm->regs, ins->dest, arith_mod(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;

            /* ── Bitwise ── */
            case OP_AND: reg_set(vm->regs, ins->dest, bitwise_and(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_OR:  reg_set(vm->regs, ins->dest, bitwise_or (vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_XOR: reg_set(vm->regs, ins->dest, bitwise_xor(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_NOT: reg_set(vm->regs, ins->dest, bitwise_not(vm->regs[ins->a]));                   ip++; break;
            case OP_SHL: reg_set(vm->regs, ins->dest, bitwise_shl(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_SHR: reg_set(vm->regs, ins->dest, bitwise_shr(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;

            /* ── Logical ── */
            case OP_LAND:
                reg_set(vm->regs, ins->dest,
                    val_bool(vm->regs[ins->a].as.boolean && vm->regs[ins->b].as.boolean));
                ip++; break;

            case OP_LOR:
                reg_set(vm->regs, ins->dest,
                    val_bool(vm->regs[ins->a].as.boolean || vm->regs[ins->b].as.boolean));
                ip++; break;

            case OP_LNOT:
                reg_set(vm->regs, ins->dest,
                    val_bool(!vm->regs[ins->a].as.boolean));
                ip++; break;

            /* ── Compare ── */
            case OP_CMP_EQ:  reg_set(vm->regs, ins->dest, cmp_eq(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_CMP_NEQ: reg_set(vm->regs, ins->dest, val_bool(!cmp_eq(vm->regs[ins->a], vm->regs[ins->b]).as.boolean)); ip++; break;
            case OP_CMP_LT:  reg_set(vm->regs, ins->dest, cmp_lt(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_CMP_GT:  reg_set(vm->regs, ins->dest, cmp_gt(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_CMP_LE:  reg_set(vm->regs, ins->dest, cmp_le(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;
            case OP_CMP_GE:  reg_set(vm->regs, ins->dest, cmp_ge(vm->regs[ins->a], vm->regs[ins->b])); ip++; break;

            /* ── Nil check ── */
            case OP_IS_NIL:
                reg_set(vm->regs, ins->dest, val_bool(vm->regs[ins->a].type == TYPE_NIL));
                ip++; break;

            /* ── String ops ── */
            case OP_STR_CAT:
                reg_set(vm->regs, ins->dest, str_cat(vm->regs[ins->a], vm->regs[ins->b]));
                ip++; break;

            case OP_STR_LEN:
                if (vm->regs[ins->a].type != TYPE_STR) vm_error("STR_LEN: expected String");
                reg_set(vm->regs, ins->dest,
                    val_i64((int64_t)(vm->regs[ins->a].as.str ? vm->regs[ins->a].as.str->len : 0)));
                ip++; break;

            /* ── Jumps ── */
            case OP_JMP:
                ip = (size_t)((int)ip + ins->offset);
                break;

            case OP_JIF:
                if (vm->regs[ins->a].as.boolean)
                    ip = (size_t)((int)ip + ins->offset);
                else
                    ip++;
                break;

            case OP_JNIF:
                if (!vm->regs[ins->a].as.boolean)
                    ip = (size_t)((int)ip + ins->offset);
                else
                    ip++;
                break;

            /* ── Call / return ── */
            case OP_CALL: {
                if (vm->call_depth >= MAX_CALL_DEPTH)
                    vm_error("CALL: call stack overflow");
                uint32_t fi = (uint32_t)ins->imm.as.i32;
                if (fi >= func_count) vm_error("CALL: unknown function index");

                CallFrame* frame = &vm->call_stack[vm->call_depth++];
                frame->return_ip = ip + 1;
                /* save registers (add string refs for saved copies) */
                memcpy(frame->saved_regs, vm->regs, sizeof(vm->regs));
                for (int i = 0; i < NUM_REGS; i++)
                    if (frame->saved_regs[i].type == TYPE_STR)
                        cluestr_ref(frame->saved_regs[i].as.str);

                ip = funcs[fi].entry;
                break;
            }

            case OP_RET: {
                if (vm->call_depth == 0) vm_error("RET: call stack underflow");
                CallFrame* frame = &vm->call_stack[--vm->call_depth];

                /* keep return value across register restore */
                ClueVal ret = vm->regs[RP];
                if (ret.type == TYPE_STR) cluestr_ref(ret.as.str);

                /* release current string registers */
                regs_unref_strings(vm->regs);

                /* restore saved registers */
                memcpy(vm->regs, frame->saved_regs, sizeof(vm->regs));
                /* (saved_regs refs were already accounted for on CALL) */

                /* put return value back */
                reg_set(vm->regs, RP, ret);
                if (ret.type == TYPE_STR) cluestr_unref(ret.as.str);

                ip = frame->return_ip;
                break;
            }

            /* ── Print ── */
            case OP_PRINT:
                print_val(vm->regs[ins->a]);
                ip++; break;

            case OP_PRINT_LN:
                print_val(vm->regs[ins->a]);
                printf("\n");
                ip++; break;

            case OP_HALT:
                return;

            default:
                vm_error("unknown opcode");
        }
    }
}

/* ─── cluevm_init ───────────────────────────────────────────────────────── */

void cluevm_init(ClueVM* vm) {
    memset(vm, 0, sizeof(*vm));
    for (int i = 0; i < NUM_REGS; i++) vm->regs[i] = val_nil();
}

#ifndef __CLUEVM_LIBRARY__

/* ─── main: takes a generated .c file, compiles it, and runs it ─────────── */
/*
 * Usage: ./cluevm program.c
 *
 * cluevm compiles the generated .c file (produced by clueasm) by linking it
 * against itself, then executes the result. The generated .c file must call
 * cluevm_init() and cluevm_run() which are provided by this file.
 *
 * Full pipeline:
 *   ./clueasm program.club program.c   # assemble .club → .c
 *   ./cluevm  program.c                # compile .c + run
 */

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: cluevm <program.c>\n");
        fprintf(stderr, "  (generate program.c with: clueasm program.club program.c)\n");
        return 1;
    }

    const char* src = argv[1];

    /* build a temp binary path next to the source file */
    char bin[1024];
    #ifdef _WIN32
    snprintf(bin, sizeof(bin), "%s.exe", src);
#else
    snprintf(bin, sizeof(bin), "%s.out", src);
#endif

    /* find path to this cluevm executable so we can link against it */
    char self[1024];
#ifdef __linux__
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (len < 0) { perror("readlink"); return 1; }
    self[len] = '\0';
#else
    strncpy(self, argv[0], sizeof(self)-1);
#endif

    /* compile:  gcc -O2 program.c cluevm.c -o program.c.out
     * We pass __CLUEVM_LIBRARY__ so cluevm.c skips its own main()  */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "gcc -O2 -D__CLUEVM_LIBRARY__ \"%s\" \"%s\" -o \"%s\" 2>&1",
             src, self, bin);

    /* gcc can't link against a running executable directly;
     * instead we recompile cluevm.c from source.
     * Locate cluevm.c next to the binary. */
    char vmdir[1024]; strncpy(vmdir, self, sizeof(vmdir)-1);
    char* slash = strrchr(vmdir, '/');
    if (slash) slash[1] = '\0'; else vmdir[0] = '\0';

    char vmsrc[1024];
    snprintf(vmsrc, sizeof(vmsrc), "%scluevm.c", vmdir);

    /* Check cluevm.c exists beside the binary */
    FILE* test = fopen(vmsrc, "r");
    if (!test) {
        /* fallback: look in current directory */
        snprintf(vmsrc, sizeof(vmsrc), "cluevm.c");
        test = fopen(vmsrc, "r");
        if (!test) {
            fprintf(stderr, "[ClueVM] cannot find cluevm.c to link against.\n");
            fprintf(stderr, "         Put cluevm.c in the same directory as cluevm.\n");
            return 1;
        }
    }
    fclose(test);

    /* The generated .c already #includes cluevm.c, so compile it alone.
     * __CLUEVM_LIBRARY__ suppresses cluevm.c's own main(). */
    snprintf(cmd, sizeof(cmd),
             "gcc -O2 -D__CLUEVM_LIBRARY__ \"%s\" -o \"%s\" 2>&1",
             src, bin);

    printf("[ClueVM] compiling %s ...\n", src);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[ClueVM] compilation failed.\n");
        return 1;
    }

    /* run the compiled binary */
    char run[1100];
#ifdef _WIN32
    snprintf(run, sizeof(run), "%s", bin);
#else
    if (strchr(bin, '/'))
        snprintf(run, sizeof(run), "%s", bin);
    else
        snprintf(run, sizeof(run), "./%s", bin);
#endif
    ret = system(run);

    /* clean up temp binary */
    remove(bin);

    return ret;
}

#endif /* __CLUEVM_LIBRARY__ */
