-- mod-version:3
local syntax = require "core.syntax"

syntax.add {
  name = "club",
  files = { "%.club$" },
  comment = "#",

  patterns = {
    -- Comments
    { pattern = "#.*$", type = "comment" },

    -- String literals
    { pattern = { '"', '"', '\\' }, type = "string" },

    -- Numbers (hex, float, int)
    { pattern = "0x[%da-fA-F]+", type = "number" },
    { pattern = "%d+[%d%.]*",    type = "number" },

    -- Registers (RA-RP)
    { pattern = "R[A-P]%f[%s,\n]", type = "literal" },

    -- Labels (word followed by colon)
    { pattern = "[%a_][%w_]*%f[:]", type = "function" },

    -- Identifiers / opcodes / func names
    { pattern = "[%a_][%w_]*", type = "symbol" },
  },

  symbols = {
    -- Section keywords
    ["func"] = "keyword",
    ["end"]  = "keyword",
    ["main"] = "keyword",

    -- Opcodes — data
    ["MOV"]       = "keyword",
    ["LOAD"]      = "keyword",
    ["LOAD_NIL"]  = "keyword",
    ["LOAD_BOOL"] = "keyword",
    ["LOAD_CHAR"] = "keyword",
    ["LOAD_STR"]  = "keyword",

    -- Opcodes — arithmetic
    ["ADD"] = "keyword",
    ["SUB"] = "keyword",
    ["MUL"] = "keyword",
    ["DIV"] = "keyword",
    ["MOD"] = "keyword",

    -- Opcodes — bitwise
    ["AND"] = "keyword",
    ["OR"]  = "keyword",
    ["XOR"] = "keyword",
    ["NOT"] = "keyword",
    ["SHL"] = "keyword",
    ["SHR"] = "keyword",

    -- Opcodes — logical
    ["LAND"] = "keyword",
    ["LOR"]  = "keyword",
    ["LNOT"] = "keyword",

    -- Opcodes — compare
    ["CMP_EQ"]  = "keyword",
    ["CMP_NEQ"] = "keyword",
    ["CMP_LT"]  = "keyword",
    ["CMP_GT"]  = "keyword",
    ["CMP_LE"]  = "keyword",
    ["CMP_GE"]  = "keyword",

    -- Opcodes — nil / string
    ["IS_NIL"]  = "keyword",
    ["STR_CAT"] = "keyword",
    ["STR_LEN"] = "keyword",

    -- Opcodes — jump
    ["JMP"]  = "keyword",
    ["JIF"]  = "keyword",
    ["JNIF"] = "keyword",

    -- Opcodes — call/return
    ["CALL"] = "keyword",
    ["RET"]  = "keyword",

    -- Opcodes — i/o
    ["PRINT"]    = "keyword",
    ["PRINT_LN"] = "keyword",

    ["HALT"] = "keyword",

    -- Types
    ["I8"]    = "keyword2",
    ["I16"]   = "keyword2",
    ["I32"]   = "keyword2",
    ["I64"]   = "keyword2",
    ["F32"]   = "keyword2",
    ["F64"]   = "keyword2",

    -- Literals
    ["true"]  = "literal",
    ["false"] = "literal",
    ["nil"]   = "literal",
  },
}
