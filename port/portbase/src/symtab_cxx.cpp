/*
 * bionic's libstdc++.so - the four allocation operators, and nothing else.
 *
 * Android's libstdc++.so is not GNU's. It is a stub the NDK ships whose entire
 * public surface is operator new, operator new[], operator delete and
 * operator delete[]; everything else a C++ program needs (the STL, RTTI, the
 * exception runtime) is statically linked into the game. So a game either has
 * no mangled C++ import at all - which is what every earlier port here looked
 * like, and why symtab.cpp used to say libstdc++ was "listed but unused" - or
 * it imports exactly these four.
 *
 * The failure without them is not subtle but it is misattributed: the loader
 * reports four unresolved symbols with names like _Znwj, which read as
 * something exotic, and the game cannot allocate a single object.
 *
 * Names, from the Itanium C++ ABI mangling:
 *
 *   _Znwj   operator new(unsigned int)
 *   _Znaj   operator new[](unsigned int)
 *   _ZdlPv  operator delete(void *)
 *   _ZdaPv  operator delete[](void *)
 *
 * The 'j' is `unsigned int`, which is what size_t mangles to on a 32-bit ARM
 * target. A 64-bit build would spell the same operators _Znwm/_Znam, so a
 * table entry copied from an x86-64 reference matches nothing here.
 *
 * ---------------------------------------------------------------------------
 * Why these forward to the host's operators rather than to malloc/free
 *
 * Both would work for a program that only ever pairs new with delete. They
 * stop being interchangeable the moment memory crosses the boundary in the
 * other direction: this loader's own C++ code allocates with the host's
 * operator new, and anything the game frees on our behalf - or we free on the
 * game's - has to reach the same allocator. Going through the operators keeps
 * one allocator on both sides of the boundary, and it keeps whatever
 * new_handler the host runtime has installed.
 *
 * No float crosses any of these boundaries, so no ABI bridge is generated:
 * they resolve to direct pointers.
 */

#include <new>
#include <stddef.h>
#include <stdint.h>

#include "so_util.h"
#include "thunk_gen.h"

/*
 * bionic's operators are the non-throwing kind in practice: the NDK stub
 * returns NULL rather than raising std::bad_alloc, and 2011-era engines test
 * the result. Requesting nothrow here keeps that contract - a game that checks
 * for NULL gets its NULL instead of an exception unwinding through frames the
 * loader compiled with a different runtime.
 */
extern "C" void *bionic_operator_new(size_t size)
{
    return ::operator new(size, std::nothrow);
}

extern "C" void *bionic_operator_new_array(size_t size)
{
    return ::operator new[](size, std::nothrow);
}

extern "C" void bionic_operator_delete(void *p)
{
    ::operator delete(p);
}

extern "C" void bionic_operator_delete_array(void *p)
{
    ::operator delete[](p);
}

/* The nothrow forms (Shadow Guardian HD imports operator new[](size_t,
 * nothrow_t const&) and the std::nothrow object itself). nothrow_t is empty;
 * the object only has to have an address. */
extern "C" void *bionic_operator_new_array_nothrow(size_t size, const void *)
{
    return ::operator new[](size, std::nothrow);
}

extern "C" void *bionic_operator_new_nothrow(size_t size, const void *)
{
    return ::operator new(size, std::nothrow);
}

static const std::nothrow_t bionic_nothrow{};

/* double -> int64, truncating (RTABI 4.1.2). The double arrives in r0:r1
 * under the soft-float call convention the game uses, hence pcs("aapcs"). */
extern "C" __attribute__((pcs("aapcs"))) long long bionic_aeabi_d2lz(double a)
{
    return (long long)a;
}

/* C++ exception personality routines, referenced from the game's .ARM.exidx.
 * Integer-only interface, so libgcc's own serve as they are. */
extern "C" int __aeabi_unwind_cpp_pr0(int, void *, void *);
extern "C" int __aeabi_unwind_cpp_pr1(int, void *, void *);

/* The run-time ABI's divmod helpers return quotient and remainder in r0:r1
 * (r0-r3 for the 64-bit ones). The generated libc table declares them as
 * returning a struct, which the host compiler returns through a hidden
 * pointer in r0 - every argument shifts by one register and the divisor
 * reads as 0: SIGFPE in Shadow Guardian's first static constructor (boost
 * unordered_map, hash % bucket_count). Same fix as Shadowgun Bridge
 * (symtab_unity.cpp): libgcc's own, integer-only, bound as they are. */
extern "C" void __aeabi_idivmod(void);
extern "C" void __aeabi_uidivmod(void);
extern "C" void __aeabi_ldivmod(void);
extern "C" void __aeabi_uldivmod(void);

DynLibFunction symtable_cxx[] = {
    NO_THUNK("__aeabi_idivmod",  (uintptr_t)&__aeabi_idivmod),
    NO_THUNK("__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod),
    NO_THUNK("__aeabi_ldivmod",  (uintptr_t)&__aeabi_ldivmod),
    NO_THUNK("__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod),
    NO_THUNK("_ZnwjRKSt9nothrow_t", (uintptr_t)&bionic_operator_new_nothrow),
    NO_THUNK("_ZnajRKSt9nothrow_t", (uintptr_t)&bionic_operator_new_array_nothrow),
    NO_THUNK("_ZSt7nothrow", (uintptr_t)&bionic_nothrow),
    NO_THUNK("__aeabi_d2lz", (uintptr_t)&bionic_aeabi_d2lz),
    NO_THUNK("__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0),
    NO_THUNK("__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1),
    NO_THUNK("_Znwj",  (uintptr_t)&bionic_operator_new),
    NO_THUNK("_Znaj",  (uintptr_t)&bionic_operator_new_array),
    NO_THUNK("_ZdlPv", (uintptr_t)&bionic_operator_delete),
    NO_THUNK("_ZdaPv", (uintptr_t)&bionic_operator_delete_array),

    { NULL, 0 },
};
