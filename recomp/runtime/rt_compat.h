/* rt_compat.h: the few POSIX calls Windows (mingw) spells differently */
#pragma once
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <time.h>
#define mkdir(path, mode) _mkdir(path)
#define localtime_r(t, tm) (localtime_s((tm), (t)) ? NULL : (tm))
#define gmtime_r(t, tm) (gmtime_s((tm), (t)) ? NULL : (tm))
#endif
