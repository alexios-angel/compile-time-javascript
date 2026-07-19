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
// are required (no ASI, by design). Reserved words lex as IDENT-
// excluding keywords, so `let let = 1` is a syntax error while
// `p.catch(f)` (keyword as a property name) is fine - see the README.

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
     | class_decl
     | labeled_stmt
     | empty_stmt
     | expr_stmt

block: "{" stmt* "}"
var_stmt: "let" declarator ("," declarator)* ";"   -> let_stmt
        | "const" declarator ("," declarator)* ";" -> const_stmt
        | "var" declarator ("," declarator)* ";"   -> varkw_stmt
declarator: IDENT ["=" assign]
          | "[" [IDENT ("," IDENT)*] "]" "=" assign     -> destr_array
          | "{" [dprop ("," dprop)*] "}" "=" assign   -> destr_object
dprop: NAME            -> dprop_shorthand
     | NAME ":" IDENT   -> dprop_renamed
fn_decl: "function" IDENT "(" params ")" block
       | "async" "function" IDENT "(" params ")" block -> async_fn_decl
       | "function" "*" IDENT "(" params ")" block     -> gen_fn_decl
params: [param ("," param)*]
param: IDENT               -> param_plain
     | IDENT "=" assign    -> param_default
     | "..." IDENT         -> param_rest
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
forof_stmt: "for" "(" "let" IDENT "of" expr ")" stmt   -> forof_let
          | "for" "(" "const" IDENT "of" expr ")" stmt -> forof_const
          | "for" "(" "var" IDENT "of" expr ")" stmt   -> forof_var
return_stmt: "return" [expr] ";"
break_stmt: "break" ";"
          | "break" IDENT ";" -> break_label
continue_stmt: "continue" ";"
             | "continue" IDENT ";" -> continue_label
throw_stmt: "throw" expr ";"
try_stmt: "try" block "catch" "(" IDENT ")" block                  -> try_catch
        | "try" block "catch" "(" IDENT ")" block "finally" block  -> try_catch_fin
        | "try" block "finally" block                             -> try_fin
class_decl: "class" IDENT "{" class_member* "}"
          | "class" IDENT "extends" IDENT "{" class_member* "}" -> class_extends
class_member: NAME "(" params ")" block                      -> class_method
            | NAME NAME "(" params ")" block                   -> class_accessor
            | "[" expr "]" "(" params ")" block                -> class_computed_method
            | NAME ["=" assign] ";"                            -> class_field
            | "[" expr "]" "=" assign ";"                      -> class_computed_field
            | "static" NAME "(" params ")" block               -> static_method
            | "static" NAME NAME "(" params ")" block          -> static_accessor
            | "static" "[" expr "]" "(" params ")" block       -> static_computed_method
            | "static" NAME ["=" assign] ";"                   -> static_field
            | "static" "[" expr "]" "=" assign ";"             -> static_computed_field
switch_stmt: "switch" "(" expr ")" "{" switch_clause* "}"
switch_clause: "case" expr ":" stmt*  -> case_clause
             | "default" ":" stmt*     -> default_clause
labeled_stmt: IDENT ":" stmt
empty_stmt: ";"
expr_stmt: expr ";"

?expr: assign
     | expr "," assign -> comma_op

?assign: nullish
       | nullish "?" assign ":" assign -> ternary
       | lhs ASSIGN_OP assign          -> assign_op
       | "yield" assign                -> yield_op
       | "yield"                       -> yield_bare

lhs: IDENT                   -> lhs_name
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
           | relational "instanceof" additive -> instanceof_op
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
      | "await" unary  -> await_op
      | INCDEC lhs     -> pre_incdec
?postfix: primary
        | "new" newable "(" args ")" -> new_op
        | postfix "(" args ")"   -> call
        | postfix "." NAME       -> member
        | postfix "[" expr "]"   -> index
        | postfix "?." NAME            -> opt_member
        | postfix "?." "[" expr "]"    -> opt_index
        | postfix "?." "(" args ")"    -> opt_call
        | lhs INCDEC             -> post_incdec
?newable: primary
        | newable "." NAME       -> member
        | newable "[" expr "]"   -> index
args: [arg ("," arg)*]
?arg: assign
    | "..." assign -> spread_arg
?primary: IDENT
        | "this"      -> this_lit
        | "super"     -> super_lit
        | NUMBER
        | DQSTRING
        | SQSTRING
        | "true"      -> true_lit
        | "false"     -> false_lit
        | "null"      -> null_lit
        | template_lit
        | REGEX -> regex_lit
        | array_lit
        | object_lit
        | fn_expr
        | arrow_fn
        | "(" expr ")" -> paren
template_lit: TEMPLATE_FULL
            | TEMPLATE_HEAD assign (TEMPLATE_MID assign)* TEMPLATE_TAIL
array_lit: "[" [arg ("," arg)*] "]"
object_lit: "{" [prop ("," prop)*] "}"
prop: NAME ":" assign     -> prop_name
    | DQSTRING ":" assign -> prop_str
    | SQSTRING ":" assign -> prop_str2
    | NAME "(" params ")" block -> prop_method
    | NAME NAME "(" params ")" block -> prop_accessor
    | "[" expr "]" ":" assign -> prop_computed
    | "..." assign        -> prop_spread
    | NAME                -> prop_shorthand
fn_expr: "function" "(" params ")" block
       | "async" "function" "(" params ")" block -> async_fn_expr
       | "function" "*" "(" params ")" block     -> gen_fn_expr
arrow_fn: "(" params ")" "=>" arrow_body
        | IDENT "=>" arrow_body
?arrow_body: block | assign

NAME: /[A-Za-z_$][A-Za-z0-9_$]*/
IDENT: /(?!(?:await|break|case|catch|class|const|continue|default|delete|do|else|extends|false|finally|for|function|if|in|instanceof|let|new|null|return|super|switch|this|throw|true|try|typeof|var|void|while|with|yield)(?![A-Za-z0-9_$]))[A-Za-z_$][A-Za-z0-9_$]*/
NUMBER: /0[xX][0-9a-fA-F]+|(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+\-]?[0-9]+)?|\.[0-9]+/
DQSTRING: /"([^"\\\x0a]|\\[\s\S])*"/
TEMPLATE_FULL: /`([^`\\$]|\\[\s\S]|\$(?!\{))*`/
TEMPLATE_HEAD: /`([^`\\$]|\\[\s\S]|\$(?!\{))*\$\{/
TEMPLATE_MID: /\}([^`\\$]|\\[\s\S]|\$(?!\{))*\$\{/
TEMPLATE_TAIL: /\}([^`\\$]|\\[\s\S]|\$(?!\{))*`/
SQSTRING: /'([^'\\\x0a]|\\[\s\S])*'/
REGEX: /\/(?![*\/=\x20])([^\/\\\x0a\x20;,\[]|\\[^\x0a]|\[([^\]\\\x0a]|\\[^\x0a])*\])+\/[a-z]*/
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
