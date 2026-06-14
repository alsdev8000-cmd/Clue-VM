# Clue-VM
a register based Virtual Machine, with 16 registers and static typing while still obtaining simplicity.
Its Bytecode is JIT Compiled using MIR, here is [vnmakarov's repo](https://github.com/vnmakarov/mir).

`Anthropic Claude` Was used only for catching errors in my code.

# Syntax

**Comments**
```
# this is a comment
```

**Functions**
```
func funcname:
  # instructions here
  RET
end
```


**Labels (for jumps)**
```
mylabel:
  # instructions here
  JMP mylabel
```


**Main entry point**
```
main:
  # your program starts here
  HALT
```


**Registers**
```
RA RB RC RD RE RF RG RH   # general purpose
RI RJ RK RL RM RN         # function arguments
RO                         # stack pointer
RP                         # return value
```


**Loading values**
```
LOAD      RA  I32  42       # integer
LOAD      RA  F64  3.14     # float
LOAD_NIL  RA                # nil
LOAD_BOOL RA  true          # bool (true or false)
LOAD_CHAR RA  66            # char (unicode codepoint, 66 = 'B')
LOAD_STR  RA  "hello"       # string
```


**Moving between registers**
```
MOV  RA  RB      # RA = RB
```


**Arithmetic**
```
ADD  RC  RA  RB   # RC = RA + RB
SUB  RC  RA  RB   # RC = RA - RB
MUL  RC  RA  RB   # RC = RA * RB
DIV  RC  RA  RB   # RC = RA / RB
MOD  RC  RA  RB   # RC = RA % RB
```


**Bitwise**
```
AND  RC  RA  RB   # RC = RA & RB
OR   RC  RA  RB   # RC = RA | RB
XOR  RC  RA  RB   # RC = RA ^ RB
NOT  RB  RA       # RB = ~RA
SHL  RC  RA  RB   # RC = RA << RB
SHR  RC  RA  RB   # RC = RA >> RB
```

**Compare (result is Bool)**
```
CMP_EQ   RC  RA  RB   # RC = RA == RB
CMP_NEQ  RC  RA  RB   # RC = RA != RB
CMP_LT   RC  RA  RB   # RC = RA < RB
CMP_GT   RC  RA  RB   # RC = RA > RB
CMP_LE   RC  RA  RB   # RC = RA <= RB
CMP_GE   RC  RA  RB   # RC = RA >= RB
```

**Logical (Bool only)**
```
LAND  RC  RA  RB   # RC = RA && RB
LOR   RC  RA  RB   # RC = RA || RB
LNOT  RB  RA       # RB = !RA
```

**Nil check**
```
IS_NIL  RB  RA    # RB = (RA == nil)
```


**Strings**
```
STR_CAT  RC  RA  RB   # RC = RA + RB  (concatenate)
STR_LEN  RB  RA       # RB = length of RA
```

**Jumps**
```
JMP   mylabel        # jump always
JIF   RA  mylabel    # jump if RA == true
JNIF  RA  mylabel    # jump if RA == false
```


**Functions — calling**
```
# put args in RI, RJ, RK... before calling
MOV  RI  RA
MOV  RJ  RB
CALL  funcname   # return value lands in RP
```

**Print**
```
PRINT     RA   # print without newline
PRINT_LN  RA   # print with newline
```


**Full example**
```
func double:
  LOAD  RA  I32  2
  MUL   RP  RI  RA
  RET
end

main:
  LOAD  RI  I32  21
  CALL  double
  PRINT_LN  RP       # 42
  HALT
```
