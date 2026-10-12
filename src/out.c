#include "out.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define A_RESET  "\033[0m"
#define A_BOLD   "\033[1m"
#define A_DIM    "\033[2m"
#define A_GREEN  "\033[32m"
#define A_YELLOW "\033[33m"
#define A_RED    "\033[31m"
#define A_BRED   "\033[1;31m"
#define A_CYAN   "\033[36m"
#define A_GRAY   "\033[90m"
#define A_WHITE  "\033[97m"

#define WRAP_COL  76   /* terminals from 1978 had 80 cols. we respect that */
#define LABEL_W   10

static const char *risk_color(Risk r)
{
    switch (r) {
    case RISK_SAFE:     return A_GREEN;
    case RISK_LOW:      return A_GREEN;
    case RISK_MEDIUM:   return A_YELLOW;
    case RISK_HIGH:     return A_RED;
    case RISK_CRITICAL: return A_BRED;
    }
    return A_RESET;
}

static const char *risk_label(Risk r)
{
    switch (r) {
    case RISK_SAFE:     return "SAFE";
    case RISK_LOW:      return "LOW";
    case RISK_MEDIUM:   return "MODERATE";
    case RISK_HIGH:     return "HIGH";
    case RISK_CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

static const char *risk_icon(Risk r)
{
    switch (r) {
    case RISK_SAFE:
    case RISK_LOW:      return "◆";
    case RISK_MEDIUM:   return "▲";
    case RISK_HIGH:     return "▲";
    case RISK_CRITICAL: return "✖";
    }
    return " ";
}

static void wrap_text(const char *text, int start_col, int indent, int width)
{
    char ibuf[64] = "";
    int  ilen = indent < 63 ? indent : 63;
    memset(ibuf, ' ', (size_t)ilen);
    ibuf[ilen] = '\0';

    int col = start_col;
    const char *p = text;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        const char *word = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        int wlen = (int)(p - word);

        if (col > indent && col + 1 + wlen > width) {
            printf("\n%s", ibuf);
            col = indent;
        } else if (col > indent) {
            putchar(' ');
            col++;
        }

        printf("%.*s", wlen, word);
        col += wlen;
    }
}

static void print_row(const char *label, const char *value,
                      const char *label_color, const char *value_color,
                      bool color)
{
    if (!value || value[0] == '\0') return;

    int label_pad = LABEL_W - (int)strlen(label);
    if (label_pad < 1) label_pad = 1;

    if (color) printf("%s", label_color);
    printf("  %s", label);
    if (color) printf("%s", A_RESET);
    printf("%*s", label_pad, "");

    int start_col = 2 + (int)strlen(label) + label_pad;
    if (color) printf("%s", value_color);
    wrap_text(value, start_col, 2 + LABEL_W, WRAP_COL);
    if (color) printf("%s", A_RESET);
    putchar('\n');
}

void out_render_pipeline(const char **stages, const Analysis *analyses,
                         const SepKind *seps, int n, Risk overall, bool color)
{
    const char *B  = color ? A_BOLD  : "";
    const char *R  = color ? A_RESET : "";
    const char *DM = color ? A_DIM   : "";

    putchar('\n');

    for (int i = 0; i < n; i++) {
        if (color) printf(A_CYAN);
        printf("  [%d/%d]", i + 1, n);
        if (color) printf(A_RESET);
        printf("  %s%s%s\n", B, stages[i], R);

        const Analysis *a = &analyses[i];
        if (a->matched) {
            printf("         ");
            wrap_text(a->explanation, 9, 9, WRAP_COL);
            putchar('\n');

            const char *rc = color ? risk_color(a->risk) : "";
            const char *rs = color ? A_RESET : "";
            printf("         %srisk%s  %s%s %s%s\n",
                   color ? A_CYAN : "", R,
                   rc, risk_icon(a->risk), risk_label(a->risk), rs);

            if (a->warning[0]) {
                printf("         ");
                if (color) printf(A_RED);
                wrap_text(a->warning, 9, 9, WRAP_COL);
                if (color) printf(A_RESET);
                putchar('\n');
            }
        } else {
            printf("         %s(not recognised - try --llm for this stage)%s\n", DM, R);
        }

        if (i < n - 1) {
            /* connector - pipe, &&, ||, or ; */
            const char *lbl = sep_label(seps[i + 1]);
            if (color) printf(A_DIM);
            if (seps[i + 1] == SEP_PIPE)
                printf("           │\n");
            else
                printf("           %s\n", lbl);
            if (color) printf(A_RESET);
        }
        putchar('\n');
    }

    /* the verdict for the whole thing */
    const char *rc = color ? risk_color(overall) : "";
    const char *rs = color ? A_RESET : "";
    if (color) printf(A_BOLD);
    printf("  pipeline  ");
    if (color) printf(A_RESET);
    printf("%s%s %s%s\n\n", rc, risk_icon(overall), risk_label(overall), rs);
}

void out_render_gen(const char *description, const char *generated,
                    const Analysis *a, bool color)
{
    const char *B = color ? A_BOLD  : "";
    const char *R = color ? A_RESET : "";
    const char *G = color ? A_GRAY  : "";

    putchar('\n');
    if (color) printf(A_DIM);
    printf("  %s\n", description);
    if (color) printf(A_RESET);

    if (!generated || generated[0] == '\0') {
        printf("\n  %s(no command generated - LLM failed or not configured)%s\n\n",
               G, R);
        return;
    }

    printf("\n  %s→%s  %s%s%s\n", B, R, B, generated, R);

    if (a->matched) {
        putchar('\n');
        printf("  ");
        wrap_text(a->explanation, 2, 2, WRAP_COL);
        putchar('\n');
        putchar('\n');

        const char *rc = color ? risk_color(a->risk) : "";
        const char *rs = color ? A_RESET : "";
        if (color) printf(A_CYAN);
        printf("  risk      ");
        if (color) printf(A_RESET);
        printf("%s%s %s%s\n", rc, risk_icon(a->risk), risk_label(a->risk), rs);

        if (a->warning[0])
            print_row("warning", a->warning,
                      color ? A_RED : "", color ? A_RED : "", color);
        if (a->safer[0])
            print_row("safer", a->safer,
                      color ? A_GREEN : "", color ? A_GREEN : "", color);
        if (a->source[0])
            print_row("source", a->source,
                      color ? A_GRAY : "", color ? A_GRAY : "", color);
    }
    putchar('\n');
}

void out_render_plain(const Analysis *analyses, const SepKind *seps,
                      const bool *quiet, const bool *joins,
                      char clauses[][256], int n)
{
    char line[2048] = "";   /* this step's line, so far */
    for (int i = 0; i < n; i++) {
        if (i > 0 && seps[i] != SEP_PIPE) { printf("%s\n", line); line[0] = '\0'; }
        size_t l = strlen(line);
        if (quiet[i]) continue;
        if (l && joins[i] && seps[i] == SEP_PIPE) {
            if (line[l - 1] == '.') line[--l] = '\0';
            snprintf(line + l, sizeof line - l, "%s.", clauses[i]);
            continue;
        }
        if (!analyses[i].matched) continue;
        if (l && l + 1 < sizeof line) line[l++] = ' ';
        for (const char *c = analyses[i].explanation; *c && l + 1 < sizeof line; c++)
            line[l++] = *c == '\n' ? ' ' : *c;
        line[l] = '\0';
    }
    printf("%s\n", line);
}

void out_render(const Analysis *a, const char *raw_cmd, bool color)
{
    putchar('\n');

    if (color) printf(A_BOLD);
    printf("  %s", raw_cmd);
    if (color) printf(A_RESET);
    putchar('\n');

    /* dashes under the command name */
    int rule_len = 2 + (int)strlen(raw_cmd);
    if (rule_len > WRAP_COL) rule_len = WRAP_COL;
    if (color) printf(A_DIM);
    printf("  ");
    for (int i = 0; i < rule_len; i++) putchar('-');
    if (color) printf(A_RESET);
    putchar('\n');

    if (!a->matched) {
        putchar('\n');
        if (color) printf(A_GRAY);
        printf("  Command not recognised.");
        if (color) printf(A_RESET);
        printf("  Use --llm to enable AI fallback.\n\n");
        return;
    }

    putchar('\n');
    printf("  ");
    wrap_text(a->explanation, 2, 2, WRAP_COL);
    putchar('\n');
    putchar('\n');

    {
        const char *rc = color ? risk_color(a->risk) : "";
        const char *rs = color ? A_RESET : "";

        if (color) printf(A_CYAN);
        printf("  risk      ");
        if (color) printf(A_RESET);

        printf("%s%s %s%s\n",
               rc, risk_icon(a->risk), risk_label(a->risk), rs);
    }

    if (a->warning[0])
        print_row("warning", a->warning,
                  color ? A_RED   : "", color ? A_RED   : "", color);

    if (a->safer[0])
        print_row("safer", a->safer,
                  color ? A_GREEN : "", color ? A_GREEN : "", color);

    if (a->source[0])
        print_row("source", a->source,
                  color ? A_GRAY  : "", color ? A_GRAY  : "", color);

    putchar('\n');
}
