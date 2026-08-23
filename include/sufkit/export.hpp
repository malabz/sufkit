#pragma once

/** @file
 *  @brief Cross-platform public symbol visibility macro.
 */

#if defined(_WIN32) && !defined(SUFKIT_STATIC)
#  if defined(SUFKIT_BUILDING_LIBRARY)
#    define SUFKIT_API __declspec(dllexport)
#  else
#    define SUFKIT_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && !defined(SUFKIT_STATIC)
#  define SUFKIT_API __attribute__((visibility("default")))
#else
#  define SUFKIT_API
#endif
