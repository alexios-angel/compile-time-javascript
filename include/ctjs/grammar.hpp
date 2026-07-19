#ifndef CTJS__GRAMMAR__HPP
#define CTJS__GRAMMAR__HPP

#include "../ctlark.hpp"

// The grammar layer: the ctjs JavaScript subset written in lark's
// grammar language and parsed by ctlark at compile time. Unlike the
// document-format siblings this is a full token-level programming
// language grammar: a precedence LADDER of ?-inlined rules encodes
// binding strength (assignment > ternary > ?? > || > && > equality >
// relational > additive > multiplicative > ** > unary > postfix), so
// precedence never needs a separate folding pass - the tree IS the
// expression structure.
//
// Operator FAMILIES are single terminals (EQ_OP, REL_OP, MUL_OP,
// ASSIGN_OP, INCDEC) rather than one alternative per operator - the
// lexer's longest-match rule keeps them apart ("=" vs "==" vs "===")
// and the lowering reads the matched text; this keeps the Earley
// tables (built once, in the PCH) a fraction of the naive size.
// Keywords are plain string literals: contextual lexing plus
// longest-match keeps "let" a keyword exactly where a keyword can
// appear while "letter" stays a NAME.
//
// Assignment targets are a dedicated lhs rule (NAME, member, index),
// so "f() = 1" is a SYNTAX error, not a runtime surprise. Semicolons
// are required (no ASI, by design). No regex literals, template
// literals, classes, this, or new in v0.1 - see the README.

// ctjs is C++20-only (the runtime layer leans on it), but the source
// NTTP mechanism is the family's usual one
#if CTLL_CNTTP_COMPILER_CHECK
#define CTJS_STRING_INPUT ctll::fixed_string
#else
#define CTJS_STRING_INPUT const auto &
#endif

namespace ctjs::detail {

inline constexpr ctll::fixed_string js_grammar = R"x(
start: stmt*

?stmt: block
     | var_stmt
     | fn_decl
     | if_stmt
     | while_stmt
     | do_stmt
     | for_stmt
     | forof_stmt
     | return_stmt
     | break_stmt
     | continue_stmt
     | throw_stmt
     | try_stmt
     | switch_stmt
     | empty_stmt
     | expr_stmt

block: "{" stmt* "}"
var_stmt: "let" declarator ("," declarator)* ";"   -> let_stmt
        | "const" declarator ("," declarator)* ";" -> const_stmt
        | "var" declarator ("," declarator)* ";"   -> varkw_stmt
declarator: NAME ["=" assign]
fn_decl: "function" NAME "(" params ")" block
params: [NAME ("," NAME)*]
if_stmt: "if" "(" expr ")" stmt ["else" stmt]
while_stmt: "while" "(" expr ")" stmt
do_stmt: "do" stmt "while" "(" expr ")" ";"
for_stmt: "for" "(" for_init ";" for_cond ";" for_step ")" stmt
for_init: "let" declarator ("," declarator)*   -> forinit_let
        | "const" declarator ("," declarator)* -> forinit_const
        | "var" declarator ("," declarator)*   -> forinit_var
        | [expr]
for_cond: [expr]
for_step: [expr]
forof_stmt: "for" "(" "let" NAME "of" expr ")" stmt   -> forof_let
          | "for" "(" "const" NAME "of" expr ")" stmt -> forof_const
          | "for" "(" "var" NAME "of" expr ")" stmt   -> forof_var
return_stmt: "return" [expr] ";"
break_stmt: "break" ";"
continue_stmt: "continue" ";"
throw_stmt: "throw" expr ";"
try_stmt: "try" block "catch" "(" NAME ")" block                  -> try_catch
        | "try" block "catch" "(" NAME ")" block "finally" block  -> try_catch_fin
        | "try" block "finally" block                             -> try_fin
switch_stmt: "switch" "(" expr ")" "{" switch_clause* "}"
switch_clause: "case" expr ":" stmt*  -> case_clause
             | "default" ":" stmt*     -> default_clause
empty_stmt: ";"
expr_stmt: expr ";"

?expr: assign
     | expr "," assign -> comma_op

?assign: nullish
       | nullish "?" assign ":" assign -> ternary
       | lhs ASSIGN_OP assign          -> assign_op

lhs: NAME                    -> lhs_name
   | postfix "." NAME        -> lhs_member
   | postfix "[" expr "]"    -> lhs_index

?nullish: oror
        | nullish "??" oror -> nullish_op
?oror: andand
     | oror "||" andand -> or_op
?andand: equality
       | andand "&&" equality -> and_op
?equality: relational
         | equality EQ_OP relational -> cmp_eq
?relational: additive
           | relational REL_OP additive -> cmp_rel
           | relational "in" additive   -> in_op
?additive: multiplicative
         | additive "+" multiplicative -> add_op
         | additive "-" multiplicative -> sub_op
?multiplicative: exponent
               | multiplicative MUL_OP exponent -> mul_op
?exponent: unary
         | unary "**" exponent -> pow_op
?unary: postfix
      | "!" unary      -> not_op
      | "-" unary      -> neg_op
      | "+" unary      -> pos_op
      | "typeof" unary -> typeof_op
      | "delete" unary -> delete_op
      | INCDEC lhs     -> pre_incdec
?postfix: primary
        | postfix "(" args ")"   -> call
        | postfix "." NAME       -> member
        | postfix "[" expr "]"   -> index
        | lhs INCDEC             -> post_incdec
args: [assign ("," assign)*]
?primary: NAME
        | NUMBER
        | DQSTRING
        | SQSTRING
        | "true"      -> true_lit
        | "false"     -> false_lit
        | "null"      -> null_lit
        | array_lit
        | object_lit
        | fn_expr
        | arrow_fn
        | "(" expr ")" -> paren
array_lit: "[" [assign ("," assign)*] "]"
object_lit: "{" [prop ("," prop)*] "}"
prop: NAME ":" assign     -> prop_name
    | DQSTRING ":" assign -> prop_str
    | SQSTRING ":" assign -> prop_str2
fn_expr: "function" "(" params ")" block
arrow_fn: "(" params ")" "=>" arrow_body
        | NAME "=>" arrow_body
?arrow_body: block | assign

NAME: /[A-Za-z_$][A-Za-z0-9_$]*/
NUMBER: /0[xX][0-9a-fA-F]+|(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+\-]?[0-9]+)?|\.[0-9]+/
DQSTRING: /"([^"\\\x0a]|\\[\s\S])*"/
SQSTRING: /'([^'\\\x0a]|\\[\s\S])*'/
ASSIGN_OP: /(\*\*|[+\-*\/%])?=/
EQ_OP: /[=!]==?/
REL_OP: /[<>]=?/
MUL_OP: /[*\/%]/
INCDEC: /\+\+|--/

%ignore /[ \x09\x0a\x0d]+/
%ignore /\/\/[^\x0a]*/
%ignore /\/\*([^*]|\*+[^*\/])*\*+\//
)x";

inline constexpr ctll::fixed_string js_start = "start";

static_assert(ctlark::grammar_valid<js_grammar>,
              "ctjs: internal error - the JavaScript grammar failed to compile");

} // namespace ctjs::detail

#endif
