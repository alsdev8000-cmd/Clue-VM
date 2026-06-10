/*
 * ClueASM — assembles a .club text file into a generated .c file
 *           containing Instruction[] structs for ClueVM to compile & run.
 *
 * Compile: gcc -O2 -o clueasm clueasm.c
 * Usage:   ./clueasm program.club output.c
 *
 * The generated output.c #includes cluevm.h and has a main() that
 * builds the Instruction array and calls cluevm_run().
 *
 * .club syntax:
 *   # comment
 *
 *   func greet:
 *     LOAD_STR  RA  "Hello, "
 *     STR_CAT   RP  RA  RI
 *     RET
 *   end
 *
 *   main:
 *     LOAD      RA  I32  10
 *     LOAD_STR  RI  "Clue"
 *     CALL greet
 *     PRINT_LN  RP
 *     HALT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#define MAX_INSTRUCTIONS 4096
#define MAX_FUNCS        256
#define MAX_LABELS       512
#define MAX_LINE         1024
#define MAX_NAME         64
#define MAX_TOKS         32

/* ── Register names ── */
static int parse_reg(const char* s, int line) {
    const char* names[] = {"RA","RB","RC","RD","RE","RF","RG","RH",
                           "RI","RJ","RK","RL","RM","RN","RO","RP"};
    for (int i = 0; i < 16; i++)
        if (strcmp(s, names[i]) == 0) return i;
    fprintf(stderr, "[ClueASM] line %d: unknown register '%s'\n", line, s);
    exit(1);
}

/* ── Type names ── */
static const char* parse_type(const char* s, int line) {
    const char* types[] = {"I8","I16","I32","I64","F32","F64",NULL};
    for (int i = 0; types[i]; i++)
        if (strcmp(s, types[i]) == 0) return types[i];
    fprintf(stderr, "[ClueASM] line %d: unknown type '%s'\n", line, s);
    exit(1);
}

/* ── Structs for two-pass assembly ── */
typedef struct { size_t instr_index; char name[MAX_NAME]; } Patch;
typedef struct { char name[MAX_NAME]; size_t ip; }          Label;
typedef struct { char name[MAX_NAME]; size_t entry; }       Func;

/* Each assembled instruction stored as a string of C initialiser fields */
typedef struct {
    char text[512];   /* the full { .opcode=..., ... } initialiser text */
} AsmInstr;

static AsmInstr g_instrs[MAX_INSTRUCTIONS];
static size_t   g_instr_count = 0;

static Func     g_funcs[MAX_FUNCS];
static size_t   g_func_count = 0;

static Label    g_labels[MAX_LABELS];
static size_t   g_label_count = 0;

static Patch    g_jump_patches[MAX_LABELS];  /* jump needs offset patched */
static size_t   g_jump_patch_count = 0;

static Patch    g_call_patches[MAX_FUNCS];   /* call needs func index patched */
static size_t   g_call_patch_count = 0;

static size_t   g_entry_ip  = 0;
static int      g_entry_set = 0;

static void asm_error(int line, const char* msg) {
    fprintf(stderr, "[ClueASM] line %d: %s\n", line, msg);
    exit(1);
}

static void add_label(const char* name, size_t ip, int line) {
    for (size_t i = 0; i < g_label_count; i++)
        if (strcmp(g_labels[i].name, name) == 0) asm_error(line, "duplicate label");
    strncpy(g_labels[g_label_count].name, name, MAX_NAME-1);
    g_labels[g_label_count].ip = ip;
    g_label_count++;
}

static size_t find_label(const char* name) {
    for (size_t i = 0; i < g_label_count; i++)
        if (strcmp(g_labels[i].name, name) == 0) return g_labels[i].ip;
    fprintf(stderr, "[ClueASM] undefined label '%s'\n", name);
    exit(1);
}

static size_t find_func(const char* name) {
    for (size_t i = 0; i < g_func_count; i++)
        if (strcmp(g_funcs[i].name, name) == 0) return i;
    fprintf(stderr, "[ClueASM] undefined function '%s'\n", name);
    exit(1);
}

static void rtrim(char* s) {
    int n = (int)strlen(s)-1;
    while (n >= 0 && (s[n]=='\n'||s[n]=='\r'||s[n]==' '||s[n]=='\t')) s[n--]='\0';
}

/* tokeniser — keeps quoted strings as one token */
static int tokenise(char* buf, char* toks[], int max) {
    int n = 0; char* p = buf;
    while (*p && n < max) {
        while (*p==' '||*p=='\t') p++;
        if (!*p || *p=='#') break;
        if (*p == '"') {
            toks[n++] = p++;
            while (*p && !(*p=='"' && *(p-1)!='\\')) p++;
            if (*p=='"') p++;
            if (*p) *p++='\0';
        } else {
            toks[n++] = p;
            while (*p && *p!=' ' && *p!='\t' && *p!='#') p++;
            if (*p) *p++='\0';
        }
    }
    return n;
}

/* escape a string for embedding in a C string literal */
static void c_escape(const char* src, char* dst, size_t dsz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j+4 < dsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if      (c=='"')  { dst[j++]='\\'; dst[j++]='"'; }
        else if (c=='\\') { dst[j++]='\\'; dst[j++]='\\'; }
        else if (c=='\n') { dst[j++]='\\'; dst[j++]='n'; }
        else if (c=='\t') { dst[j++]='\\'; dst[j++]='t'; }
        else if (c=='\r') { dst[j++]='\\'; dst[j++]='r'; }
        else              { dst[j++]=c; }
    }
    dst[j]='\0';
}

/* parse a string literal token (strips quotes, handles escapes) into buf */
static void parse_strlit(const char* tok, char* out, size_t outsz, int line) {
    if (tok[0]!='"') asm_error(line, "expected string literal");
    size_t j=0; size_t i=1;
    while (tok[i] && tok[i]!='"' && j+1<outsz) {
        if (tok[i]=='\\') {
            i++;
            switch(tok[i]) {
                case 'n': out[j++]='\n'; break;
                case 't': out[j++]='\t'; break;
                case 'r': out[j++]='\r'; break;
                case '"': out[j++]='"';  break;
                case '\\':out[j++]='\\'; break;
                default: out[j++]=tok[i]; break;
            }
        } else { out[j++]=tok[i]; }
        i++;
    }
    out[j]='\0';
}

/* ── emit a simple instruction with just opcode ── */
#define EMIT(fmt, ...) \
    snprintf(g_instrs[g_instr_count].text, 512, fmt, ##__VA_ARGS__); \
    g_instr_count++

static void assemble(FILE* f) {
    char line_buf[MAX_LINE];
    int  line_no=0, in_func=0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        line_no++;
        rtrim(line_buf);
        char* toks[MAX_TOKS];
        int n = tokenise(line_buf, toks, MAX_TOKS);
        if (n==0) continue;

        /* func name: */
        if (strcmp(toks[0],"func")==0) {
            if (in_func) asm_error(line_no,"nested func");
            if (n<2) asm_error(line_no,"func needs name");
            char fname[MAX_NAME]; strncpy(fname,toks[1],MAX_NAME-1);
            size_t fl=strlen(fname); if(fname[fl-1]==':') fname[fl-1]='\0';
            g_funcs[g_func_count].entry = g_instr_count;
            strncpy(g_funcs[g_func_count].name, fname, MAX_NAME-1);
            g_func_count++; in_func=1; continue;
        }
        if (strcmp(toks[0],"end")==0) { in_func=0; continue; }

        /* label: */
        if (n==1 && toks[0][strlen(toks[0])-1]==':') {
            char lname[MAX_NAME]; strncpy(lname,toks[0],MAX_NAME-1);
            lname[strlen(lname)-1]='\0';
            add_label(lname, g_instr_count, line_no);
            if (!in_func && strcmp(lname,"main")==0) { g_entry_ip=g_instr_count; g_entry_set=1; }
            continue;
        }

        if (g_instr_count >= MAX_INSTRUCTIONS) asm_error(line_no,"too many instructions");

        /* ── HALT / RET ── */
        if (strcmp(toks[0],"HALT")==0) { EMIT("{OP_HALT}"); continue; }
        if (strcmp(toks[0],"RET") ==0) { EMIT("{OP_RET}");  continue; }

        /* ── Jumps ── */
        if (strcmp(toks[0],"JMP")==0) {
            if (n<2) asm_error(line_no,"JMP needs label");
            strncpy(g_jump_patches[g_jump_patch_count].name, toks[1], MAX_NAME-1);
            g_jump_patches[g_jump_patch_count].instr_index = g_instr_count;
            g_jump_patch_count++;
            EMIT("{OP_JMP, .offset=0/*patch*/}"); continue;
        }
        if (strcmp(toks[0],"JIF")==0) {
            if (n<3) asm_error(line_no,"JIF needs reg label");
            int ra = parse_reg(toks[1],line_no);
            strncpy(g_jump_patches[g_jump_patch_count].name, toks[2], MAX_NAME-1);
            g_jump_patches[g_jump_patch_count].instr_index = g_instr_count;
            g_jump_patch_count++;
            EMIT("{OP_JIF, .a=%d, .offset=0/*patch*/}", ra); continue;
        }
        if (strcmp(toks[0],"JNIF")==0) {
            if (n<3) asm_error(line_no,"JNIF needs reg label");
            int ra = parse_reg(toks[1],line_no);
            strncpy(g_jump_patches[g_jump_patch_count].name, toks[2], MAX_NAME-1);
            g_jump_patches[g_jump_patch_count].instr_index = g_instr_count;
            g_jump_patch_count++;
            EMIT("{OP_JNIF, .a=%d, .offset=0/*patch*/}", ra); continue;
        }

        /* ── CALL ── */
        if (strcmp(toks[0],"CALL")==0) {
            if (n<2) asm_error(line_no,"CALL needs func name");
            strncpy(g_call_patches[g_call_patch_count].name, toks[1], MAX_NAME-1);
            g_call_patches[g_call_patch_count].instr_index = g_instr_count;
            g_call_patch_count++;
            EMIT("{OP_CALL, .imm={.type=TYPE_I32,.as.i32=0}/*patch*/}"); continue;
        }

        /* ── PRINT / PRINT_LN ── */
        if (strcmp(toks[0],"PRINT")==0||strcmp(toks[0],"PRINT_LN")==0) {
            if (n<2) asm_error(line_no,"PRINT needs reg");
            int ra=parse_reg(toks[1],line_no);
            const char* op = strcmp(toks[0],"PRINT_LN")==0 ? "OP_PRINT_LN" : "OP_PRINT";
            EMIT("{%s, .a=%d}", op, ra); continue;
        }

        /* ── MOV dest src ── */
        if (strcmp(toks[0],"MOV")==0) {
            if (n<3) asm_error(line_no,"MOV needs dest src");
            int dest=parse_reg(toks[1],line_no), ra=parse_reg(toks[2],line_no);
            EMIT("{OP_MOV, .dest=%d, .a=%d}", dest, ra); continue;
        }

        /* ── LOAD dest type imm ── */
        if (strcmp(toks[0],"LOAD")==0) {
            if (n<4) asm_error(line_no,"LOAD needs dest type imm");
            int dest=parse_reg(toks[1],line_no);
            const char* ty=parse_type(toks[2],line_no);
            char field[8];
            if      (strcmp(ty,"I8") ==0) strcpy(field,"i8");
            else if (strcmp(ty,"I16")==0) strcpy(field,"i16");
            else if (strcmp(ty,"I32")==0) strcpy(field,"i32");
            else if (strcmp(ty,"I64")==0) strcpy(field,"i64");
            else if (strcmp(ty,"F32")==0) strcpy(field,"f32");
            else                          strcpy(field,"f64");
            EMIT("{OP_LOAD, .dest=%d, .imm={.type=TYPE_%s, .as.%s=%s}}",
                 dest, ty, field, toks[3]); continue;
        }

        /* ── LOAD_NIL dest ── */
        if (strcmp(toks[0],"LOAD_NIL")==0) {
            if (n<2) asm_error(line_no,"LOAD_NIL needs dest");
            EMIT("{OP_LOAD_NIL, .dest=%d}", parse_reg(toks[1],line_no)); continue;
        }

        /* ── LOAD_BOOL dest true|false ── */
        if (strcmp(toks[0],"LOAD_BOOL")==0) {
            if (n<3) asm_error(line_no,"LOAD_BOOL needs dest true|false");
            int dest=parse_reg(toks[1],line_no);
            int bval = strcmp(toks[2],"true")==0 ? 1 : 0;
            EMIT("{OP_LOAD_BOOL, .dest=%d, .imm={.type=TYPE_BOOL,.as.boolean=%d}}", dest, bval); continue;
        }

        /* ── LOAD_CHAR dest codepoint ── */
        if (strcmp(toks[0],"LOAD_CHAR")==0) {
            if (n<3) asm_error(line_no,"LOAD_CHAR needs dest codepoint");
            int dest=parse_reg(toks[1],line_no);
            EMIT("{OP_LOAD_CHAR, .dest=%d, .imm={.type=TYPE_CHAR,.as.chr=%s}}", dest, toks[2]); continue;
        }

        /* ── LOAD_STR dest "literal" ── */
        if (strcmp(toks[0],"LOAD_STR")==0) {
            if (n<3) asm_error(line_no,"LOAD_STR needs dest \"string\"");
            int dest=parse_reg(toks[1],line_no);
            char raw[MAX_LINE]; parse_strlit(toks[2], raw, sizeof(raw), line_no);
            char esc[MAX_LINE*2]; c_escape(raw, esc, sizeof(esc));
            EMIT("{OP_LOAD_STR, .dest=%d, .imm={.type=TYPE_STR,.as.str=(ClueStr*)&(ClueStr){.data=\"%s\",.len=%zu,.refs=1}}}", dest, esc, strlen(raw)); continue;
        }

        /* ── Arithmetic: ADD SUB MUL DIV MOD  dest ra rb ── */
        { const char* arith[]={"ADD","SUB","MUL","DIV","MOD",NULL};
          const char* ops[]  ={"OP_ADD","OP_SUB","OP_MUL","OP_DIV","OP_MOD",NULL};
          for (int i=0; arith[i]; i++) {
            if (strcmp(toks[0],arith[i])==0) {
                if (n<4) asm_error(line_no,"needs dest ra rb");
                EMIT("{%s, .dest=%d, .a=%d, .b=%d}", ops[i],
                     parse_reg(toks[1],line_no), parse_reg(toks[2],line_no), parse_reg(toks[3],line_no));
                goto next;
            }
          }
        }

        /* ── Bitwise: AND OR XOR SHL SHR  dest ra rb ── */
        { const char* bw[]={"AND","OR","XOR","SHL","SHR",NULL};
          const char* ops[]={"OP_AND","OP_OR","OP_XOR","OP_SHL","OP_SHR",NULL};
          for (int i=0; bw[i]; i++) {
            if (strcmp(toks[0],bw[i])==0) {
                if (n<4) asm_error(line_no,"needs dest ra rb");
                EMIT("{%s, .dest=%d, .a=%d, .b=%d}", ops[i],
                     parse_reg(toks[1],line_no), parse_reg(toks[2],line_no), parse_reg(toks[3],line_no));
                goto next;
            }
          }
        }

        /* ── NOT LNOT IS_NIL STR_LEN  dest ra ── */
        { const char* un[]={"NOT","LNOT","IS_NIL","STR_LEN",NULL};
          const char* ops[]={"OP_NOT","OP_LNOT","OP_IS_NIL","OP_STR_LEN",NULL};
          for (int i=0; un[i]; i++) {
            if (strcmp(toks[0],un[i])==0) {
                if (n<3) asm_error(line_no,"needs dest src");
                EMIT("{%s, .dest=%d, .a=%d}", ops[i],
                     parse_reg(toks[1],line_no), parse_reg(toks[2],line_no));
                goto next;
            }
          }
        }

        /* ── Logical / Compare / STR_CAT  dest ra rb ── */
        { const char* tri[]={"LAND","LOR","CMP_EQ","CMP_NEQ","CMP_LT","CMP_GT","CMP_LE","CMP_GE","STR_CAT",NULL};
          const char* ops[]={"OP_LAND","OP_LOR","OP_CMP_EQ","OP_CMP_NEQ","OP_CMP_LT","OP_CMP_GT","OP_CMP_LE","OP_CMP_GE","OP_STR_CAT",NULL};
          for (int i=0; tri[i]; i++) {
            if (strcmp(toks[0],tri[i])==0) {
                if (n<4) asm_error(line_no,"needs dest ra rb");
                EMIT("{%s, .dest=%d, .a=%d, .b=%d}", ops[i],
                     parse_reg(toks[1],line_no), parse_reg(toks[2],line_no), parse_reg(toks[3],line_no));
                goto next;
            }
          }
        }

        fprintf(stderr,"[ClueASM] line %d: unknown instruction '%s'\n", line_no, toks[0]);
        exit(1);
        next:;
    }

    /* ── resolve CALL patches ── */
    for (size_t i=0; i<g_call_patch_count; i++) {
        size_t fi = find_func(g_call_patches[i].name);
        snprintf(g_instrs[g_call_patches[i].instr_index].text, 512,
                 "{OP_CALL, .imm={.type=TYPE_I32,.as.i32=%d}}", (int)fi);
    }

    /* ── resolve JUMP patches ── */
    for (size_t i=0; i<g_jump_patch_count; i++) {
        size_t target = find_label(g_jump_patches[i].name);
        size_t from   = g_jump_patches[i].instr_index;
        int    offset = (int)target - (int)from;
        /* re-emit with patched offset */
        char* t = g_instrs[from].text;
        /* patch the offset value in the text */
        char tmp[512]; strncpy(tmp, t, 512);
        char* p = strstr(tmp, "offset=0/*patch*/");
        if (p) { char after[256]; strcpy(after, p+17); sprintf(p,"offset=%d%s", offset, after); strcpy(t,tmp); }
    }

    /* ── auto-detect entry ── */
    if (!g_entry_set) {
        size_t after=0;
        for (size_t i=0; i<g_func_count; i++) {
            size_t ip=g_funcs[i].entry;
            while (ip<g_instr_count && strstr(g_instrs[ip].text,"OP_RET")==NULL) ip++;
            if (ip+1>after) after=ip+1;
        }
        g_entry_ip=after;
    }
}

/* ── emit the generated .c file ── */
static void emit_c(FILE* out, const char* src_name) {
    fprintf(out,
        "/* Generated by ClueASM from %s — do not edit */\n"
        "#include \"cluevm.c\"\n\n"
        "int main(void) {\n"
        "    ClueVM vm;\n"
        "    cluevm_init(&vm);\n\n"
        "    ClueFunc funcs[] = {\n", src_name);
    for (size_t i=0; i<g_func_count; i++)
        fprintf(out, "        {\"%s\", %zu},\n", g_funcs[i].name, g_funcs[i].entry);
    fprintf(out,
        "    };\n\n"
        "    Instruction program[] = {\n");
    for (size_t i=0; i<g_instr_count; i++)
        fprintf(out, "        /* %3zu */ %s,\n", i, g_instrs[i].text);
    fprintf(out,
        "    };\n\n"
        "    cluevm_run(&vm,\n"
        "               program, %zu,\n"
        "               funcs,   %zu,\n"
        "               %zu);\n"
        "    return 0;\n"
        "}\n",
        g_instr_count, g_func_count, g_entry_ip);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: clueasm <input.club> <output.c>\n");
        return 1;
    }
    FILE* in = fopen(argv[1], "r");
    if (!in) { fprintf(stderr, "clueasm: cannot open '%s'\n", argv[1]); return 1; }
    assemble(in);
    fclose(in);

    FILE* out = fopen(argv[2], "w");
    if (!out) { fprintf(stderr, "clueasm: cannot write '%s'\n", argv[2]); return 1; }
    emit_c(out, argv[1]);
    fclose(out);

    printf("clueasm: wrote %zu instructions to %s\n", g_instr_count, argv[2]);
    return 0;
}
