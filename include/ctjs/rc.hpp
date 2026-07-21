#ifndef CTJS__RC__HPP
#define CTJS__RC__HPP

#ifndef CTJS_IN_A_MODULE
#include <cstddef>
#include <type_traits>
#include <utility>
#endif

#include "gc.hpp"

// A constexpr reference-counted shared pointer - std::shared_ptr is not
// usable in constant evaluation (C++23), this is. Same reference
// semantics the JS value model needs (objects/arrays/functions/scopes
// are shared and mutable), refcounted with constexpr new/delete (the
// allocation is transient and released by the end of the evaluation).
// Interface mirrors the shared_ptr subset the interpreter uses.
//
// RUNTIME cycle collection: the refcount lives in a gc::header at the head
// of every block. For participating types (gc::participates<T>), rc installs
// type-erased trace/clear/destroy hooks and feeds decrements-to-nonzero to the
// cycle collector's roots buffer (see gc.hpp). ALL of that is guarded
// `if (!std::is_constant_evaluated())`, so the compile-time interpreter behaves
// exactly as before - pure refcounting, cycles released when the eval arena is
// torn down. The collector never runs at compile time.

namespace ctjs {

template <class T> class rc {
	struct block {
		gc::header hdr;
		T value;
	};
	block * b_ = nullptr;

	static void gc_destroy(void * owner) { delete static_cast<block *>(owner); }

public:
	constexpr rc() noexcept = default;
	constexpr rc(std::nullptr_t) noexcept { }

	// allocate a T, forwarding to its constructor (brace-init so aggregates work)
	template <class... A> static constexpr rc make(A &&... a) {
		rc r;
		r.b_ = new block{gc::header{}, T{std::forward<A>(a)...}};
		r.b_->hdr.count = 1;
		if constexpr (gc::participates<T>) {
			if (!std::is_constant_evaluated()) {
				r.b_->hdr.obj = &r.b_->value;
				r.b_->hdr.owner = r.b_;
				r.b_->hdr.trace = &gc::trace_thunk<T>;
				r.b_->hdr.clear = &gc::clear_thunk<T>;
				r.b_->hdr.destroy = &gc_destroy;
			}
		}
		return r;
	}

	constexpr rc(const rc & o) noexcept : b_{o.b_} { incref(); }
	constexpr rc(rc && o) noexcept : b_{o.b_} { o.b_ = nullptr; }
	constexpr rc & operator=(const rc & o) noexcept {
		if (this != &o) {
			release();
			b_ = o.b_;
			incref();
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

	// the collector's handle to this block (null when empty)
	constexpr gc::header * gc_header() const noexcept { return b_ ? &b_->hdr : nullptr; }

private:
	constexpr void incref() noexcept {
		if (b_ == nullptr) { return; }
		++b_->hdr.count;
		if (!std::is_constant_evaluated()) { b_->hdr.col = gc::color::black; }
	}
	constexpr void release() {
		if (b_ == nullptr) { return; }
		if (--b_->hdr.count == 0) {
			// a buffered node stays for the collector (leaving a dangling pointer
			// in the roots buffer would be a use-after-free); it is reclaimed in
			// the next collect()'s MarkRoots. Everything else frees immediately.
			if (std::is_constant_evaluated() || !b_->hdr.buffered) {
				delete b_;
			} else {
				b_->hdr.col = gc::color::black;
			}
		} else if (!std::is_constant_evaluated()) {
			if (b_->hdr.trace != nullptr) { gc::possible_root(&b_->hdr); }
		}
	}
};

} // namespace ctjs

#endif
