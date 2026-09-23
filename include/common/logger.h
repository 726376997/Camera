#ifndef LOGGER_H_
#define LOGGER_H_

#include <stdio.h>

// #define DEBUG_MODE

#ifdef DEBUG_MODE
#define DBG_PRINTF(...) printf(__VA_ARGS__) // 调试模式：打印
#else
#define DBG_PRINTF(...) ((void)0) // 发布模式：什么都不做
#endif

#define ERR_PRINTF(...) fprintf(stderr, __VA_ARGS__) // 打印错误

#endif /* LOGGER_H_ */
