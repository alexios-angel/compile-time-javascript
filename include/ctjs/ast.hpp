#ifndef CTJS__AST__HPP
#define CTJS__AST__HPP

#include "../ctlark.hpp"

// The type-level AST. Parsing happens at compile time, so a whole
// script is ONE TYPE built from the empty structs below; identifier
// and literal spellings ride along as ctlark::text<...> type
// parameters (cooked - escapes, number parsing - lazily at runtime,
// once per instantiation). `void` fills absent optional slots.
//
// The runtime interpreter (interp.hpp) is specialized over these
// nodes, so the C++ compiler emits code specialized for each script -
// there is no generic AST walk at runtime, the walk IS the emitted
// program.

namespace ctjs::ast {

// --- operator tags

struct op_add { }; struct op_sub { }; struct op_mul { }; struct op_div { };
struct op_mod { }; struct op_pow { };
struct op_eq { }; struct op_ne { }; struct op_seq { }; struct op_sne { };
struct op_lt { }; struct op_gt { }; struct op_le { }; struct op_ge { };
struct op_and { }; struct op_or { }; struct op_nullish { };
struct op_not { }; struct op_neg { }; struct op_pos { }; struct op_typeof { };
struct op_none { }; // plain `=` in assignments

// --- expressions

template <typename Text> struct ident { };
template <typename Text> struct num_lit { };            // raw spelling
template <typename Text> struct str_lit { };            // raw, quotes included
struct true_lit { }; struct false_lit { }; struct null_lit { };
template <typename... Elems> struct array_lit { };
template <typename KeyText, typename V> struct prop { }; // key already unquoted
template <typename... Props> struct object_lit { };
template <typename Op, typename L, typename R> struct binary { };
template <typename Op, typename E> struct unary { };
template <typename C, typename T, typename F> struct ternary { };
template <typename L, typename R> struct comma_op { };
template <typename L, typename R> struct in_op { };
template <typename T> struct delete_op { };
template <typename Fn, typename... Args> struct call { };
template <typename Obj, typename NameText> struct member { };
template <typename Obj, typename Index> struct index { };
// Target is ident/member/index; Op is op_none for `=`, else the
// compound operator (+= etc.)
template <typename Op, typename Target, typename V> struct assign { };
template <typename Target, bool Pre, bool Inc> struct incdec { };
template <typename... NameTexts> struct plist { };
// Body: block for function/arrow-with-block, else the expression of
// an expression-bodied arrow
template <typename Params, typename Body, bool ExprBody> struct fn_expr { };

// --- statements

template <typename... Stmts> struct program { };
template <typename... Stmts> struct block { };
template <typename NameText, typename Init> struct declarator { }; // Init = void when absent
template <typename... Decls> struct let_stmt { };
template <typename... Decls> struct const_stmt { };
template <typename... Decls> struct var_stmt { };
template <typename E> struct expr_stmt { };
template <typename NameText, typename Params, typename Body> struct fn_decl { };
template <typename C, typename Then, typename Else> struct if_stmt { }; // Else = void
template <typename C, typename Body> struct while_stmt { };
template <typename Body, typename C> struct do_stmt { };
// any slot may be void; Init is a let/const/var_stmt-shaped node or an
// expression
template <typename Init, typename Cond, typename Step, typename Body> struct for_stmt { };
template <typename NameText, typename Iter, typename Body> struct forof_stmt { };
template <typename E> struct return_stmt { }; // E = void for bare return
struct break_stmt { }; struct continue_stmt { };
template <typename E> struct throw_stmt { };
// CatchName/Handler = void for try/finally; Finally = void when absent
template <typename Body, typename CatchName, typename Handler, typename Finally>
struct try_stmt { };
template <typename Callee, typename... Args> struct new_op { };
template <typename Name, typename Params, typename Body> struct class_method { };
template <typename Name, typename... Methods> struct class_decl { };
template <typename E, typename... Ss> struct case_clause { };
template <typename... Ss> struct default_clause { };
template <typename D, typename... Clauses> struct switch_stmt { };
struct empty_stmt { };

} // namespace ctjs::ast

#endif
