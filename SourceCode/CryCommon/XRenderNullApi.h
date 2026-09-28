#ifndef CRY_XRENDER_NULL_API_H
#define CRY_XRENDER_NULL_API_H

#if defined(_WIN32) && !defined(LINUX)
#  if defined(XRENDERNULL_EXPORTS)
#    define XRENDERNULL_API __declspec(dllexport)
#  elif defined(XRENDERNULL_IMPORTS)
#    define XRENDERNULL_API __declspec(dllimport)
#  else
#    define XRENDERNULL_API
#  endif
#else
#  define XRENDERNULL_API
#endif

#endif
