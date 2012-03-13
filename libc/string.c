/*
 * string.c
 */
#include <types.h>
#include <stdio.h>
#include <string.h>

int strcmp(const char *str1, const char *str2)
{
	int res = 0;
	while (!(res = *(unsigned char *)str1 - *(unsigned char *)str2) && *str2)
		++str1, ++str2;

	if (res < 0)
		res = -1;
	if (res > 0)
		res = 1;

	return res;
}

char * strcpy(char *dst, const char *src)
{
	char *cp = dst;
	while (*(cp++) = *(src++));
	return (dst);
}

size_t strlen(const char *str)
{
	size_t len = 0;
	while (str[len++]);
	return len;
}

char * strchr(const char *str, int ch)
{
	while (*str && *str != (char)ch)
		str++;
	if (*str == (char)ch)
		return ((char*)str);
	return NULL;
}

void * memset(void *dst, char val, size_t count)
{
	unsigned char *temp = (unsigned char *)dst;
	for (; count != 0; count--, temp[count] = val);
	return dst;
}

void * memcpy(void *dst, const void *src, size_t count)
{
	const char *sp = (const char *)src;
	char *dp = (char *)dst;
	for (; count != 0; count--)
		*dp++ = *sp++;
	return dst;
}