#pragma once

#include <stddef.h>
#include "token.h"

/* risk levels. CRITICAL means "you will regret this" */
typedef enum {
    RISK_SAFE     = 0,
    RISK_LOW      = 1,
    RISK_MEDIUM   = 2,
    RISK_HIGH     = 3,
    RISK_CRITICAL = 4,
} Risk;

typedef struct {
    char explanation[1024];
    char warning[512];
    char safer[512];
    char source[32];    /* "local rules" | "llm (model)" */
    Risk risk;
    bool matched;
} Analysis;

static inline Risk risk_max(Risk a, Risk b) { return a > b ? a : b; }

void cmd_analyze(const TokList *tl, Analysis *out);

/* --plain: a pipe stage that only shapes what the stage before it prints
 * (head, tail, grep, wc, sort, uniq, cut, tee, a bare cat) as a clause for
 * that stage's sentence (", showing the first 20 lines"); false when the
 * stage stands on its own, like a head of a file */
bool cmd_clause(const TokList *tl, char *out, size_t len);

/* --plain: a stage worth no sentence: cd, sleep, true, :, set, and an echo
 * of a separator ("---", "=== tests ==="); --plain keeps a command that is
 * only that */
bool cmd_quiet(const TokList *tl);
