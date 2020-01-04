#ifndef __UNUSED_H_
#define __UNUSED_H_
#define __UNUSED(x) (void)x

#if defined(__clang__)
#define __NO_DEPRECATED_BEGIN \
  _Pragma("clang diagnostic push") \
  _Pragma("clang diagnostic ignored \"-Wdeprecated\"")
#define __NO_DEPRECATED_END _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define __NO_DEPRECATED_BEGIN \
  _Pragma("GCC diagnostic push") \
  _Pragma("GCC diagnostic ignored \"-Wdeprecated\"")
#define __NO_DEPRECATED_END _Pragma("GCC diagnostic pop")
#else
#define __NO_DEPRECATED_BEGIN
#define __NO_DEPRECATED_END
#endif

#if defined(__clang__)
#define __NO_PADDING_BEGIN \
  _Pragma("clang diagnostic push") \
  _Pragma("clang diagnostic ignored \"-Wpadded\"")
#define __NO_PADDING_END _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define __NO_PADDING_BEGIN \
  _Pragma("GCC diagnostic push") \
  _Pragma("GCC diagnostic ignored \"-Wpadded\"")
#define __NO_PADDING_END _Pragma("GCC diagnostic pop")
#else
#define __NO_PADDING_BEGIN
#define __NO_PADDING_END
#endif

#ifdef __clang__
#define __NO_UNUSED_FUNCTION_BEGIN \
  _Pragma("clang diagnostic push") \
  _Pragma("clang diagnostic ignored \"-Wunused-function\"")
#define __NO_UNUSED_FUNCTION_END _Pragma("clang diagnostic pop")
#else
#define __NO_UNUSED_FUNCTION_BEGIN
#define __NO_UNUSED_FUNCTION_END
#endif

#ifdef __clang__
#define __NO_EXPAND_RECURSIVE_MACRO_BEGIN \
  _Pragma("clang diagnostic push") \
  _Pragma("clang diagnostic ignored \"-Wdisabled-macro-expansion\"")
#define __NO_EXPAND_RECURSIVE_MACRO_END _Pragma("clang diagnostic pop")
#else
#define __NO_EXPAND_RECURSIVE_MACRO_BEGIN
#define __NO_EXPAND_RECURSIVE_MACRO_END
#endif

#endif
