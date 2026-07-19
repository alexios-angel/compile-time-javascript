#ifndef CTJS__LOWER__HPP
#define CTJS__LOWER__HPP

#include "grammar.hpp"
#include "ast.hpp"
#ifndef CTJS_IN_A_MODULE
#include <cstddef>
#include <string_view>
#include <type_traits>
#endif

// Lowering the ctlark parse tree (a type) into the ctjs AST (another
// type), entirely at compile time. Dispatch is by RULE NAME: every
// tree node matches one generic specialization whose if-constexpr
// chain compares the node's name string, so the lowering reads like
// the grammar. Operator-family tokens (ASSIGN_OP, EQ_OP, ...) pick
// their ast:: operator tag from the matched text.

namespace ctjs::detail {

template <size_t I, typename... Ts> struct nth_s {
	using type = void;
};
template <typename T0, typename... Ts> struct nth_s<0, T0, Ts...> {
	using type = T0;
};
template <size_t I, typename T0, typename... Ts> struct nth_s<I, T0, Ts...>
    : nth_s<I - 1, Ts...> { };
template <size_t I, typename... Ts> using nth_t = typename nth_s<I, Ts...>::type;

template <typename Node> struct lower_expr;
template <typename Node> struct lower_stmt;
template <typename... Ks2> struct lower_new;
template <typename K> struct tpl_part;
template <typename... Ks2> struct lower_class;
template <typename Tree> struct lower_method;
template <typename Node> using lower_expr_t = typename lower_expr<Node>::type;
template <typename Node> using lower_stmt_t = typename lower_stmt<Node>::type;

// --- operator tokens -> ast tags

template <typename Tok> struct assign_tag {
	static constexpr auto pick() {
		constexpr std::string_view v = Tok::value_type::view();
		if constexpr (v == std::string_view{"="}) { return ast::op_none{}; }
		else if constexpr (v == std::string_view{"+="}) { return ast::op_add{}; }
		else if constexpr (v == std::string_view{"-="}) { return ast::op_sub{}; }
		else if constexpr (v == std::string_view{"*="}) { return ast::op_mul{}; }
		else if constexpr (v == std::string_view{"/="}) { return ast::op_div{}; }
		else if constexpr (v == std::string_view{"%="}) { return ast::op_mod{}; }
		else { return ast::op_pow{}; } // **=
	}
	using type = decltype(pick());
};

template <typename Tok> struct eq_tag {
	static constexpr auto pick() {
		constexpr std::string_view v = Tok::value_type::view();
		if constexpr (v == std::string_view{"=="}) { return ast::op_eq{}; }
		else if constexpr (v == std::string_view{"!="}) { return ast::op_ne{}; }
		else if constexpr (v == std::string_view{"==="}) { return ast::op_seq{}; }
		else { return ast::op_sne{}; } // !==
	}
	using type = decltype(pick());
};

template <typename Tok> struct rel_tag {
	static constexpr auto pick() {
		constexpr std::string_view v = Tok::value_type::view();
		if constexpr (v == std::string_view{"<"}) { return ast::op_lt{}; }
		else if constexpr (v == std::string_view{">"}) { return ast::op_gt{}; }
		else if constexpr (v == std::string_view{"<="}) { return ast::op_le{}; }
		else { return ast::op_ge{}; } // >=
	}
	using type = decltype(pick());
};

template <typename Tok> struct mul_tag {
	static constexpr auto pick() {
		constexpr std::string_view v = Tok::value_type::view();
		if constexpr (v == std::string_view{"*"}) { return ast::op_mul{}; }
		else if constexpr (v == std::string_view{"/"}) { return ast::op_div{}; }
		else { return ast::op_mod{}; } // %
	}
	using type = decltype(pick());
};

template <typename Tok> constexpr bool is_increment() {
	return Tok::value_type::view() == std::string_view{"++"};
}

// --- pack helpers (props, args, params, declarators, blocks)

template <typename PropTree> struct lower_prop;
template <typename PN, typename K, typename V> struct lower_prop<ctlark::tree<PN, K, V>> {
	static constexpr auto pick() {
		if constexpr (PN::view() == std::string_view{"prop_name"}) {
			return ast::prop<ast::ident<typename K::value_type>, lower_expr_t<V>>{};
		} else {
			return ast::prop<ast::str_lit<typename K::value_type>, lower_expr_t<V>>{};
		}
	}
	using type = decltype(pick());
};

template <typename K> struct lower_arg; // spread-aware (defined below)
template <typename Callee, typename ArgsTree> struct lower_call;
template <typename Callee, typename AN, typename... As>
struct lower_call<Callee, ctlark::tree<AN, As...>> {
	using type = ast::call<Callee, typename lower_arg<As>::type...>;
};

template <typename P> struct lower_param;
template <typename PN2, typename Name> struct lower_param<ctlark::tree<PN2, Name>> {
	static constexpr auto pick() {
		if constexpr (PN2::view() == std::string_view{"param_rest"}) {
			return ast::param_rest<typename Name::value_type>{};
		} else {
			return typename Name::value_type{}; // plain: bare name text
		}
	}
	using type = decltype(pick());
};
template <typename PN2, typename Name, typename Def>
struct lower_param<ctlark::tree<PN2, Name, Def>> {
	using type = ast::param_default<typename Name::value_type, lower_expr_t<Def>>;
};

template <typename ParamsTree> struct lower_params;
template <typename PN, typename... Ps> struct lower_params<ctlark::tree<PN, Ps...>> {
	using type = ast::plist<typename lower_param<Ps>::type...>;
};

template <typename DP> struct lower_dprop;
template <typename DPN, typename K> struct lower_dprop<ctlark::tree<DPN, K>> {
	using type = ast::dprop<typename K::value_type, typename K::value_type>;
};
template <typename DPN, typename K, typename B> struct lower_dprop<ctlark::tree<DPN, K, B>> {
	using type = ast::dprop<typename K::value_type, typename B::value_type>;
};

template <typename DeclTree> struct lower_decl;
template <typename DN, typename Name> struct lower_decl<ctlark::tree<DN, Name>> {
	using type = ast::declarator<typename Name::value_type, void>;
};
template <typename DN, typename Name, typename Init>
struct lower_decl<ctlark::tree<DN, Name, Init>> {
	static constexpr auto pick() {
		if constexpr (DN::view() == std::string_view{"destr_array"}) {
			return ast::destr_array<lower_expr_t<Init>, typename Name::value_type>{};
		} else if constexpr (DN::view() == std::string_view{"destr_object"}) {
			return ast::destr_object<lower_expr_t<Init>, typename lower_dprop<Name>::type>{};
		} else {
			return ast::declarator<typename Name::value_type, lower_expr_t<Init>>{};
		}
	}
	using type = decltype(pick());
};
// destructuring with 2+ names: [a, b, c] = init / {x, y: z} = init -
// the LAST kid is the initializer, the rest are names/dprops
template <typename... Ts> struct kpack { };
template <typename Acc, typename... Ks2> struct split_last;
template <typename... As, typename K> struct split_last<kpack<As...>, K> {
	using front = kpack<As...>;
	using last = K;
};
template <typename... As, typename K, typename... Rest>
struct split_last<kpack<As...>, K, Rest...> : split_last<kpack<As..., K>, Rest...> { };

template <typename Front, typename Init> struct build_destr_array;
template <typename... Names, typename Init> struct build_destr_array<kpack<Names...>, Init> {
	using type = ast::destr_array<lower_expr_t<Init>, typename Names::value_type...>;
};
template <typename Front, typename Init> struct build_destr_object;
template <typename... Ps, typename Init> struct build_destr_object<kpack<Ps...>, Init> {
	using type = ast::destr_object<lower_expr_t<Init>, typename lower_dprop<Ps>::type...>;
};

template <typename DN, typename K1, typename K2, typename K3, typename... Rest>
struct lower_decl<ctlark::tree<DN, K1, K2, K3, Rest...>> {
	using parts = split_last<kpack<>, K1, K2, K3, Rest...>;
	static constexpr auto pick() {
		if constexpr (DN::view() == std::string_view{"destr_array"}) {
			return typename build_destr_array<typename parts::front,
			                                  typename parts::last>::type{};
		} else {
			return typename build_destr_object<typename parts::front,
			                                   typename parts::last>::type{};
		}
	}
	using type = decltype(pick());
};

template <typename BlockTree> struct lower_block;
template <typename BN, typename... Ss> struct lower_block<ctlark::tree<BN, Ss...>> {
	using type = ast::block<lower_stmt_t<Ss>...>;
};

// for-slot wrappers: for_init / for_cond / for_step lower to void, an
// init statement, or a bare expression statement
template <typename Tree> struct lower_for_init;
template <typename N, typename... Ks> struct lower_for_init<ctlark::tree<N, Ks...>> {
	static constexpr auto pick() {
		constexpr std::string_view n = N::view();
		if constexpr (n == std::string_view{"forinit_let"}) {
			return ast::let_stmt<typename lower_decl<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"forinit_const"}) {
			return ast::const_stmt<typename lower_decl<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"forinit_var"}) {
			return ast::var_stmt<typename lower_decl<Ks>::type...>{};
		} else if constexpr (sizeof...(Ks) == 0) {
			return std::type_identity<void>{};
		} else {
			return ast::expr_stmt<lower_expr_t<nth_t<0, Ks...>>>{};
		}
	}
	static constexpr auto unwrap() {
		if constexpr (std::is_same_v<decltype(pick()), std::type_identity<void>>) {
			return std::type_identity<void>{};
		} else {
			return std::type_identity<decltype(pick())>{};
		}
	}
	using type = typename decltype(unwrap())::type;
};

template <typename Tree> struct lower_for_expr; // for_cond / for_step
template <typename N> struct lower_for_expr<ctlark::tree<N>> {
	using type = void;
};
template <typename N, typename E> struct lower_for_expr<ctlark::tree<N, E>> {
	using type = lower_expr_t<E>;
};

// arrow functions: params are a params tree or a single NAME token
template <typename Head> struct arrow_params {
	using type = typename lower_params<Head>::type; // params tree
};
template <typename TN, typename TV> struct arrow_params<ctlark::token<TN, TV>> {
	using type = ast::plist<TV>;
};
template <typename T> struct is_block_tree : std::false_type { };
template <typename N, typename... Ks> struct is_block_tree<ctlark::tree<N, Ks...>> {
	static constexpr bool value = N::view() == std::string_view{"block"};
};

// --- expressions

// tokens: NAME / NUMBER / DQSTRING / SQSTRING
template <typename TN, typename TV> struct lower_expr<ctlark::token<TN, TV>> {
	static constexpr auto pick() {
		constexpr std::string_view n = TN::view();
		if constexpr (n == std::string_view{"NAME"}) { return ast::ident<TV>{}; }
		else if constexpr (n == std::string_view{"NUMBER"}) { return ast::num_lit<TV>{}; }
		else { return ast::str_lit<TV>{}; }
	}
	using type = decltype(pick());
};

template <typename TN, typename... Ks> struct lower_expr<ctlark::tree<TN, Ks...>> {
	template <size_t I> using kid = nth_t<I, Ks...>;
	static constexpr auto pick() {
		constexpr std::string_view n = TN::view();
		if constexpr (n == std::string_view{"paren"}) {
			return typename lower_expr<kid<0>>::type{};
		} else if constexpr (n == std::string_view{"true_lit"}) {
			return ast::true_lit{};
		} else if constexpr (n == std::string_view{"false_lit"}) {
			return ast::false_lit{};
		} else if constexpr (n == std::string_view{"null_lit"}) {
			return ast::null_lit{};
		} else if constexpr (n == std::string_view{"array_lit"}) {
			return ast::array_lit<typename lower_arg<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"object_lit"}) {
			return ast::object_lit<typename lower_prop<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"template_lit"}) {
			return ast::template_lit<typename tpl_part<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"new_op"}) {
			return typename lower_new<Ks...>::type{};
		} else if constexpr (n == std::string_view{"comma_op"}) {
			return ast::comma_op<lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"in_op"}) {
			return ast::in_op<lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"delete_op"}) {
			return ast::delete_op<lower_expr_t<kid<0>>>{};
		} else if constexpr (n == std::string_view{"ternary"}) {
			return ast::ternary<lower_expr_t<kid<0>>, lower_expr_t<kid<1>>,
			                    lower_expr_t<kid<2>>>{};
		} else if constexpr (n == std::string_view{"assign_op"}) {
			return ast::assign<typename assign_tag<kid<1>>::type, lower_expr_t<kid<0>>,
			                   lower_expr_t<kid<2>>>{};
		} else if constexpr (n == std::string_view{"lhs_name"}) {
			return ast::ident<typename kid<0>::value_type>{};
		} else if constexpr (n == std::string_view{"lhs_member"} ||
		                     n == std::string_view{"member"}) {
			return ast::member<lower_expr_t<kid<0>>, typename kid<1>::value_type>{};
		} else if constexpr (n == std::string_view{"lhs_index"} ||
		                     n == std::string_view{"index"}) {
			return ast::index<lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"nullish_op"}) {
			return ast::binary<ast::op_nullish, lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"or_op"}) {
			return ast::binary<ast::op_or, lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"and_op"}) {
			return ast::binary<ast::op_and, lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"cmp_eq"}) {
			return ast::binary<typename eq_tag<kid<1>>::type, lower_expr_t<kid<0>>,
			                   lower_expr_t<kid<2>>>{};
		} else if constexpr (n == std::string_view{"cmp_rel"}) {
			return ast::binary<typename rel_tag<kid<1>>::type, lower_expr_t<kid<0>>,
			                   lower_expr_t<kid<2>>>{};
		} else if constexpr (n == std::string_view{"add_op"}) {
			return ast::binary<ast::op_add, lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"sub_op"}) {
			return ast::binary<ast::op_sub, lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"mul_op"}) {
			return ast::binary<typename mul_tag<kid<1>>::type, lower_expr_t<kid<0>>,
			                   lower_expr_t<kid<2>>>{};
		} else if constexpr (n == std::string_view{"pow_op"}) {
			return ast::binary<ast::op_pow, lower_expr_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"not_op"}) {
			return ast::unary<ast::op_not, lower_expr_t<kid<0>>>{};
		} else if constexpr (n == std::string_view{"neg_op"}) {
			return ast::unary<ast::op_neg, lower_expr_t<kid<0>>>{};
		} else if constexpr (n == std::string_view{"pos_op"}) {
			return ast::unary<ast::op_pos, lower_expr_t<kid<0>>>{};
		} else if constexpr (n == std::string_view{"typeof_op"}) {
			return ast::unary<ast::op_typeof, lower_expr_t<kid<0>>>{};
		} else if constexpr (n == std::string_view{"await_op"}) {
			return ast::unary<ast::op_await, lower_expr_t<kid<0>>>{};
		} else if constexpr (n == std::string_view{"pre_incdec"}) {
			return ast::incdec<lower_expr_t<kid<1>>, true, is_increment<kid<0>>()>{};
		} else if constexpr (n == std::string_view{"post_incdec"}) {
			return ast::incdec<lower_expr_t<kid<0>>, false, is_increment<kid<1>>()>{};
		} else if constexpr (n == std::string_view{"call"}) {
			return typename lower_call<lower_expr_t<kid<0>>, kid<1>>::type{};
		} else if constexpr (n == std::string_view{"fn_expr"}) {
			return ast::fn_expr<typename lower_params<kid<0>>::type,
			                    typename lower_block<kid<1>>::type, false>{};
		} else if constexpr (n == std::string_view{"async_fn_expr"}) {
			return ast::fn_expr<typename lower_params<kid<0>>::type,
			                    typename lower_block<kid<1>>::type, false, true>{};
		} else {
			static_assert(n == std::string_view{"arrow_fn"}, "ctjs: unknown expression node");
			using params = typename arrow_params<kid<0>>::type;
			if constexpr (is_block_tree<kid<1>>::value) {
				return ast::fn_expr<params, typename lower_block<kid<1>>::type, false>{};
			} else {
				return ast::fn_expr<params, lower_expr_t<kid<1>>, true>{};
			}
		}
	}
	using type = decltype(pick());
};

// --- statements

// call/array arguments: a spread_arg tree wraps its expr in
// ast::spread_arg; anything else lowers as a plain expression
// a token arg (collapsed single-token expr) or a non-spread tree is a
// plain expression; only a tree literally named spread_arg wraps
template <typename K> struct lower_arg {
	using type = lower_expr_t<K>;
};
template <typename AN, typename E> struct lower_arg<ctlark::tree<AN, E>> {
	static constexpr auto pick() {
		if constexpr (AN::view() == std::string_view{"spread_arg"}) {
			return ast::spread_arg<lower_expr_t<E>>{};
		} else {
			return lower_expr_t<ctlark::tree<AN, E>>{};
		}
	}
	using type = decltype(pick());
};

// template literal parts: TEMPLATE_* tokens are text; any other kid
// (tree OR collapsed single-token expression like a bare NAME) lowers
// as an expression
template <typename TN, typename TV> struct tpl_part<ctlark::token<TN, TV>> {
	static constexpr auto pick() {
		if constexpr (TN::view().starts_with("TEMPLATE")) {
			return ast::tpl_text<TV>{};
		} else {
			return typename lower_expr<ctlark::token<TN, TV>>::type{};
		}
	}
	using type = decltype(pick());
};
template <typename TN, typename... Ks3> struct tpl_part<ctlark::tree<TN, Ks3...>> {
	using type = lower_expr_t<ctlark::tree<TN, Ks3...>>;
};

// new C(a, b): kids are [callee, args-tree]; args-tree kids are exprs
template <typename Callee, typename AN, typename... ArgKs>
struct lower_new<Callee, ctlark::tree<AN, ArgKs...>> {
	using type = ast::new_op<lower_expr_t<Callee>, typename lower_arg<ArgKs>::type...>;
};
template <typename Callee> struct lower_new<Callee> {
	using type = ast::new_op<lower_expr_t<Callee>>;
};

template <typename MN, typename Name, typename Params, typename Body>
struct lower_method<ctlark::tree<MN, Name, Params, Body>> {
	using type = ast::class_method<typename Name::value_type,
	                               typename lower_params<Params>::type,
	                               lower_stmt_t<Body>>;
};
template <typename Name, typename... Ms> struct lower_class<Name, Ms...> {
	using type = ast::class_decl<typename Name::value_type,
	                             typename lower_method<Ms>::type...>;
};

template <typename Tree> struct lower_clause;
template <typename CN, typename K0, typename... Ks2>
struct lower_clause<ctlark::tree<CN, K0, Ks2...>> {
	static constexpr auto pick() {
		if constexpr (CN::view() == std::string_view{"case_clause"}) {
			return ast::case_clause<lower_expr_t<K0>, lower_stmt_t<Ks2>...>{};
		} else {
			return ast::default_clause<lower_stmt_t<K0>, lower_stmt_t<Ks2>...>{};
		}
	}
	using type = decltype(pick());
};
template <typename CN> struct lower_clause<ctlark::tree<CN>> {
	using type = ast::default_clause<>;
};
template <typename... Ks2> struct lower_switch;
template <typename D, typename... Cs> struct lower_switch<D, Cs...> {
	using type = ast::switch_stmt<lower_expr_t<D>, typename lower_clause<Cs>::type...>;
};

template <typename TN, typename... Ks> struct lower_stmt<ctlark::tree<TN, Ks...>> {
	template <size_t I> using kid = nth_t<I, Ks...>;
	static constexpr auto pick() {
		constexpr std::string_view n = TN::view();
		if constexpr (n == std::string_view{"block"}) {
			return ast::block<lower_stmt_t<Ks>...>{};
		} else if constexpr (n == std::string_view{"let_stmt"}) {
			return ast::let_stmt<typename lower_decl<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"const_stmt"}) {
			return ast::const_stmt<typename lower_decl<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"varkw_stmt"}) {
			return ast::var_stmt<typename lower_decl<Ks>::type...>{};
		} else if constexpr (n == std::string_view{"fn_decl"}) {
			return ast::fn_decl<typename kid<0>::value_type,
			                    typename lower_params<kid<1>>::type,
			                    typename lower_block<kid<2>>::type>{};
		} else if constexpr (n == std::string_view{"async_fn_decl"}) {
			return ast::fn_decl<typename kid<0>::value_type,
			                    typename lower_params<kid<1>>::type,
			                    typename lower_block<kid<2>>::type, true>{};
		} else if constexpr (n == std::string_view{"if_stmt"}) {
			if constexpr (sizeof...(Ks) == 3) {
				return ast::if_stmt<lower_expr_t<kid<0>>, lower_stmt_t<kid<1>>,
				                    lower_stmt_t<kid<2>>>{};
			} else {
				return ast::if_stmt<lower_expr_t<kid<0>>, lower_stmt_t<kid<1>>, void>{};
			}
		} else if constexpr (n == std::string_view{"while_stmt"}) {
			return ast::while_stmt<lower_expr_t<kid<0>>, lower_stmt_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"do_stmt"}) {
			return ast::do_stmt<lower_stmt_t<kid<0>>, lower_expr_t<kid<1>>>{};
		} else if constexpr (n == std::string_view{"for_stmt"}) {
			return ast::for_stmt<typename lower_for_init<kid<0>>::type,
			                     typename lower_for_expr<kid<1>>::type,
			                     typename lower_for_expr<kid<2>>::type,
			                     lower_stmt_t<kid<3>>>{};
		} else if constexpr (n == std::string_view{"forof_let"} ||
		                     n == std::string_view{"forof_const"} ||
		                     n == std::string_view{"forof_var"}) {
			return ast::forof_stmt<typename kid<0>::value_type, lower_expr_t<kid<1>>,
			                       lower_stmt_t<kid<2>>>{};
		} else if constexpr (n == std::string_view{"return_stmt"}) {
			if constexpr (sizeof...(Ks) == 1) {
				return ast::return_stmt<lower_expr_t<kid<0>>>{};
			} else {
				return ast::return_stmt<void>{};
			}
		} else if constexpr (n == std::string_view{"break_stmt"}) {
			return ast::break_stmt{};
		} else if constexpr (n == std::string_view{"continue_stmt"}) {
			return ast::continue_stmt{};
		} else if constexpr (n == std::string_view{"class_decl"}) {
			return typename lower_class<Ks...>::type{};
		} else if constexpr (n == std::string_view{"switch_stmt"}) {
			return typename lower_switch<Ks...>::type{};
		} else if constexpr (n == std::string_view{"empty_stmt"}) {
			return ast::empty_stmt{};
		} else if constexpr (n == std::string_view{"throw_stmt"}) {
			return ast::throw_stmt<lower_expr_t<kid<0>>>{};
		} else if constexpr (n == std::string_view{"try_catch"}) {
			return ast::try_stmt<lower_stmt_t<kid<0>>, typename kid<1>::value_type,
			                     lower_stmt_t<kid<2>>, void>{};
		} else if constexpr (n == std::string_view{"try_catch_fin"}) {
			return ast::try_stmt<lower_stmt_t<kid<0>>, typename kid<1>::value_type,
			                     lower_stmt_t<kid<2>>, lower_stmt_t<kid<3>>>{};
		} else if constexpr (n == std::string_view{"try_fin"}) {
			return ast::try_stmt<lower_stmt_t<kid<0>>, void, void, lower_stmt_t<kid<1>>>{};
		} else {
			static_assert(n == std::string_view{"expr_stmt"}, "ctjs: unknown statement node");
			return ast::expr_stmt<lower_expr_t<kid<0>>>{};
		}
	}
	using type = decltype(pick());
};

// --- the program

template <typename Tree> struct lower_program;
template <typename SN, typename... Ss> struct lower_program<ctlark::tree<SN, Ss...>> {
	using type = ast::program<lower_stmt_t<Ss>...>;
};

} // namespace ctjs::detail

#endif
