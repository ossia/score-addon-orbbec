/*
 * Force-included before the libPointCloud headers. See Backends/CMakeLists.txt.
 *
 * The SDK's Common.h trims strings with std::ptr_fun, removed in C++17. The
 * functions are inline in a vendored header we cannot patch, so they have to
 * compile even though nothing calls them. libstdc++ still declares ptr_fun;
 * libc++ deleted it, and _LIBCPP_ENABLE_CXX17_REMOVED_BINDERS -- the escape
 * hatch for exactly this -- went away in LLVM 19. Hence the shim.
 */
#ifndef SCORE_DEPTHCAM_OAK_CXX17_COMPAT_H
#define SCORE_DEPTHCAM_OAK_CXX17_COMPAT_H

/*
 * MSVC's own escape hatch, which only works if it is defined before
 * <yvals_core.h> is first pulled in -- hence /FI rather than an #include.
 */
#if defined(_MSC_VER) && !defined(_HAS_AUTO_PTR_ETC)
#define _HAS_AUTO_PTR_ETC 1
#endif

#include <version> // the standard library's feature macros, and nothing else

/*
 * Keyed on the two libraries that *do* provide ptr_fun, not on libc++: an
 * unfamiliar standard library then gets the shim and, if it turns out not to
 * need it, a clear redefinition error. Not _GLIBCXX_USE_DEPRECATED, which only
 * marks ptr_fun deprecated -- libstdc++ declares it either way.
 */
#if !defined(_MSC_VER) && !defined(__GLIBCXX__)

namespace std
{
/// The bare minimum the SDK's use needs: std::find_if only calls the predicate.
template <typename Arg, typename Result>
struct pointer_to_unary_function
{
  using argument_type = Arg;
  using result_type = Result;

  Result (*_M_ptr)(Arg);

  explicit pointer_to_unary_function(Result (*f)(Arg))
      : _M_ptr{f}
  {
  }

  Result operator()(Arg x) const { return _M_ptr(x); }
};

template <typename Arg, typename Result>
inline pointer_to_unary_function<Arg, Result> ptr_fun(Result (*f)(Arg))
{
  return pointer_to_unary_function<Arg, Result>{f};
}
}

#endif

#endif
