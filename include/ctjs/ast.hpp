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
struct op_await { }; // settled-promise unwrap (the engine has no pending state)
struct op_none { }; // plain `=` in assignments

// --- expressions

template <typename Text> struct ident { };
template <typename Text> struct num_lit { };            // raw spelling
template <typename Text> struct str_lit { };            // raw, quotes included
// a number computed AT COMPILE TIME by the constant folder (fold.hpp),
// carried in the type itself; the runtime just loads it. true/false/
// null folds reuse the literal nodes above.
template <double V> struct const_num { };
struct true_lit { }; struct false_lit { }; struct null_lit { };
template <typename... Elems> struct array_lit { };
template <typename KeyText, typename V> struct prop { }; // key already unquoted
template <typename K, typename V> struct computed_prop { }; // { [expr]: v }
template <char Kind, typename Name, typename Params, typename Body>
struct accessor_prop { }; // { get x() {} / set x(v) {} }
template <typename E> struct spread_prop { };            // { ...expr }
template <typename... Props> struct object_lit { };
template <typename Op, typename L, typename R> struct binary { };
template <typename Op, typename E> struct unary { };
template <typename C, typename T, typename F> struct ternary { };
template <typename L, typename R> struct comma_op { };
template <typename L, typename R> struct in_op { };
template <typename L, typename R> struct instanceof_op { };
template <typename T> struct delete_op { };
template <typename E> struct yield_op { }; // E = void for bare `yield`
template <typename Text> struct regex_lit { }; // raw spelling, slashes included
struct this_lit { };  // `this` — resolves to the call receiver
struct super_lit { }; // `super` — only valid as super(...) / super.x
template <typename Fn, typename... Args> struct call { };
template <typename Obj, typename NameText> struct member { };
template <typename Obj, typename Index> struct index { };
// optional chaining: nullish receiver/callee short-circuits PER LINK
// (a?.b.c still throws when a?.b is undefined - write a?.b?.c)
template <typename Obj, typename NameText> struct opt_member { };
template <typename Obj, typename Index> struct opt_index { };
template <typename Fn, typename... Args> struct opt_call { };
// Target is ident/member/index; Op is op_none for `=`, else the
// compound operator (+= etc.)
template <typename Op, typename Target, typename V> struct assign { };
template <typename Target, bool Pre, bool Inc> struct incdec { };
// params: plain name texts, param_default<N, E>, param_rest<N> mixed
template <typename N, typename Default> struct param_default { };
template <typename N> struct param_rest { };
template <typename... Params> struct plist { };
template <typename E> struct spread_arg { };
template <typename Init, typename... Names> struct destr_array { };
template <typename Key, typename Bind> struct dprop { };
template <typename Init, typename... Props> struct destr_object { };
// Body: block for function/arrow-with-block, else the expression of
// an expression-bodied arrow. IsAsync functions wrap their return
// value in a resolved promise (settled-promise subset: bodies run
// synchronously, so the promise is settled by the time callers see
// it). IsGen functions are EAGER generators: the body runs to
// completion on the call, yields buffer up, and the caller gets an
// iterator object draining the buffer
template <typename Params, typename Body, bool ExprBody, bool IsAsync = false,
          bool IsGen = false>
struct fn_expr { };

// --- statements

template <typename... Stmts> struct program { };
template <typename... Stmts> struct block { };
template <typename NameText, typename Init> struct declarator { }; // Init = void when absent
template <typename... Decls> struct let_stmt { };
template <typename... Decls> struct const_stmt { };
template <typename... Decls> struct var_stmt { };
template <typename E> struct expr_stmt { };
template <typename NameText, typename Params, typename Body, bool IsAsync = false,
          bool IsGen = false>
struct fn_decl { };
template <typename C, typename Then, typename Else> struct if_stmt { }; // Else = void
template <typename C, typename Body> struct while_stmt { };
template <typename Body, typename C> struct do_stmt { };
// any slot may be void; Init is a let/const/var_stmt-shaped node or an
// expression
template <typename Init, typename Cond, typename Step, typename Body> struct for_stmt { };
template <typename NameText, typename Iter, typename Body> struct forof_stmt { };
template <typename E> struct return_stmt { }; // E = void for bare return
struct break_stmt { }; struct continue_stmt { };
template <typename LabelText> struct break_label { };
template <typename LabelText> struct continue_label { };
template <typename LabelText, typename S> struct labeled_stmt { };
template <typename E> struct throw_stmt { };
// CatchName/Handler = void for try/finally; Finally = void when absent
template <typename Body, typename CatchName, typename Handler, typename Finally>
struct try_stmt { };
template <typename Callee, typename... Args> struct new_op { };
template <typename Text> struct tpl_text { };
template <typename... Parts> struct template_lit { };
template <typename Name, typename Params, typename Body> struct class_method { };
// Kind: 'g' getter, 's' setter (validated at lowering from the NAME
// NAME(...) accessor shape - get/set stay ordinary identifiers)
template <char Kind, typename Name, typename Params, typename Body>
struct class_accessor { };
template <typename KeyExpr, typename Params, typename Body> struct class_computed_method { };
template <typename Name, typename Init> struct class_field { };     // Init = void when absent
template <typename KeyExpr, typename Init> struct class_computed_field { };
template <typename Inner> struct static_member { };                 // `static` prefix
template <typename Name, typename... Members> struct class_decl { };
template <typename Name, typename SuperName, typename... Members> struct class_ext { };
template <typename E, typename... Ss> struct case_clause { };
template <typename... Ss> struct default_clause { };
template <typename D, typename... Clauses> struct switch_stmt { };
struct empty_stmt { };

} // namespace ctjs::ast

#endif
