#pragma once

#include <stdbool.h>

#define TOK_MAX  64
/* 512 per token - plenty for any flag or path segment.
 * the full command string can be 8192, but individual tokens never approach that.
 * was 4096 (PATH_MAX) before, which made TokList 256KB on the stack. not ideal. */
#define TOK_SLEN 512

typedef enum {
    TT_COMMAND,
    TT_FLAG_SHORT,  /* -r, -rf */
    TT_FLAG_LONG,   /* --recursive, --force=yes */
    TT_WORD,
} TokType;

typedef struct {
    TokType type;
    char    s[TOK_SLEN];
} Tok;

typedef struct {
    Tok t[TOK_MAX];
    int n;
    int dd;     /* index of the first token after a bare "--", -1 if none */
} TokList;

/* how many tokens we parsed, -1 if something went wrong */
int tok_parse(const char *restrict cmd, TokList *restrict out);

const Tok  *tok_first_word(const TokList *tl, int from);
bool        tok_has_short(const TokList *tl, char flag);
bool        tok_has_long(const TokList *tl, const char *flag);
const char *tok_long_val(const TokList *tl, const char *flag);
const char *tok_short_val(const TokList *tl, char flag);
