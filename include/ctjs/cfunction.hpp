#ifndef CTJS__CFUNCTION__HPP
#define CTJS__CFUNCTION__HPP

#ifndef CTJS_IN_A_MODULE
#include <memory>
#include <utility>
#endif

// A constexpr type-erased callable - std::function is not usable in
// constant evaluation (C++23), this is. It stores the callable behind a
// constexpr-virtual interface owned by a constexpr std::unique_ptr, so
// the whole thing is evaluable at compile time. Adapted from the
// compile-time function wrapper (thanks to the pattern the user shared);
// made copyable (via a clone hook) since a JS function value is copied.

namespace ctjs {

template <class> class cfunction;

template <class R, class... Args> class cfunction<R(Args...)> {
public:
	cfunction() = default; // empty
	template <class F>
	    requires(!std::is_same_v<std::decay_t<F>, cfunction>)
	constexpr cfunction(F f) : ptr_{std::make_unique<impl<F>>(std::move(f))} { }

	constexpr cfunction(const cfunction & o) : ptr_{o.ptr_ ? o.ptr_->clone() : nullptr} { }
	constexpr cfunction(cfunction &&) noexcept = default;
	constexpr cfunction & operator=(const cfunction & o) {
		ptr_ = o.ptr_ ? o.ptr_->clone() : nullptr;
		return *this;
	}
	constexpr cfunction & operator=(cfunction &&) noexcept = default;

	constexpr explicit operator bool() const { return static_cast<bool>(ptr_); }
	constexpr R operator()(Args... args) const { return ptr_->call(static_cast<Args>(args)...); }

private:
	struct iface {
		constexpr virtual R call(Args...) const = 0;
		constexpr virtual std::unique_ptr<iface> clone() const = 0;
		constexpr virtual ~iface() = default;
	};
	template <class F> struct impl final : iface {
		constexpr explicit impl(F f) : f_{std::move(f)} { }
		constexpr R call(Args... args) const override {
			return f_(static_cast<Args>(args)...);
		}
		constexpr std::unique_ptr<iface> clone() const override {
			return std::make_unique<impl>(f_);
		}
		F f_;
	};
	std::unique_ptr<iface> ptr_;
};

} // namespace ctjs

#endif
