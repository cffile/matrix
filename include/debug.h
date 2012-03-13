#ifndef __DEBUG_H__
#define __DEBUG_H__

#include "util.h"

#define DL_ERR		0x00000004
#define DL_WRN		0x00000003
#define DL_INF		0x00000002
#define DL_DBG		0x00000001

#define DEBUG(level, params) do { \
		if (debug_level >= (level)) { \
			kprintf("[DEBUG] ");  \
			kprintf params;	      \
		}			      \
	} while (0)


#define PANIC(msg)	panic(msg, __FILE__, __LINE__)

#define ASSERT(b)	((b) ? (void)0: panic_assert(__FILE__, __LINE__, #b))


extern uint32_t debug_level;

void panic(const char *message, const char *file, uint32_t line);

void panic_assert(const char *file, uint32_t line, const char *desc);

#endif	/* __DEBUG_H__ */