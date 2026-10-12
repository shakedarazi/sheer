#include "token.h"

#include <ctype.h>
#include <string.h>

typedef enum { S_NORMAL, S_SQ, S_DQ, S_ESC, S_DQESC } LexState;

/*
 * we are not bash. we will never be bash. bash has decades of accumulated
 * demons and we want none of it. no $VAR, no $(), no backticks, no here-docs,
 * no brace expansion, no arithmetic. we just want the structural shape:
 * what's the command, what are the flags, what are the arguments.
 *
 * consequence: "rm -rf $HOME" is explained as "rm -rf $HOME", not as
 * "rm -rf /home/yourname". that's fine for our purpose - we care about
 * intent, not expansion.
 */
int tok_parse(const char *restrict cmd, TokList *restrict out)
{
    if (!cmd || !out)
        return -1;

    out->n  = 0;
    out->dd = -1;

    char     buf[TOK_SLEN];
    int      blen     = 0;
    LexState st       = S_NORMAL;
    bool     in_tok   = false;
    bool     dashdash = false;  /* seen "--" separator yet? */

    for (const char *p = cmd; ; ++p) {
        const char c = *p;

        switch (st) {

        case S_NORMAL:
            if (c == '\0' || isspace((unsigned char)c)) {
                if (in_tok) {
                    if (out->n >= TOK_MAX)
                        return -1;  /* TOK_MAX is 64, if you hit this... wow */

                    buf[blen] = '\0';
                    Tok *tok  = &out->t[out->n];
                    memcpy(tok->s, buf, (size_t)blen + 1);

                    if (out->n == 0) {
                        tok->type = TT_COMMAND;
                    } else if (!dashdash && buf[0] == '-' && buf[1] == '-' && buf[2] == '\0') {
                        dashdash = true;
                        out->dd  = out->n;
                        in_tok   = false;
                        blen     = 0;
                        if (c == '\0') goto done;
                        continue;
                    } else if (!dashdash && buf[0] == '-' && buf[1] != '\0') {
                        tok->type = (buf[1] == '-') ? TT_FLAG_LONG : TT_FLAG_SHORT;
                    } else {
                        tok->type = TT_WORD;
                    }

                    out->n++;
                    in_tok = false;
                    blen   = 0;
                }
                if (c == '\0') goto done;

            } else {
                in_tok = true;
                if      (c == '\'') { st = S_SQ; }
                else if (c == '"')  { st = S_DQ; }
                else if (c == '\\') { st = S_ESC; }
                else if (blen < TOK_SLEN - 1) buf[blen++] = c;
            }
            break;

        case S_SQ:
            if (c == '\0') goto done;  /* unterminated quote, just bail */
            if (c == '\'') { st = S_NORMAL; in_tok = true; }
            else if (blen < TOK_SLEN - 1) buf[blen++] = c;
            break;

        case S_DQ:
            if (c == '\0') goto done;
            if      (c == '"')  { st = S_NORMAL; in_tok = true; }
            else if (c == '\\') { st = S_DQESC; }
            else if (blen < TOK_SLEN - 1) buf[blen++] = c;
            break;

        case S_ESC:
            if (c == '\0') goto done;
            if (blen < TOK_SLEN - 1) buf[blen++] = c;
            st = S_NORMAL;
            break;

        case S_DQESC:
            if (c == '\0') goto done;
            /* inside double-quotes, only \", \\, \$, \` are actual escapes.
             * anything else: keep the backslash. yes, this is what bash does. */
            if (c != '"' && c != '\\' && c != '$' && c != '`')
                if (blen < TOK_SLEN - 1) buf[blen++] = '\\';
            if (blen < TOK_SLEN - 1) buf[blen++] = c;
            st = S_DQ;
            break;
        }
    }

done:
    return out->n;
}

const Tok *tok_first_word(const TokList *tl, int from)
{
    for (int i = from; i < tl->n; i++)
        if (tl->t[i].type == TT_WORD)
            return &tl->t[i];
    return NULL;
}

bool tok_has_short(const TokList *tl, char flag)
{
    for (int i = 0; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_FLAG_SHORT) continue;
        /*
         * Little be hacky there: tokens longer than 4 chars after '-' are probably find(1)-style
         * named options (-exec, -name, -mtime) not bundles like -rfa.
         * so we skip them. this means -rvvvvf would be silently ignored.
         * nobody writes that, but just so you know.
         * the real fix is a per-command flag registry. TODO, someday, maybe..
         */
        if (strlen(t->s + 1) > 4) continue;
        for (const char *c = t->s + 1; *c; c++)
            if (*c == flag) return true;
    }
    return false;
}

bool tok_has_long(const TokList *tl, const char *flag)
{
    const size_t flen = strlen(flag);
    for (int i = 0; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_FLAG_LONG) continue;
        if (strncmp(t->s, flag, flen) == 0 &&
            (t->s[flen] == '\0' || t->s[flen] == '='))
            return true;
    }
    return false;
}

const char *tok_long_val(const TokList *tl, const char *flag)
{
    const size_t flen = strlen(flag);
    for (int i = 0; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_FLAG_LONG) continue;
        if (strncmp(t->s, flag, flen) != 0) continue;
        if (t->s[flen] == '=')                    return t->s + flen + 1;
        if (t->s[flen] == '\0' && i + 1 < tl->n) return tl->t[i + 1].s;
    }
    return NULL;
}

const char *tok_short_val(const TokList *tl, char flag)
{
    for (int i = 0; i < tl->n - 1; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_FLAG_SHORT) continue;
        for (const char *c = t->s + 1; *c; c++)
            if (*c == flag && *(c + 1) == '\0')
                return tl->t[i + 1].s;
    }
    return NULL;
}
