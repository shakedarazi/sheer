#pragma once

#include "cmd.h"
#include <stdbool.h>

typedef enum {
    SEP_PIPE  = 0,
    SEP_AND   = 1,
    SEP_OR    = 2,
    SEP_SEMI  = 3,
} SepKind;

static inline const char *sep_label(SepKind k)
{
    switch (k) {
    case SEP_PIPE: return "│";
    case SEP_AND:  return "then (if ok)";
    case SEP_OR:   return "or (if failed)";
    case SEP_SEMI: return "then";
    }
    return "";
}

void out_render(const Analysis *a, const char *raw_cmd, bool color);

/* pipeline: n stages each with their own analysis, plus an overall risk.
 * seps[i] is the separator that *precedes* stage i (seps[0] is ignored). */
void out_render_pipeline(const char **stages, const Analysis *analyses,
                         const SepKind *seps, int n, Risk overall, bool color);

/* --plain: one line per step, unwrapped. &&, ||, ; and a newline start a
 * step; the stages of one pipe are one step. A stage with a clause
 * (clauses[i], from cmd_clause) adds it to the sentence before it;
 * otherwise its sentence follows, joined by a space. A quiet stage, or one
 * not recognised, says nothing, and a step with nothing to say is an empty
 * line, so line i is step i */
void out_render_plain(const Analysis *analyses, const SepKind *seps,
                      const bool *quiet, const bool *joins,
                      char clauses[][256], int n);

/* "sheer gen": show the generated command then its risk analysis */
void out_render_gen(const char *description, const char *generated,
                    const Analysis *a, bool color);
