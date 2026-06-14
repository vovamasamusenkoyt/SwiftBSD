#pragma once
#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
int isdigit(int c);
int isalpha(int c);
int isspace(int c);
