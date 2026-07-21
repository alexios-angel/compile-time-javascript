#ifndef CTJS__GC__HPP
#define CTJS__GC__HPP

#ifndef CTJS_IN_A_MODULE
#include <vector>
#endif

// Synchronous cycle collector (Bacon & Rajan, "Concurrent Cycle Collection in
// Reference Counted Systems", 2001 - synchronous variant), layered on rc<T>'s
// reference counting. Reference counting already reclaims all ACYCLIC garbage
// promptly; this reclaims reference CYCLES, which pure refcounting cannot.
//
// RUNTIME ONLY. Every hook is inert during constant evaluation: the compile-time
// interpreter frees its whole allocation arena when evaluation ends, so cycles
// there are harmless and no collection is needed (and a global roots buffer is
// not usable in a constant expression anyway). rc.hpp guards every gc:: call
// with `if !consteval`.
//
// Each collectable rc block carries a `header` at offset 0. `obj` points at the
// T payload; `trace`/`clear`/`destroy` are type-erased per-T operations installed
// when the block is created (see rc<T>::make). Types that set no trace fn (e.g.
// rc<context>) never participate - they are treated as opaque roots (safe: never
// collected).

namespace ctjs::gc {

enum class color : unsigned char { black, gray, white, purple };

struct header {
	long count = 0;         // the reference count (the sole count; rc<T> uses it)
	color col = color::black;
	bool buffered = false;  // currently in the roots candidate buffer
	void * obj = nullptr;   // -> the T payload (for trace/clear)
	void * owner = nullptr; // -> the enclosing rc block (for destroy; avoids a layout cast)
	void (*trace)(void * obj, void (*visit)(header *)) = nullptr; // visit each rc child's header
	void (*clear)(void * obj) = nullptr;                   // null out every rc child (normal release)
	void (*destroy)(void * owner) = nullptr;               // deallocate the whole block
};

// The roots candidate buffer + the in-progress white set. Runtime-only globals;
// collect() is non-reentrant (single-threaded), so a static gather target is safe.
inline std::vector<header *> & roots() {
	static std::vector<header *> r;
	return r;
}
inline std::vector<header *> *& gather_target() {
	static std::vector<header *> * p = nullptr;
	return p;
}
// true while collect() runs: the free phase releases children (via rc), which
// would otherwise re-buffer nodes we are about to destroy - leaving dangling
// pointers in the roots buffer. Suppress buffering during collection.
inline bool & collecting() {
	static bool c = false;
	return c;
}

// --- per-type participation + type-erased operations ----------------------
// A type opts in by specializing `participates<T> = true` and providing free
// functions `gc_trace(T&, visit)` / `gc_clear(T&)` (found by ADL). rc<T>::make
// installs the thunks below into the header only for participating types; every
// other type stays opaque (its objects are never buffered, never collected -
// safe, they simply cannot be part of a *traced* cycle).
template <class T> inline constexpr bool participates = false;

template <class T> inline void trace_thunk(void * o, void (*v)(header *)) {
	gc_trace(*static_cast<T *>(o), v); // ADL -> the T-specific overload
}
template <class T> inline void clear_thunk(void * o) {
	gc_clear(*static_cast<T *>(o));
}

// --- rc hooks (called only at runtime, from rc<T>) ------------------------

// a decrement left count > 0: this object might be part of a cycle
inline void possible_root(header * s) {
	if (collecting()) { return; } // don't re-buffer while the collector is freeing
	if (s->trace == nullptr) { return; } // non-collectable type: cannot form a traced cycle
	if (s->col == color::purple) { return; }
	s->col = color::purple;
	if (!s->buffered) {
		s->buffered = true;
		roots().push_back(s);
	}
}

// --- the algorithm --------------------------------------------------------

inline void mark_gray(header * s) {
	if (s->col == color::gray) { return; }
	s->col = color::gray;
	s->trace(s->obj, [](header * t) {
		--t->count; // trial deletion: remove the internal edge
		mark_gray(t);
	});
}

inline void scan_black(header * s) {
	s->col = color::black;
	s->trace(s->obj, [](header * t) {
		++t->count; // restore the internal edge
		if (t->col != color::black) { scan_black(t); }
	});
}

inline void scan(header * s) {
	if (s->col != color::gray) { return; }
	if (s->count > 0) {
		scan_black(s); // externally referenced -> live, restore
	} else {
		s->col = color::white; // no external refs -> candidate garbage
		s->trace(s->obj, [](header * t) { scan(t); });
	}
}

// gather the white set reachable from a root (recolor black as the visited mark)
inline void collect_white(header * s) {
	if (s->col == color::white) {
		s->col = color::black;
		gather_target()->push_back(s);
		s->trace(s->obj, [](header * t) { collect_white(t); });
	}
}

// Run one full collection. Frees every unreachable cycle among the candidates.
inline void collect() {
	if consteval {
		return; // never at compile time
	} else {
		if (collecting()) { return; } // non-reentrant
		collecting() = true;
		std::vector<header *> buf;
		buf.swap(roots()); // snapshot; new candidates raised during collect go to a fresh buffer

		// The set of blocks to free at the end. Everything is freed by ONE safe
		// pin/clear/destroy pass below - never inline, which would let a
		// destructor cascade corrupt a node still referenced later in the walk.
		std::vector<header *> dead;

		// MarkRoots: trial-delete internal edges from every purple candidate; drop
		// the rest. A node that hit count 0 while buffered was parked (not freed)
		// by rc release with col=black - it is dead, so defer it to the free pass.
		for (header * s : buf) {
			if (s->col == color::purple && s->count > 0) {
				mark_gray(s);
			} else {
				s->buffered = false;
				if (s->col == color::black && s->count == 0) { dead.push_back(s); }
			}
		}
		// ScanRoots: repaint live (externally-referenced) subgraphs black, the rest white
		for (header * s : buf) {
			if (s->buffered) { scan(s); }
		}
		// CollectRoots: gather the white (garbage-cycle) set into `dead` too
		gather_target() = &dead;
		for (header * s : buf) {
			if (s->buffered) {
				s->buffered = false;
				collect_white(s);
			}
		}
		gather_target() = nullptr;

		// Free `dead` WITHOUT use-after-free: pin every dying node so a cascading
		// release during clear cannot free it early, null every node's children
		// (releasing live children properly, decrementing pinned dying ones
		// harmlessly), then deallocate every dying node unconditionally - by now
		// nothing (live or dying) still references any of them.
		constexpr long PIN = 1L << 40;
		for (header * s : dead) { s->count += PIN; }
		for (header * s : dead) {
			if (s->clear != nullptr) { s->clear(s->obj); }
		}
		for (header * s : dead) {
			if (s->destroy != nullptr) { s->destroy(s->owner); }
		}
		collecting() = false;
	}
}

} // namespace ctjs::gc

#endif
