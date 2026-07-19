#ifndef CTJS__RC__HPP
#define CTJS__RC__HPP

#ifndef CTJS_IN_A_MODULE
#include <cstddef>
#include <utility>
#endif

// A constexpr reference-counted shared pointer - std::shared_ptr is not
// usable in constant evaluation (C++23), this is. Same reference
// semantics the JS value model needs (objects/arrays/functions/scopes
// are shared and mutable), refcounted with constexpr new/delete (the
// allocation is transient and released by the end of the evaluation).
// Interface mirrors the shared_ptr subset the interpreter uses.

namespace ctjs {

template <class T> class rc {
	struct block {
		T value;
		long count;
	};
	block * b_ = nullptr;

public:
	constexpr rc() noexcept = default;
	constexpr rc(std::nullptr_t) noexcept { }

	// allocate a T, forwarding to its constructor (brace-init so aggregates work)
	template <class... A> static constexpr rc make(A &&... a) {
		rc r;
		r.b_ = new block{T{std::forward<A>(a)...}, 1};
		return r;
	}

	constexpr rc(const rc & o) noexcept : b_{o.b_} {
		if (b_ != nullptr) { ++b_->count; }
	}
	constexpr rc(rc && o) noexcept : b_{o.b_} { o.b_ = nullptr; }
	constexpr rc & operator=(const rc & o) noexcept {
		if (this != &o) {
			release();
			b_ = o.b_;
			if (b_ != nullptr) { ++b_->count; }
		}
		return *this;
	}
	constexpr rc & operator=(rc && o) noexcept {
		if (this != &o) {
			release();
			b_ = o.b_;
			o.b_ = nullptr;
		}
		return *this;
	}
	constexpr rc & operator=(std::nullptr_t) noexcept {
		release();
		b_ = nullptr;
		return *this;
	}
	constexpr ~rc() { release(); }

	constexpr T * get() const noexcept { return b_ ? &b_->value : nullptr; }
	constexpr T & operator*() const noexcept { return b_->value; }
	constexpr T * operator->() const noexcept { return &b_->value; }
	constexpr explicit operator bool() const noexcept { return b_ != nullptr; }

	constexpr bool operator==(const rc & o) const noexcept { return b_ == o.b_; }
	constexpr bool operator==(std::nullptr_t) const noexcept { return b_ == nullptr; }

private:
	constexpr void release() {
		if (b_ != nullptr && --b_->count == 0) { delete b_; }
	}
};

} // namespace ctjs

#endif
