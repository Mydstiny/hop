#pragma once
enum { LOG_APP = 0, LOG_INFO = 0, LOG_ERROR = 1 };
inline void OH_LOG_Print(int, int, unsigned, const char*, const char*, ...) {}
