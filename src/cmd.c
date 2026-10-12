#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── what a command acts on ─────────────────────────────────────────────── */

/*
 * the shell's redirections, taken off the token list before an analyzer
 * sees it: `2>&1` and `> out.txt` are not arguments. where stdout goes is
 * kept so the sentence can name it; an analyzer that names it itself
 * (cat into a file) sets `used`.
 */
typedef struct {
    char out[256];  /* where stdout goes, "" for the terminal */
    bool append;
    bool heredoc;   /* stdin is a here-doc */
    bool used;
} Redir;

static Redir g_redir;
static int   g_sub;         /* token index of a compound command's subcommand */
static char  g_gitdir[256]; /* git -C <dir> */

/* a redirection token: [N|&]>[>][&N|target], <target, <<WORD, <<<word.
 * returns how many tokens it spans (1 or 2), 0 if it isn't one. */
static int redir_take(const TokList *tl, int i, Redir *r)
{
    const char *s  = tl->t[i].s;
    const char *p  = s;
    bool        fd1 = true;

    if (tl->t[i].type != TT_WORD) return 0;
    if (*p == '&') p++;
    else if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') p++;
        fd1 = (p - s == 1 && s[0] == '1');
    }
    if (*p == '>') {
        bool append = p[1] == '>';
        p += append ? 2 : 1;
        if (*p == '&' && p[1] >= '0' && p[1] <= '9') return 1;  /* 2>&1 */
        int         span   = 1;
        const char *target = p;
        if (!*p) {
            if (i + 1 >= tl->n) return 1;
            target = tl->t[i + 1].s;
            span   = 2;
        }
        if (fd1 && strcmp(target, "/dev/null") != 0) {
            snprintf(r->out, sizeof r->out, "%s", target);
            r->append = append;
        }
        return span;
    }
    if (p != s) return 0;  /* digits without a > are just a word */
    if (*p == '<') {
        if (p[1] == '<' && p[2] != '<') {
            r->heredoc = true;
            const char *d = p + 2;
            if (*d == '-') d++;
            return (*d || i + 1 >= tl->n) ? 1 : 2;
        }
        if (p[1] == '<') return p[3] ? 1 : 2;  /* <<<word */
        return (p[1] || i + 1 >= tl->n) ? 1 : 2;
    }
    return 0;
}

static void redir_strip(const TokList *in, TokList *out, Redir *r)
{
    memset(r, 0, sizeof *r);
    out->n  = 0;
    out->dd = -1;
    for (int i = 0; i < in->n; i++) {
        if (i == in->dd) out->dd = out->n;
        int span = i > 0 ? redir_take(in, i, r) : 0;
        if (span) { i += span - 1; continue; }
        out->t[out->n++] = in->t[i];
    }
}

/*
 * indexes of the positional words from token `from` on, skipping the value
 * that follows a flag which takes one: short flags by letter in `vshort`
 * ("nABC"), long ones in `vlong` (NULL-terminated, without "="). a flag's
 * value attached to it (-n5, --lines=5) is no word to skip.
 */
static int words_of(const TokList *tl, int from, const char *vshort,
                    const char *const *vlong, int *idx, int max)
{
    int n = 0;
    for (int i = from; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type == TT_WORD) {
            if (t->s[0] && n < max) idx[n++] = i;
            continue;
        }
        if (tl->dd >= 0 && i >= tl->dd) continue;
        bool takes = false;
        if (t->type == TT_FLAG_SHORT && vshort && strlen(t->s) <= 5 &&
            strchr(vshort, t->s[strlen(t->s) - 1]) &&
            !(t->s[1] >= '0' && t->s[1] <= '9'))
            takes = true;
        if (t->type == TT_FLAG_LONG && vlong && !strchr(t->s, '='))
            for (int k = 0; vlong[k]; k++)
                if (strcmp(t->s, vlong[k]) == 0) takes = true;
        if (takes) i++;
    }
    return n;
}

/* the k-th positional word (from 0), past the flags that take a value */
static const Tok *word_of(const TokList *tl, const char *vshort,
                          const char *const *vlong, int k)
{
    int w[8];
    int n = words_of(tl, 1, vshort, vlong, w, 8);
    return k < n ? &tl->t[w[k]] : NULL;
}

/* "a", "a and b", "a, b and c", "a, b and 3 more files" */
static void list_of(const TokList *tl, const int *idx, int n, char *buf, size_t len)
{
    buf[0] = '\0';
    if (n == 0) return;
    if (n == 1) { snprintf(buf, len, "%s", tl->t[idx[0]].s); return; }
    if (n == 2) {
        snprintf(buf, len, "%s and %s", tl->t[idx[0]].s, tl->t[idx[1]].s);
        return;
    }
    if (n == 3) {
        snprintf(buf, len, "%s, %s and %s",
                 tl->t[idx[0]].s, tl->t[idx[1]].s, tl->t[idx[2]].s);
        return;
    }
    snprintf(buf, len, "%s, %s and %d more files",
             tl->t[idx[0]].s, tl->t[idx[1]].s, n - 2);
}

/* the value of a count flag: -n 5, -n5, -5, --lines=5, --lines 5 */
static const char *count_of(const TokList *tl, char letter, const char *lname)
{
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (tl->dd >= 0 && i >= tl->dd) break;
        if (t->type == TT_FLAG_SHORT) {
            if (t->s[1] == letter && t->s[2] == '\0' && i + 1 < tl->n)
                return tl->t[i + 1].s;
            if (t->s[1] == letter && t->s[2])
                return t->s + 2;
            if (letter == 'n' && t->s[1] >= '0' && t->s[1] <= '9')
                return t->s + 1;
        }
    }
    return lname ? tok_long_val(tl, lname) : NULL;
}

/* the value of -X: attached (-Sfoo) or the next token (-S foo) */
static const char *short_val(const TokList *tl, char letter)
{
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_FLAG_SHORT || t->s[1] != letter) continue;
        if (t->s[2]) return t->s + 2;
        if (i + 1 < tl->n) return tl->t[i + 1].s;
    }
    return NULL;
}

/* "its input" when a command names no file */
static const char *files_or_input(const TokList *tl, const int *idx, int n,
                                  char *buf, size_t len)
{
    if (n == 0) return "its input";
    list_of(tl, idx, n, buf, len);
    return buf;
}

/*
 * caveat: we only know the string, not the actual filesystem. /tmp/foo
 * could be a symlink to /etc/shadow and we'd happily say "LOW". not our
 * problem. if you're being socially-engineered via path names, sheer is
 * not your biggest issue.
 */
static Risk path_risk(const char *path)
{
    if (!path || path[0] == '\0')
        return RISK_SAFE;

    if (strcmp(path, "/")   == 0 ||
        strcmp(path, "/*")  == 0 ||
        strcmp(path, "/.")  == 0)
        return RISK_CRITICAL;

    static const char *const sys_dirs[] = {
        "/bin", "/boot", "/dev", "/etc", "/lib", "/lib32",
        "/lib64", "/libx32", "/proc", "/root", "/run",
        "/sbin", "/srv", "/sys", "/usr", "/var", "/home",
        NULL
    };
    for (int i = 0; sys_dirs[i]; i++) {
        size_t len = strlen(sys_dirs[i]);
        if (strncmp(path, sys_dirs[i], len) == 0 &&
            (path[len] == '\0' || path[len] == '/' || path[len] == '*'))
            return RISK_HIGH;
    }

    if (strchr(path, '*') || strchr(path, '?'))
        return RISK_MEDIUM;  /* wildcards are spicy */

    if (strncmp(path, "/tmp", 4) == 0)
        return RISK_LOW;

    return RISK_SAFE;
}

static const char *sig_desc(int sig)
{
    switch (sig) {
    case  1: return "SIGHUP (hangup / reload)";
    case  2: return "SIGINT (interrupt, like Ctrl-C)";
    case  3: return "SIGQUIT (quit with core dump)";
    case  6: return "SIGABRT (abort)";
    case  9: return "SIGKILL (force-kill, unblockable)";
    case 11: return "SIGSEGV (segmentation fault)";
    case 15: return "SIGTERM (graceful termination)";
    case 17: return "SIGCHLD (child status changed)";
    case 18: return "SIGCONT (resume)";
    case 19: return "SIGSTOP (pause, unblockable)";
    default: return "a signal";
    }
}

static void chmod_octal_desc(const char *mode_str, char *buf, size_t len)
{
    char *end;
    long m = strtol(mode_str, &end, 8);
    if (end == mode_str || *end != '\0') {
        snprintf(buf, len, "permissions %s", mode_str);
        return;
    }

    static const char *const rwx[8] = {
        "---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"
    };
    int o = (int)((m >> 6) & 7);
    int g = (int)((m >> 3) & 7);
    int ot = (int)(m & 7);

    if (m == 0777)
        snprintf(buf, len, "full read/write/execute for all users (777)");
    else if (m == 0755)
        snprintf(buf, len, "owner rwx, group/others r-x (755)");
    else if (m == 0644)
        snprintf(buf, len, "owner rw-, others r-- (644)");
    else if (m == 0600)
        snprintf(buf, len, "owner rw- only, no access for others (600)");
    else if (m == 0400)
        snprintf(buf, len, "owner read-only, no access for others (400)");
    else
        snprintf(buf, len, "owner: %s, group: %s, others: %s (%s)",
                 rwx[o], rwx[g], rwx[ot], mode_str);
}

static void analyze_rm(const TokList *tl, Analysis *out)
{
    bool recursive = tok_has_short(tl, 'r') || tok_has_short(tl, 'R') ||
                     tok_has_long(tl, "--recursive");
    bool force     = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");
    bool inter     = tok_has_short(tl, 'i') || tok_has_short(tl, 'I') ||
                     tok_has_long(tl, "--interactive");

    char target[256] = "(unspecified)";
    Risk path_r = RISK_SAFE;
    int  nargs  = 0;

    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_WORD) continue;
        if (nargs == 0) snprintf(target, sizeof target, "%s", t->s);
        path_r = risk_max(path_r, path_risk(t->s));
        nargs++;
    }
    if (nargs > 1)
        snprintf(target, sizeof target, "%d paths", nargs);

    if (recursive && force)
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes '%s' and everything inside it, no questions asked.", target);
    else if (recursive)
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes '%s' and everything inside it.", target);
    else if (force)
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes file '%s' without prompting, ignores missing files.", target);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes file '%s'.", target);

    out->risk = RISK_LOW;
    if (recursive) out->risk = risk_max(out->risk, RISK_MEDIUM);
    if (force)     out->risk = risk_max(out->risk, RISK_MEDIUM);
    if (recursive && force && !inter) out->risk = risk_max(out->risk, RISK_HIGH);
    out->risk = risk_max(out->risk, path_r);

    if (path_r == RISK_CRITICAL)
        snprintf(out->warning, sizeof out->warning,
                 "Targeting a critical system path - this will destroy the OS.");
    else if (out->risk >= RISK_HIGH)
        snprintf(out->warning, sizeof out->warning,
                 "Deletion is irreversible. Files cannot be recovered.");

    if (force && !inter && recursive)
        snprintf(out->safer, sizeof out->safer,
                 "rm -ri %s  (prompts before each deletion)", target);
}

static void analyze_mv(const TokList *tl, Analysis *out)
{
    bool force     = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");
    bool no_clob   = tok_has_short(tl, 'n') || tok_has_long(tl, "--no-clobber");

    char src[256] = "(source)", dst[256] = "(destination)";
    int nargs = 0;
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_WORD) continue;
        if      (nargs == 0) snprintf(src, sizeof src, "%s", t->s);
        else if (nargs == 1) snprintf(dst, sizeof dst, "%s", t->s);
        nargs++;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Moves %s to %s%s.",
             src, dst,
             force ? " (overwrites destination without prompt)" : "");

    out->risk = RISK_LOW;
    if (force && !no_clob) out->risk = risk_max(out->risk, RISK_MEDIUM);
    out->risk = risk_max(out->risk, path_risk(dst));

    if (path_risk(dst) >= RISK_HIGH)
        snprintf(out->warning, sizeof out->warning,
                 "Destination is in a system-critical path.");
}

static void analyze_cp(const TokList *tl, Analysis *out)
{
    bool recursive = tok_has_short(tl, 'r') || tok_has_short(tl, 'R') ||
                     tok_has_long(tl, "--recursive");
    bool force     = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");
    bool no_clob   = tok_has_short(tl, 'n') || tok_has_long(tl, "--no-clobber");

    char src[256] = "(source)", dst[256] = "(destination)";
    int nargs = 0;
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_WORD) continue;
        if      (nargs == 0) snprintf(src, sizeof src, "%s", t->s);
        else if (nargs == 1) snprintf(dst, sizeof dst, "%s", t->s);
        nargs++;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Copies %s to %s%s%s.",
             src, dst,
             recursive ? " recursively" : "",
             (force && !no_clob) ? " (overwrites existing files)" : "");

    out->risk = RISK_SAFE;
    if (force && !no_clob) out->risk = risk_max(out->risk, RISK_LOW);
    out->risk = risk_max(out->risk, path_risk(dst));
}

static void analyze_chmod(const TokList *tl, Analysis *out)
{
    bool recursive = tok_has_short(tl, 'R') || tok_has_long(tl, "--recursive");

    const Tok *mode_tok = tok_first_word(tl, 1);
    const Tok *path_tok = mode_tok
        ? tok_first_word(tl, (int)(mode_tok - tl->t) + 1)
        : NULL;

    char mode_desc[128] = "permissions";
    if (mode_tok) chmod_octal_desc(mode_tok->s, mode_desc, sizeof mode_desc);

    char target[256] = "the specified path";
    Risk path_r = RISK_SAFE;
    if (path_tok) {
        snprintf(target, sizeof target, "%s", path_tok->s);
        path_r = path_risk(path_tok->s);
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Sets %s on %s%s.",
             mode_desc, target,
             recursive ? " and all its contents" : "");

    out->risk = RISK_LOW;

    if (mode_tok) {
        char *end;
        long m = strtol(mode_tok->s, &end, 8);
        if (end != mode_tok->s && *end == '\0') {
            /* world-writable or world-executable on sensitive paths */
            if ((m & 0002) || m == 0777) {
                out->risk = risk_max(out->risk, RISK_HIGH);
                snprintf(out->warning, sizeof out->warning,
                         "World-writable permissions are a serious security risk.");
                snprintf(out->safer, sizeof out->safer,
                         "chmod 755 %s  (owner full, others read/execute)", target);
            }
        }
    }

    out->risk = risk_max(out->risk, path_r);
    if (recursive) out->risk = risk_max(out->risk, RISK_MEDIUM);
}

static void analyze_chown(const TokList *tl, Analysis *out)
{
    bool recursive = tok_has_short(tl, 'R') || tok_has_long(tl, "--recursive");

    const Tok *owner_tok = tok_first_word(tl, 1);
    const Tok *path_tok  = owner_tok
        ? tok_first_word(tl, (int)(owner_tok - tl->t) + 1)
        : NULL;

    char target[256] = "the specified path";
    Risk path_r = RISK_SAFE;
    if (path_tok) {
        snprintf(target, sizeof target, "%s", path_tok->s);
        path_r = path_risk(path_tok->s);
    }

    char owner[64] = "a new owner";
    if (owner_tok) snprintf(owner, sizeof owner, "%s", owner_tok->s);

    snprintf(out->explanation, sizeof out->explanation,
             "Changes ownership of %s to %s%s.",
             target, owner, recursive ? " recursively" : "");

    out->risk = RISK_LOW;
    out->risk = risk_max(out->risk, path_r);
    if (recursive) out->risk = risk_max(out->risk, RISK_MEDIUM);
    if (owner_tok &&
        (strcmp(owner_tok->s, "root") == 0 || strcmp(owner_tok->s, "root:root") == 0))
        out->risk = risk_max(out->risk, RISK_HIGH);
}

static void analyze_ln(const TokList *tl, Analysis *out)
{
    bool symbolic = tok_has_short(tl, 's') || tok_has_long(tl, "--symbolic");
    bool force    = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");

    char tgt[256]  = "(target)";
    char link[256] = "(link name)";
    int nargs = 0;
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_WORD) continue;
        if      (nargs == 0) snprintf(tgt,  sizeof tgt,  "%s", t->s);
        else if (nargs == 1) snprintf(link, sizeof link, "%s", t->s);
        nargs++;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Creates a %s link at %s pointing to %s%s.",
             symbolic ? "symbolic" : "hard", link, tgt,
             force ? " (overwrites existing)" : "");

    out->risk = force ? RISK_LOW : RISK_SAFE;
}

static void analyze_find(const TokList *tl, Analysis *out)
{
    bool has_delete = false;
    bool has_exec   = false;
    bool exec_rm    = false;

    /*
     * NOTE: we now split pipelines in main.c before reaching here, so when
     * we see "find /var/log -name '*.log'", the "| xargs rm" part is gone.
     * sheer scores each stage independently and aggregates at the end.
     * so this function only sees one stage at a time now - good.
     */
    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (strcmp(s, "-delete") == 0) { has_delete = true; continue; }
        if (strcmp(s, "-exec") == 0 || strcmp(s, "-execdir") == 0) {
            has_exec = true;
            if (i + 1 < tl->n) {
                const char *cmd = tl->t[i + 1].s;
                /* strip path prefix */
                const char *sl = strrchr(cmd, '/');
                if (sl) cmd = sl + 1;
                if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "shred") == 0)
                    exec_rm = true;
            }
        }
    }

    /* Search root: first WORD after the command */
    const Tok *root = tok_first_word(tl, 1);
    char root_desc[256] = "the current directory";
    Risk path_r = RISK_SAFE;
    if (root) {
        snprintf(root_desc, sizeof root_desc, "%s", root->s);
        path_r = path_risk(root->s);
    }

    /* Name pattern */
    char name_desc[128] = "files";
    for (int i = 1; i < tl->n - 1; i++) {
        if (strcmp(tl->t[i].s, "-name") == 0 ||
            strcmp(tl->t[i].s, "-iname") == 0) {
            snprintf(name_desc, sizeof name_desc,
                     "files matching '%s'", tl->t[i + 1].s);
            break;
        }
    }

    if (has_delete || exec_rm) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Finds and permanently deletes %s under %s.",
                 name_desc, root_desc);
        out->risk = risk_max(RISK_HIGH, path_r);
        snprintf(out->warning, sizeof out->warning,
                 "Deletion via find is irreversible.");
        snprintf(out->safer, sizeof out->safer,
                 "Replace -delete with -print first to preview matches.");
    } else if (has_exec) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Finds %s under %s and executes a command on each match.",
                 name_desc, root_desc);
        out->risk = risk_max(RISK_MEDIUM, path_r);
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Searches for %s under %s.", name_desc, root_desc);
        /* read-only search. cap at MEDIUM even for /etc: it's just traversal,
         * not modification. HIGH would be alarmist and annoying. */
        Risk cap = path_r > RISK_MEDIUM ? RISK_MEDIUM : path_r;
        out->risk = cap;
    }
}

static void analyze_tar(const TokList *tl, Analysis *out)
{
    bool create   = tok_has_short(tl, 'c');
    bool extract  = tok_has_short(tl, 'x');
    bool list     = tok_has_short(tl, 't');
    bool gzip     = tok_has_short(tl, 'z');
    bool bzip2    = tok_has_short(tl, 'j');
    bool xz       = tok_has_short(tl, 'J');
    bool keep_old = tok_has_short(tl, 'k') || tok_has_long(tl, "--keep-old-files");

    const char *archive = tok_short_val(tl, 'f');
    if (!archive) archive = tok_long_val(tl, "--file");

    char comp[32] = "";
    if (gzip)  snprintf(comp, sizeof comp, "gzip-compressed ");
    if (bzip2) snprintf(comp, sizeof comp, "bzip2-compressed ");
    if (xz)    snprintf(comp, sizeof comp, "xz-compressed ");

    char arc_desc[256] = "an archive";
    if (archive) snprintf(arc_desc, sizeof arc_desc, "%s%s", comp, archive);

    if (create) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates %s.", arc_desc);
        out->risk = RISK_LOW;
    } else if (extract) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Extracts %s%s.",
                 arc_desc,
                 keep_old ? " (skips existing files)" : " (may overwrite existing files)");
        out->risk = RISK_LOW;
        if (!keep_old)
            snprintf(out->safer, sizeof out->safer,
                     "Add -k to preserve existing files, or tar -tf %s to preview.",
                     archive ? archive : "archive");
    } else if (list) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists contents of %s.", arc_desc);
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Operates on archive %s.", arc_desc);
        out->risk = RISK_LOW;
    }
}

static void analyze_kill(const TokList *tl, Analysis *out)
{
    int sig = 15; /* default SIGTERM */

    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_FLAG_SHORT) continue;
        const char *s = t->s + 1;
        if (*s >= '0' && *s <= '9') { sig = atoi(s); break; }
        /* -SIGKILL, -TERM etc. - leave at default for brevity */
    }

    char pid_desc[128] = "the specified process";
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) {
            snprintf(pid_desc, sizeof pid_desc, "PID %s", tl->t[i].s);
            break;
        }
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Sends %s to %s.", sig_desc(sig), pid_desc);

    out->risk = (sig == 9 || sig == 19) ? RISK_HIGH : RISK_MEDIUM;
    if (sig == 9)
        snprintf(out->warning, sizeof out->warning,
                 "SIGKILL cannot be caught or ignored - the process dies immediately "
                 "without any cleanup.");
}

static void analyze_pkill(const TokList *tl, Analysis *out)
{
    int sig = 15;
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type == TT_FLAG_SHORT) {
            const char *s = t->s + 1;
            if (*s >= '0' && *s <= '9') { sig = atoi(s); break; }
        }
    }

    char pattern[128] = "(pattern)";
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) {
            snprintf(pattern, sizeof pattern, "%s", tl->t[i].s);
            break;
        }
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Sends %s to all processes whose name matches '%s'.",
             sig_desc(sig), pattern);

    out->risk = (sig == 9) ? RISK_HIGH : RISK_MEDIUM;
    if (sig == 9)
        snprintf(out->warning, sizeof out->warning,
                 "SIGKILL cannot be caught - all matched processes die immediately.");
}

static void analyze_killall(const TokList *tl, Analysis *out)
{
    int sig = 15;
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type == TT_FLAG_SHORT) {
            const char *s = t->s + 1;
            if (*s >= '0' && *s <= '9') { sig = atoi(s); break; }
        }
    }

    char name[128] = "(name)";
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) {
            snprintf(name, sizeof name, "%s", tl->t[i].s);
            break;
        }
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Sends %s to all processes named '%s'.", sig_desc(sig), name);

    out->risk = (sig == 9) ? RISK_HIGH : RISK_MEDIUM;
    if (sig == 9)
        snprintf(out->warning, sizeof out->warning,
                 "SIGKILL cannot be caught - all matched processes die immediately.");
}

static void analyze_dd(const TokList *tl, Analysis *out)
{
    const char *inf  = NULL;
    const char *outf = NULL;

    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (strncmp(s, "if=", 3) == 0) inf  = s + 3;
        if (strncmp(s, "of=", 3) == 0) outf = s + 3;
    }

    char src[256] = "input";
    char dst[256] = "output";
    if (inf)  snprintf(src, sizeof src, "%s", inf);
    if (outf) snprintf(dst, sizeof dst, "%s", outf);

    bool to_dev   = outf && strncmp(outf, "/dev/", 5) == 0;
    bool from_zero = inf && strcmp(inf, "/dev/zero") == 0;
    bool from_rnd  = inf && (strcmp(inf, "/dev/urandom") == 0 ||
                              strcmp(inf, "/dev/random")  == 0);

    if (to_dev && (from_zero || from_rnd)) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Overwrites device %s with %s - effectively wiping it.",
                 dst, from_rnd ? "random data (secure erase)" : "zeros");
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "ALL data on %s will be permanently and unrecoverably destroyed.", dst);
    } else if (to_dev) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Writes raw data from %s directly to device %s.", src, dst);
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "Writing directly to a block device bypasses the filesystem - "
                 "typically irreversible.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Copies raw data from %s to %s.", src, dst);
        out->risk = RISK_MEDIUM;
        out->risk = risk_max(out->risk, path_risk(dst));
    }
}

static void analyze_mkfs(const TokList *tl, Analysis *out)
{
    const char *cmd = tl->n > 0 ? tl->t[0].s : "mkfs";
    const char *dot = strchr(cmd, '.');

    char fstype[32] = "a new filesystem";
    if (dot) snprintf(fstype, sizeof fstype, "%s filesystem", dot + 1);

    const Tok *dev = tok_first_word(tl, 1);
    char dev_desc[256] = "the specified device";
    if (dev) snprintf(dev_desc, sizeof dev_desc, "%s", dev->s);

    snprintf(out->explanation, sizeof out->explanation,
             "Formats %s as %s, erasing all existing data.",
             dev_desc, fstype);

    out->risk = RISK_CRITICAL;
    snprintf(out->warning, sizeof out->warning,
             "ALL data on %s will be permanently destroyed. This cannot be undone.",
             dev_desc);
}

static void analyze_git_reset(const TokList *tl, Analysis *out)
{
    bool hard  = tok_has_long(tl, "--hard");
    bool soft  = tok_has_long(tl, "--soft");

    char ref[128] = "the current HEAD";
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type == TT_WORD && strcmp(t->s, "reset") != 0) {
            snprintf(ref, sizeof ref, "%s", t->s);
            break;
        }
    }

    if (hard) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Moves HEAD to %s and discards ALL uncommitted changes "
                 "(staged and unstaged).", ref);
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Unstaged and staged changes are permanently lost.");
        snprintf(out->safer, sizeof out->safer,
                 "git stash  (preserve changes before resetting)");
    } else if (soft) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Moves HEAD to %s, keeping all changes staged for commit.", ref);
        out->risk = RISK_LOW;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Moves HEAD to %s, unstaging changes while keeping them "
                 "in the working tree (default --mixed).", ref);
        out->risk = RISK_LOW;
    }
}

static void analyze_git_clean(const TokList *tl, Analysis *out)
{
    bool force   = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");
    bool dirs    = tok_has_short(tl, 'd');
    bool dry_run = tok_has_short(tl, 'n') || tok_has_long(tl, "--dry-run");
    bool ignored = tok_has_short(tl, 'x');
    bool only_ig = tok_has_short(tl, 'X');

    if (dry_run) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows what would be removed (dry run - nothing is deleted).");
        out->risk = RISK_SAFE;
        return;
    }
    if (!force) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Would remove untracked files, but -f is required to execute.");
        out->risk = RISK_SAFE;
        return;
    }

    char what[256] = "untracked files";
    if (dirs && ignored)
        snprintf(what, sizeof what,
                 "untracked files, directories, and .gitignore-ignored files");
    else if (dirs)
        snprintf(what, sizeof what, "untracked files and directories");
    else if (ignored)
        snprintf(what, sizeof what,
                 "untracked files plus files ignored by .gitignore");
    else if (only_ig)
        snprintf(what, sizeof what, "files ignored by .gitignore only");

    snprintf(out->explanation, sizeof out->explanation,
             "Permanently removes %s from the working tree.", what);
    out->risk = RISK_HIGH;
    snprintf(out->warning, sizeof out->warning,
             "Removed files are untracked - git cannot recover them.");
    snprintf(out->safer, sizeof out->safer,
             "git clean -n  (preview what would be removed)");
}

static void analyze_git_checkout(const TokList *tl, Analysis *out)
{
    bool force      = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");
    bool new_branch = tok_has_short(tl, 'b') || tok_has_short(tl, 'B');

    char tgt[256] = "the specified branch or file";
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type == TT_WORD && strcmp(t->s, "checkout") != 0) {
            snprintf(tgt, sizeof tgt, "%s", t->s);
            break;
        }
    }

    if (new_branch) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates and switches to new branch '%s'.", tgt);
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Switches to %s%s.", tgt,
                 force ? " (discarding uncommitted changes)" : "");
        out->risk = force ? RISK_HIGH : RISK_LOW;
        if (force)
            snprintf(out->warning, sizeof out->warning,
                     "Uncommitted changes to checked-out files will be overwritten.");
    }
}

static void analyze_git_push(const TokList *tl, Analysis *out)
{
    bool force    = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");
    bool force_wl = tok_has_long(tl, "--force-with-lease");
    bool del      = tok_has_short(tl, 'd') || tok_has_long(tl, "--delete");
    bool tags     = tok_has_long(tl, "--tags");

    /* Skip subcommand word ("push") to reach arguments */
    static const char *const push_vlong[] = { "--push-option", "--repo", NULL };
    int w[2];
    int nw = words_of(tl, g_sub + 1, "o", push_vlong, w, 2);
    const Tok *remote = nw > 0 ? &tl->t[w[0]] : NULL;
    const Tok *branch = nw > 1 ? &tl->t[w[1]] : NULL;

    char remote_s[64] = "remote";
    char branch_s[64] = "current branch";
    if (remote) snprintf(remote_s, sizeof remote_s, "%s", remote->s);
    if (branch) snprintf(branch_s, sizeof branch_s, "%s", branch->s);

    if (del) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes branch '%s' from %s.", branch_s, remote_s);
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Remote branch deletion affects all collaborators.");
    } else if (force && !force_wl) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Force-pushes %s to %s, rewriting remote history.",
                 branch_s, remote_s);
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "Rewrites remote history. All collaborators must re-sync "
                 "(git pull --rebase or re-clone).");
        snprintf(out->safer, sizeof out->safer,
                 "git push --force-with-lease  (aborts if remote changed)");
    } else if (force_wl) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Force-pushes %s to %s only if no one else has pushed since "
                 "your last fetch.",
                 branch_s, remote_s);
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Still rewrites history, but guards against clobbering others' work.");
    } else if (tags) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Pushes %s and all local tags to %s.", branch_s, remote_s);
        out->risk = RISK_LOW;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Pushes %s to %s.", branch_s, remote_s);
        out->risk = RISK_SAFE;
    }
}

static void analyze_docker_run(const TokList *tl, Analysis *out)
{
    bool privileged = tok_has_long(tl, "--privileged");
    bool remove     = tok_has_long(tl, "--rm");
    bool detached   = tok_has_short(tl, 'd');
    bool net_host   = false;

    const char *netval = tok_long_val(tl, "--network");
    if (netval && strcmp(netval, "host") == 0) net_host = true;

    const char *name = tok_long_val(tl, "--name");

    const char *ports[8], *vols[8];
    int n_ports = 0, n_vols = 0;

    for (int i = 1; i < tl->n - 1; i++) {
        if (tl->t[i].type != TT_FLAG_SHORT) continue;
        size_t slen = strlen(tl->t[i].s);
        if (slen < 2) continue;
        char last = tl->t[i].s[slen - 1];
        if (last == 'p' && n_ports < 8)
            ports[n_ports++] = tl->t[i + 1].s;
        if (last == 'v' && n_vols < 8)
            vols[n_vols++] = tl->t[i + 1].s;
    }

    const char *other_vals[] = {
        tok_short_val(tl, 'e'),
        tok_short_val(tl, 'u'),
        tok_long_val(tl, "--network"),
        tok_long_val(tl, "--entrypoint"),
        tok_long_val(tl, "--hostname"),
        name,
    };

    const char *image = NULL;
    bool found_run = false;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type != TT_WORD) continue;
        if (!found_run) { found_run = true; continue; }
        const char *s = tl->t[i].s;
        bool is_val = false;
        for (int k = 0; k < n_ports && !is_val; k++)
            if (strcmp(s, ports[k]) == 0) is_val = true;
        for (int k = 0; k < n_vols && !is_val; k++)
            if (strcmp(s, vols[k]) == 0) is_val = true;
        for (size_t k = 0; k < sizeof other_vals / sizeof other_vals[0] && !is_val; k++)
            if (other_vals[k] && strcmp(s, other_vals[k]) == 0) is_val = true;
        if (!is_val) { image = s; break; }
    }

    int off = 0;
    if (image)
        off = snprintf(out->explanation, sizeof out->explanation,
                       "Runs a Docker container from image '%s'", image);
    else
        off = snprintf(out->explanation, sizeof out->explanation,
                       "Runs a Docker container");
    if (off < 0) off = 0;

    if (name && off < (int)sizeof out->explanation)
        off += snprintf(out->explanation + off, sizeof out->explanation - (size_t)off,
                        ", named '%s'", name);
    for (int i = 0; i < n_ports && off < (int)sizeof out->explanation; i++)
        off += snprintf(out->explanation + off, sizeof out->explanation - (size_t)off,
                        ", port %s exposed", ports[i]);
    for (int i = 0; i < n_vols && off < (int)sizeof out->explanation; i++)
        off += snprintf(out->explanation + off, sizeof out->explanation - (size_t)off,
                        ", volume %s mounted", vols[i]);
    if (detached && off < (int)sizeof out->explanation)
        off += snprintf(out->explanation + off, sizeof out->explanation - (size_t)off,
                        ", in background");
    if (remove && off < (int)sizeof out->explanation)
        off += snprintf(out->explanation + off, sizeof out->explanation - (size_t)off,
                        ", auto-removed on exit");
    if (off < (int)sizeof out->explanation)
        snprintf(out->explanation + off, sizeof out->explanation - (size_t)off, ".");

    out->risk = RISK_LOW;
    if (privileged) {
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "--privileged grants the container full host kernel access "
                 "(equivalent to running as root on the host).");
    } else if (net_host) {
        out->risk = risk_max(out->risk, RISK_HIGH);
        snprintf(out->warning, sizeof out->warning,
                 "--network host removes container network isolation.");
    }
    for (int i = 0; i < n_vols; i++) {
        char host_path[256];
        snprintf(host_path, sizeof host_path, "%s", vols[i]);
        char *colon = strchr(host_path, ':');
        if (colon) *colon = '\0';
        if (path_risk(host_path) >= RISK_HIGH)
            out->risk = risk_max(out->risk, RISK_HIGH);
    }
}

static void analyze_docker_rm(const TokList *tl, Analysis *out)
{
    bool force   = tok_has_short(tl, 'f') || tok_has_long(tl, "--force");
    bool volumes = tok_has_short(tl, 'v') || tok_has_long(tl, "--volumes");

    char ct[128] = "the specified container";
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD && strcmp(tl->t[i].s, "rm") != 0) {
            snprintf(ct, sizeof ct, "container '%s'", tl->t[i].s);
            break;
        }
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Removes %s%s%s.",
             ct,
             force   ? " (kills if still running)" : "",
             volumes ? " and its associated volumes" : "");

    out->risk = force ? RISK_HIGH : RISK_MEDIUM;
    if (volumes)
        snprintf(out->warning, sizeof out->warning,
                 "Volume data will be permanently deleted.");
}

static void analyze_docker_exec(const TokList *tl, Analysis *out)
{
    bool interactive = tok_has_short(tl, 'i') || tok_has_short(tl, 't');
    const char *user = tok_long_val(tl, "--user");
    if (!user) user  = tok_short_val(tl, 'u');
    bool as_root = user && (strcmp(user, "root") == 0 || strcmp(user, "0") == 0);

    char ct[128]  = "the container";
    char cmd[128] = "a command";
    int  wc = 0;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type != TT_WORD || strcmp(tl->t[i].s, "exec") == 0) continue;
        if      (wc == 0) snprintf(ct,  sizeof ct,  "container '%s'", tl->t[i].s);
        else if (wc == 1) snprintf(cmd, sizeof cmd, "'%s'",            tl->t[i].s);
        wc++;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Executes %s inside %s%s%s.",
             cmd, ct,
             interactive ? " interactively" : "",
             as_root     ? " as root"       : "");

    out->risk = interactive ? RISK_MEDIUM : RISK_LOW;
    if (as_root) out->risk = risk_max(out->risk, RISK_HIGH);
}

static void analyze_curl(const TokList *tl, Analysis *out)
{
    const char *method = tok_short_val(tl, 'X');
    if (!method) method = tok_long_val(tl, "--request");
    bool output = tok_has_short(tl, 'o') || tok_has_short(tl, 'O');
    bool silent = tok_has_short(tl, 's') || tok_has_long(tl, "--silent");
    bool data   = tok_has_short(tl, 'd') || tok_has_long(tl, "--data") ||
                  tok_has_long(tl, "--data-raw");

    char url[256] = "a URL";
    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0) {
            snprintf(url, sizeof url, "%s", s);
            break;
        }
    }

    char m[16] = "GET";
    if (method) snprintf(m, sizeof m, "%s", method);
    else if (data) snprintf(m, sizeof m, "POST");

    snprintf(out->explanation, sizeof out->explanation,
             "Sends an HTTP %s request to %s%s%s.",
             m, url,
             output ? ", saving the response to a file" : "",
             silent ? " (silent mode)" : "");

    out->risk = RISK_SAFE;
    if (strcmp(m, "DELETE") == 0) out->risk = RISK_HIGH;
    else if (strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 ||
             strcmp(m, "PATCH") == 0) out->risk = RISK_MEDIUM;
}

static void analyze_wget(const TokList *tl, Analysis *out)
{
    bool recursive = tok_has_short(tl, 'r') || tok_has_long(tl, "--recursive");
    const char *outfile = tok_short_val(tl, 'O');
    if (!outfile) outfile = tok_long_val(tl, "--output-document");

    char url[256] = "a URL";
    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0) {
            snprintf(url, sizeof url, "%s", s);
            break;
        }
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Downloads %s%s%s.",
             url,
             recursive ? " and all linked resources" : "",
             outfile   ? " to a specified file"       : "");

    out->risk = RISK_LOW;
    /* Piping to shell (wget -O - | sh) is dangerous but we can't detect
     * the pipe here - it would appear as a separate process */
}

static void analyze_rsync(const TokList *tl, Analysis *out)
{
    bool del     = tok_has_long(tl, "--delete") ||
                   tok_has_long(tl, "--delete-after") ||
                   tok_has_long(tl, "--delete-before");
    bool archive = tok_has_short(tl, 'a') || tok_has_long(tl, "--archive");
    bool dry_run = tok_has_short(tl, 'n') || tok_has_long(tl, "--dry-run");

    char src[256] = "source", dst[256] = "destination";
    int nargs = 0;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type != TT_WORD) continue;
        if      (nargs == 0) snprintf(src, sizeof src, "%s", tl->t[i].s);
        else if (nargs == 1) snprintf(dst, sizeof dst, "%s", tl->t[i].s);
        nargs++;
    }

    if (dry_run) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Dry run: shows what would sync from %s to %s (nothing changed).",
                 src, dst);
        out->risk = RISK_SAFE;
        return;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Syncs %s to %s%s%s.",
             src, dst,
             archive ? " (preserving permissions, timestamps, symlinks)" : "",
             del     ? ", deleting destination files absent at source" : "");

    out->risk = RISK_LOW;
    if (del) {
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "--delete permanently removes files at destination that are "
                 "not present at source.");
        snprintf(out->safer, sizeof out->safer,
                 "rsync -n ...  (dry run to preview before deleting)");
    }
    out->risk = risk_max(out->risk, path_risk(dst));
}

static void analyze_scp(const TokList *tl, Analysis *out)
{
    bool recursive = tok_has_short(tl, 'r') || tok_has_short(tl, 'R');

    char src[256] = "source", dst[256] = "destination";
    int nargs = 0;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type != TT_WORD) continue;
        if      (nargs == 0) snprintf(src, sizeof src, "%s", tl->t[i].s);
        else if (nargs == 1) snprintf(dst, sizeof dst, "%s", tl->t[i].s);
        nargs++;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Copies %s to %s over SSH%s.",
             src, dst, recursive ? " recursively" : "");

    out->risk = RISK_LOW;
    out->risk = risk_max(out->risk, path_risk(dst));
}

static void analyze_iptables(const TokList *tl, Analysis *out)
{
    bool flush  = tok_has_short(tl, 'F') || tok_has_long(tl, "--flush");
    bool append = tok_has_short(tl, 'A') || tok_has_long(tl, "--append");
    bool del    = tok_has_short(tl, 'D') || tok_has_long(tl, "--delete");
    bool insert = tok_has_short(tl, 'I') || tok_has_long(tl, "--insert");
    bool policy = tok_has_short(tl, 'P') || tok_has_long(tl, "--policy");

    const char *chain  = NULL;
    const char *action = NULL;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) {
            if      (!chain)  chain  = tl->t[i].s;
            else if (!action) action = tl->t[i].s;
            else break;
        }
    }
    const char *target = tok_short_val(tl, 'j');
    if (!target) target = tok_long_val(tl, "--jump");

    if (flush) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Flushes (clears) all rules from %s.",
                 chain ? chain : "all chains");
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "Clearing firewall rules may immediately expose services "
                 "to the network.");
    } else if (policy) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Sets the default policy for chain %s to %s.",
                 chain ? chain : "?", action ? action : "?");
        out->risk = RISK_HIGH;
        if (action && strcmp(action, "ACCEPT") == 0)
            snprintf(out->warning, sizeof out->warning,
                     "Setting a chain default policy to ACCEPT is permissive - "
                     "all unmatched packets are allowed.");
    } else if (append || insert) {
        const char *action_desc = "";
        if (target) {
            if      (strcmp(target, "DROP")   == 0) action_desc = " (silently drops matching traffic)";
            else if (strcmp(target, "ACCEPT") == 0) action_desc = " (accepts matching traffic)";
            else if (strcmp(target, "REJECT") == 0) action_desc = " (rejects with an error response)";
        }
        snprintf(out->explanation, sizeof out->explanation,
                 "%s a rule to chain %s%s.",
                 append ? "Appends" : "Inserts",
                 chain ? chain : "?",
                 action_desc);
        out->risk = RISK_MEDIUM;
    } else if (del) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes a rule from chain %s.", chain ? chain : "?");
        out->risk = RISK_MEDIUM;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Modifies netfilter firewall rules.");
        out->risk = RISK_MEDIUM;
    }
}

static void analyze_systemctl(const TokList *tl, Analysis *out)
{
    const Tok *subcmd = tok_first_word(tl, 1);
    const Tok *unit   = subcmd
        ? tok_first_word(tl, (int)(subcmd - tl->t) + 1)
        : NULL;

    char sub[64]  = "unknown";
    char svc[128] = "the specified unit";
    if (subcmd) snprintf(sub, sizeof sub, "%s", subcmd->s);
    if (unit)   snprintf(svc, sizeof svc, "%s", unit->s);

    if      (strcmp(sub, "start")         == 0) {
        snprintf(out->explanation, sizeof out->explanation, "Starts service %s.", svc);
        out->risk = RISK_LOW;
    } else if (strcmp(sub, "stop")        == 0) {
        snprintf(out->explanation, sizeof out->explanation, "Stops service %s.", svc);
        out->risk = RISK_MEDIUM;
    } else if (strcmp(sub, "restart")     == 0) {
        snprintf(out->explanation, sizeof out->explanation, "Restarts service %s.", svc);
        out->risk = RISK_MEDIUM;
    } else if (strcmp(sub, "enable")      == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Enables %s to start automatically at boot.", svc);
        out->risk = RISK_LOW;
    } else if (strcmp(sub, "disable")     == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Prevents %s from starting automatically at boot.", svc);
        out->risk = RISK_LOW;
    } else if (strcmp(sub, "mask")        == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Masks %s - it cannot be started by any means until unmasked.", svc);
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Masking prevents the service from ever being started, even manually.");
    } else if (strcmp(sub, "daemon-reload") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Reloads systemd manager configuration (picks up unit file changes).");
        out->risk = RISK_LOW;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs systemctl %s on %s.", sub, svc);
        out->risk = RISK_MEDIUM;
    }
}

static void analyze_sudo(const TokList *tl, Analysis *out)
{
    TokList inner = { .n = 0 };
    bool found = false;

    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (!found) {
            if (t->type == TT_FLAG_SHORT || t->type == TT_FLAG_LONG) {
                /* -u/-g eat the next token as their value, skip it */
                if (t->type == TT_FLAG_SHORT &&
                    (t->s[1] == 'u' || t->s[1] == 'g') && t->s[2] == '\0')
                    i++;
                continue;
            }
            found = true;
            inner.t[inner.n]      = *t;
            inner.t[inner.n].type = TT_COMMAND;
            inner.n++;
        } else {
            inner.t[inner.n++] = *t;
            if (inner.n >= TOK_MAX - 1) break;
        }
    }

    if (!found) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Executes a command with elevated root privileges.");
        out->risk = RISK_HIGH;
        out->matched = true;
        return;
    }

    cmd_analyze(&inner, out);
    out->matched = true;

    if (out->risk < RISK_CRITICAL)
        out->risk = (Risk)(out->risk + 1);

    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", out->explanation);
    snprintf(out->explanation, sizeof out->explanation, "As root: %s", tmp);
}

typedef struct {
    const char *key;
    void (*fn)(const TokList *, Analysis *);
} CmdEntry;

/*
 * bsearch dispatch. O(log n), fast enough for any sane number of commands.
 * table is sorted at runtime on first call - add entries in whatever order
 * you want, the sort handles it. no more "oops wrong alphabetical order" bugs.
 *
 * around 200 entries: think about a hashmap.
 * at 1000+: you definitely should have done it already and this comment haunts you.
 */

/* ---------------------------------------------------------------- network */

static void analyze_ssh(const TokList *tl, Analysis *out)
{
    bool no_host_check = false;
    bool agent_forward = tok_has_short(tl, 'A');
    bool x11_forward   = tok_has_short(tl, 'X') || tok_has_short(tl, 'Y');

    for (int i = 1; i < tl->n - 1; i++) {
        if (strcmp(tl->t[i].s, "-o") == 0 &&
            strstr(tl->t[i+1].s, "StrictHostKeyChecking=no"))
            no_host_check = true;
    }

    const char *host = NULL;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD && tl->t[i].s[0] != '-') {
            host = tl->t[i].s; break;
        }
    }

    char hdesc[128] = "a remote host";
    if (host) snprintf(hdesc, sizeof hdesc, "%s", host);

    snprintf(out->explanation, sizeof out->explanation,
             "Opens an SSH session to %s%s%s.",
             hdesc,
             agent_forward ? " (forwarding your SSH agent)" : "",
             x11_forward   ? " (forwarding X11 display)"   : "");

    out->risk = RISK_SAFE;
    if (agent_forward) {
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "-A forwards your SSH agent - a compromised remote host can use your keys.");
        snprintf(out->safer, sizeof out->safer,
                 "Use -a (lowercase) to explicitly disable agent forwarding.");
    }
    if (no_host_check) {
        out->risk = risk_max(out->risk, RISK_HIGH);
        snprintf(out->warning, sizeof out->warning,
                 "StrictHostKeyChecking=no skips host verification - vulnerable to MITM.");
    }
}

static void analyze_nc(const TokList *tl, Analysis *out)
{
    bool listen  = tok_has_short(tl, 'l') || tok_has_long(tl, "--listen");
    bool exec    = tok_has_short(tl, 'e');  /* -e /bin/sh: shell backdoor */
    bool udp     = tok_has_short(tl, 'u');

    const Tok *port = NULL;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD && tl->t[i].s[0] != '-')
            port = &tl->t[i];
    }

    if (listen && exec) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Opens a %s listener and hands the connection a shell - classic backdoor.",
                 udp ? "UDP" : "TCP");
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "Anyone who connects gets an interactive shell with your privileges.");
    } else if (listen) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Listens on %s%s for incoming connections.",
                 port ? port->s : "a port",
                 udp ? " (UDP)" : " (TCP)");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Exposes a port - make sure you know who can reach it.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Connects to %s%s.",
                 port ? port->s : "a host/port",
                 udp ? " over UDP" : " over TCP");
        out->risk = RISK_SAFE;
    }
}

static void analyze_ip(const TokList *tl, Analysis *out)
{
    const char *sub = NULL;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) { sub = tl->t[i].s; break; }
    }

    if (!sub || strcmp(sub, "addr") == 0 || strcmp(sub, "address") == 0 ||
        strcmp(sub, "a")    == 0 ||
        strcmp(sub, "link") == 0 || strcmp(sub, "route") == 0 ||
        strcmp(sub, "r")    == 0 || strcmp(sub, "l") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows network interface and routing information.");
        out->risk = RISK_SAFE;
    } else if (strcmp(sub, "neigh") == 0 || strcmp(sub, "n") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows ARP/neighbour table.");
        out->risk = RISK_SAFE;
    } else {
        /* ip link set, ip addr add/del, ip route add/del - modifying network */
        snprintf(out->explanation, sizeof out->explanation,
                 "Modifies network configuration (%s).", sub);
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Changing network config can drop your connection immediately.");
    }
}

static void analyze_ping(const TokList *tl, Analysis *out)
{
    const Tok *host = word_of(tl, "cisWtIlpS", NULL, 0);
    char h[128] = "a host";
    if (host) snprintf(h, sizeof h, "%s", host->s);
    snprintf(out->explanation, sizeof out->explanation,
             "Sends ICMP echo packets to %s to check connectivity.", h);
    out->risk = RISK_SAFE;
}

/* ---------------------------------------------------------------- filesystem / system */

static void analyze_mount(const TokList *tl, Analysis *out)
{
    bool bind = tok_has_long(tl, "--bind");
    bool ro   = tok_has_short(tl, 'r') || tok_has_long(tl, "--read-only");

    const Tok *dev = NULL, *mnt = NULL;
    int nw = 0;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type != TT_WORD) continue;
        if (nw == 0) dev = &tl->t[i];
        else         mnt = &tl->t[i];
        nw++;
    }

    if (!dev && !mnt) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists all currently mounted filesystems.");
        out->risk = RISK_SAFE;
        return;
    }

    char src[256] = "(source)", dst[256] = "(mountpoint)";
    if (dev) snprintf(src, sizeof src, "%s", dev->s);
    if (mnt) snprintf(dst, sizeof dst, "%s", mnt->s);

    snprintf(out->explanation, sizeof out->explanation,
             "%s %s at %s%s.",
             bind ? "Bind-mounts" : "Mounts",
             src, dst,
             ro ? " (read-only)" : "");

    out->risk = RISK_MEDIUM;
    if (path_risk(dst) >= RISK_HIGH)
        out->risk = RISK_HIGH;
    if (strstr(src, "/dev/") == src && !ro) {
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Mounting a block device read-write - existing data on %s may be affected.",
                 src);
    }
}

static void analyze_umount(const TokList *tl, Analysis *out)
{
    const Tok *t = tok_first_word(tl, 1);
    char tgt[256] = "the filesystem";
    if (t) snprintf(tgt, sizeof tgt, "%s", t->s);
    snprintf(out->explanation, sizeof out->explanation,
             "Unmounts %s. Any buffered writes will be flushed first.", tgt);
    out->risk = RISK_LOW;
    snprintf(out->warning, sizeof out->warning,
             "Unmounting a busy filesystem can kill processes using it.");
}

static void analyze_fdisk(const TokList *tl, Analysis *out)
{
    bool list = tok_has_short(tl, 'l') || tok_has_long(tl, "--list");
    const Tok *dev = tok_first_word(tl, 1);
    char d[128] = "a device";
    if (dev) snprintf(d, sizeof d, "%s", dev->s);

    if (list) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists partition tables on all or specified devices.");
        out->risk = RISK_SAFE;
        return;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Opens an interactive partition editor for %s.", d);
    out->risk = RISK_CRITICAL;
    snprintf(out->warning, sizeof out->warning,
             "Writing a new partition table destroys all data on %s.", d);
}

static void analyze_parted(const TokList *tl, Analysis *out)
{
    bool print = false;
    for (int i = 1; i < tl->n; i++)
        if (strcmp(tl->t[i].s, "print") == 0) { print = true; break; }

    const Tok *dev = tok_first_word(tl, 1);
    char d[128] = "a device";
    if (dev) snprintf(d, sizeof d, "%s", dev->s);

    if (print) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints the partition table of %s.", d);
        out->risk = RISK_SAFE;
        return;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Modifies partition table of %s.", d);
    out->risk = RISK_CRITICAL;
    snprintf(out->warning, sizeof out->warning,
             "Partition changes on %s are immediate and data loss is likely.", d);
}

static void analyze_chroot(const TokList *tl, Analysis *out)
{
    const Tok *t = tok_first_word(tl, 1);
    char dir[256] = "a directory";
    if (t) snprintf(dir, sizeof dir, "%s", t->s);
    snprintf(out->explanation, sizeof out->explanation,
             "Changes the root filesystem to %s - processes inside see a different /.", dir);
    out->risk = RISK_HIGH;
    snprintf(out->warning, sizeof out->warning,
             "Requires root. If the chroot environment has a shell, it can escape to the real system.");
}

static void analyze_useradd(const TokList *tl, Analysis *out)
{
    bool system  = tok_has_short(tl, 'r') || tok_has_long(tl, "--system");
    bool sudoer  = false;
    for (int i = 1; i < tl->n - 1; i++)
        if ((strcmp(tl->t[i].s, "-G") == 0 || strcmp(tl->t[i].s, "--groups") == 0) &&
            strstr(tl->t[i+1].s, "sudo"))
            sudoer = true;

    const Tok *user = NULL;
    for (int i = tl->n - 1; i >= 1; i--)
        if (tl->t[i].type == TT_WORD) { user = &tl->t[i]; break; }

    char u[128] = "a new user";
    if (user) snprintf(u, sizeof u, "'%s'", user->s);

    snprintf(out->explanation, sizeof out->explanation,
             "Creates %s%s.", u, system ? " (system account, no home dir)" : "");
    out->risk = RISK_MEDIUM;
    if (sudoer) {
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Adding to the sudo group gives this user full root access.");
    }
}

static void analyze_userdel(const TokList *tl, Analysis *out)
{
    bool remove_home = tok_has_short(tl, 'r') || tok_has_long(tl, "--remove");
    const Tok *user = NULL;
    for (int i = tl->n - 1; i >= 1; i--)
        if (tl->t[i].type == TT_WORD) { user = &tl->t[i]; break; }

    char u[128] = "a user";
    if (user) snprintf(u, sizeof u, "'%s'", user->s);

    snprintf(out->explanation, sizeof out->explanation,
             "Deletes %s%s.",
             u, remove_home ? " and their home directory" : "");
    out->risk = remove_home ? RISK_HIGH : RISK_MEDIUM;
    if (remove_home)
        snprintf(out->warning, sizeof out->warning,
                 "Home directory deletion is permanent.");
}

static void analyze_crontab(const TokList *tl, Analysis *out)
{
    bool edit   = tok_has_short(tl, 'e');
    bool remove = tok_has_short(tl, 'r');
    bool list   = tok_has_short(tl, 'l');

    if (remove) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Removes ALL scheduled cron jobs for this user - no confirmation.");
        out->risk = RISK_HIGH;
        snprintf(out->safer, sizeof out->safer,
                 "crontab -l > crontab.bak first to save a copy.");
    } else if (edit) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Opens the crontab for editing - scheduled commands run as this user.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "A malicious crontab entry runs silently on a schedule - common persistence trick.");
    } else if (list) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists all scheduled cron jobs for this user.");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Manages scheduled cron jobs.");
        out->risk = RISK_LOW;
    }
}

static void analyze_du(const TokList *tl, Analysis *out)
{
    bool human   = tok_has_short(tl, 'h');
    bool summary = tok_has_short(tl, 's') || tok_has_long(tl, "--summarize");
    bool max1    = tok_has_long(tl, "--max-depth");

    const Tok *t = word_of(tl, "dBt", NULL, 0);
    char dir[256] = "the current directory";
    if (t) snprintf(dir, sizeof dir, "%s", t->s);

    snprintf(out->explanation, sizeof out->explanation,
             "%s%s of %s.",
             summary ? "Shows total disk usage" : "Shows disk usage",
             human   ? " in human-readable units" : "",
             dir);

    (void)max1;
    out->risk = RISK_SAFE;
}

static void analyze_lsblk(const TokList *tl, Analysis *out)
{
    (void)tl;
    snprintf(out->explanation, sizeof out->explanation,
             "Lists block devices (disks, partitions, loop devices) and their mount points.");
    out->risk = RISK_SAFE;
}

static void analyze_journalctl(const TokList *tl, Analysis *out)
{
    bool follow  = tok_has_short(tl, 'f') || tok_has_long(tl, "--follow");
    bool unit    = tok_has_short(tl, 'u') || tok_has_long(tl, "--unit");
    bool since   = tok_has_long(tl, "--since");
    bool kernel  = tok_has_short(tl, 'k') || tok_has_long(tl, "--dmesg");

    (void)since; (void)unit;

    if (kernel)
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows kernel log messages (like dmesg, but from the journal).");
    else if (follow)
        snprintf(out->explanation, sizeof out->explanation,
                 "Streams the system journal in real time (like tail -f for systemd logs).");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries the systemd journal log.");
    out->risk = RISK_SAFE;
}

/* ---------------------------------------------------------------- script execution */

static void analyze_bash(const TokList *tl, Analysis *out)
{
    bool inline_cmd = tok_has_short(tl, 'c');
    bool check_only = tok_has_short(tl, 'n');
    /* -o and -O take an option's name: -o pipefail */
    int w[1];
    const Tok *script = words_of(tl, 1, "oO", NULL, w, 1) ? &tl->t[w[0]] : NULL;
    bool from_input = tok_has_short(tl, 's') || (g_redir.heredoc && !script);

    if (check_only && script) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Checks the shell script '%s' for syntax errors, without running it.",
                 script->s);
        out->risk = RISK_SAFE;
    } else if (from_input && !inline_cmd) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs the shell commands %s.",
                 g_redir.heredoc ? "that follow" : "from its input");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Executes whatever commands it reads - verify them before running.");
    } else if (inline_cmd) {
        /* bash -c "something" */
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an inline shell command string directly.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "-c executes whatever string follows - verify the source before running.");
    } else if (script) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs the shell script '%s'.", script->s);
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Scripts run with your privileges - read it before executing.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Opens an interactive bash shell.");
        out->risk = RISK_SAFE;
    }
}

static void analyze_python(const TokList *tl, Analysis *out)
{
    bool inline_cmd = tok_has_short(tl, 'c');
    /* -m, -W and -X take a value, also last in a bundle (-um mod) */
    int w[1];
    int nw = words_of(tl, 1, "mWX", NULL, w, 1);
    const Tok *script = nw ? &tl->t[w[0]] : NULL;
    const char *module = NULL;
    for (int i = 1; i < tl->n && (!script || i < w[0]); i++) {
        const char *s = tl->t[i].s;
        if (tl->t[i].type == TT_FLAG_SHORT && strlen(s) <= 5 && s[strlen(s) - 1] == 'm'
            && i + 1 < tl->n)
            module = tl->t[i + 1].s;
    }

    if (module) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs the Python module '%s'.", module);
        out->risk = RISK_MEDIUM;
    } else if (inline_cmd) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an inline Python expression.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "-c executes arbitrary Python - make sure you know what it does.");
    } else if (g_redir.heredoc || (script && strcmp(script->s, "-") == 0)) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs the Python code %s.",
                 g_redir.heredoc ? "that follows" : "from its input");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Executes arbitrary Python - make sure you know what it does.");
    } else if (script) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs the Python script '%s'.", script->s);
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Scripts run with your privileges - read it before executing.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Opens an interactive Python interpreter.");
        out->risk = RISK_SAFE;
    }
}

static void analyze_eval(const TokList *tl, Analysis *out)
{
    (void)tl;
    snprintf(out->explanation, sizeof out->explanation,
             "Evaluates and executes its arguments as a shell command.");
    out->risk = RISK_CRITICAL;
    snprintf(out->warning, sizeof out->warning,
             "eval executes whatever string you give it - a common code injection vector. "
             "If the string comes from user input or a network source, this is a serious risk.");
    snprintf(out->safer, sizeof out->safer,
             "If you can avoid eval, do. There's almost always a better way.");
}

static void analyze_base64(const TokList *tl, Analysis *out)
{
    bool decode = tok_has_short(tl, 'd') || tok_has_long(tl, "--decode");

    if (decode) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Decodes base64-encoded input back to its original form.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Decoding then piping to bash/sh is a classic obfuscation trick - "
                 "always inspect the decoded content before executing it.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Encodes input as base64. Often used to transfer binary data safely.");
        out->risk = RISK_SAFE;
    }
}

static void analyze_nohup(const TokList *tl, Analysis *out)
{
    /* the command it runs, up to three words; "&" only backgrounds it */
    char c[128] = "a command", run[120] = "";
    for (int i = 1, k = 0; i < tl->n && k < 3; i++) {
        if (strcmp(tl->t[i].s, "&") == 0) continue;
        if (k++) strncat(run, " ", sizeof run - strlen(run) - 1);
        strncat(run, tl->t[i].s, sizeof run - strlen(run) - 1);
    }
    if (run[0]) snprintf(c, sizeof c, "'%s'", run);
    g_redir.used = true;

    snprintf(out->explanation, sizeof out->explanation,
             "Runs %s detached from the terminal - keeps running after you log out, "
             "output goes to %s.", c, g_redir.out[0] ? g_redir.out : "nohup.out");
    out->risk = RISK_LOW;
}

/* ---------------------------------------------------------------- package managers */

static void analyze_pip(const TokList *tl, Analysis *out)
{
    const char *sub = NULL;
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type == TT_WORD) { sub = tl->t[i].s; break; }

    if (!sub) {
        snprintf(out->explanation, sizeof out->explanation, "Python package manager.");
        out->risk = RISK_SAFE;
        return;
    }

    if (strcmp(sub, "install") == 0) {
        bool user   = tok_has_long(tl, "--user");
        bool upgrade = tok_has_short(tl, 'U') || tok_has_long(tl, "--upgrade");
        snprintf(out->explanation, sizeof out->explanation,
                 "Installs a Python package%s%s.",
                 user    ? " into your user directory (no root needed)" : " system-wide",
                 upgrade ? ", upgrading if already installed" : "");
        out->risk = user ? RISK_LOW : RISK_MEDIUM;
        if (!user)
            snprintf(out->warning, sizeof out->warning,
                     "Installing system-wide overwrites packages used by other tools. "
                     "Prefer --user or a virtual environment.");
    } else if (strcmp(sub, "uninstall") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Removes a Python package.");
        out->risk = RISK_LOW;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs pip %s.", sub);
        out->risk = RISK_SAFE;
    }
}

static void analyze_npm(const TokList *tl, Analysis *out)
{
    const char *sub = NULL;
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type == TT_WORD) { sub = tl->t[i].s; break; }

    if (!sub) {
        snprintf(out->explanation, sizeof out->explanation, "Node.js package manager.");
        out->risk = RISK_SAFE;
        return;
    }

    if (strcmp(sub, "install") == 0 || strcmp(sub, "i") == 0) {
        bool global = tok_has_short(tl, 'g') || tok_has_long(tl, "--global");
        snprintf(out->explanation, sizeof out->explanation,
                 "Installs Node.js dependencies from package.json%s.",
                 global ? " globally" : "");
        out->risk = global ? RISK_MEDIUM : RISK_LOW;
        if (global)
            snprintf(out->warning, sizeof out->warning,
                     "Global install affects all Node projects on this machine.");
    } else if (strcmp(sub, "run") == 0 || strcmp(sub, "start") == 0 ||
               strcmp(sub, "test") == 0 || strcmp(sub, "build") == 0) {
        const Tok *script = NULL;
        for (int i = 2; i < tl->n; i++)
            if (tl->t[i].type == TT_WORD) { script = &tl->t[i]; break; }
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs the '%s' script defined in package.json.",
                 script ? script->s : sub);
        out->risk = RISK_LOW;
        snprintf(out->warning, sizeof out->warning,
                 "npm scripts can run arbitrary shell commands - check package.json first.");
    } else if (strcmp(sub, "publish") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Publishes the package to the npm registry. Public by default.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "This pushes your code to the public internet unless --access=restricted.");
    } else {
        snprintf(out->explanation, sizeof out->explanation, "Runs npm %s.", sub);
        out->risk = RISK_SAFE;
    }
}

/* ---------------------------------------------------------------- more git subcommands */

static void analyze_git_clone(const TokList *tl, Analysis *out)
{
    bool depth  = tok_has_long(tl, "--depth");
    bool single = tok_has_long(tl, "--single-branch");
    bool bare   = tok_has_long(tl, "--bare");

    const char *url = NULL;
    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (tl->t[i].type == TT_WORD && strcmp(s, "clone") != 0) {
            url = s; break;
        }
    }

    char udesc[256] = "a repository";
    if (url) snprintf(udesc, sizeof udesc, "%s", url);

    snprintf(out->explanation, sizeof out->explanation,
             "Downloads %s%s%s%s.",
             udesc,
             bare   ? " as a bare repository (no working tree)" : "",
             depth  ? " (shallow clone, limited history)" : "",
             single ? " (single branch only)" : "");
    out->risk = RISK_SAFE;
    snprintf(out->warning, sizeof out->warning,
             "The repo may contain scripts or Makefiles that run code - inspect before building.");
}

static void analyze_git_pull(const TokList *tl, Analysis *out)
{
    bool rebase = tok_has_long(tl, "--rebase");
    bool ff     = tok_has_long(tl, "--ff-only");

    snprintf(out->explanation, sizeof out->explanation,
             "Fetches and %s remote changes into the current branch.",
             rebase ? "rebases" : "merges");
    out->risk = RISK_LOW;
    if (rebase)
        snprintf(out->warning, sizeof out->warning,
                 "--rebase rewrites local commits - don't do this on shared branches.");
    if (!rebase && !ff)
        snprintf(out->safer, sizeof out->safer,
                 "git pull --ff-only fails instead of creating a merge commit, keeping history clean.");
}

/* a word after git's subcommand that names a file rather than a commit */
static bool git_is_path(const TokList *tl, int i)
{
    if (tl->dd >= 0) return i >= tl->dd;
    const char *s = tl->t[i].s;
    size_t      l = strlen(s);
    if (strstr(s, "..") || strpbrk(s, "~^@:")) return false;
    return s[0] == '/' || s[0] == '.' || (l && s[l - 1] == '/') ||
           (strrchr(s, '.') && strrchr(s, '.') > s);
}

/* the commits and the paths after git's subcommand */
static void git_split(const TokList *tl, const char *vshort, const char *const *vlong,
                      int *revs, int *nrev, char *paths, size_t plen)
{
    int idx[TOK_MAX], pidx[TOK_MAX], np = 0;
    int n = words_of(tl, g_sub + 1, vshort, vlong, idx, TOK_MAX);
    *nrev = 0;
    for (int k = 0; k < n; k++) {
        if (git_is_path(tl, idx[k])) pidx[np++] = idx[k];
        else                          revs[(*nrev)++] = idx[k];
    }
    list_of(tl, pidx, np, paths, plen);
}

/* " between A and B" for A..B or two commits, " since A" for one */
static void git_span(const TokList *tl, const int *revs, int nrev,
                     const char *one, char *buf, size_t len)
{
    buf[0] = '\0';
    if (nrev == 0) return;
    const char *r   = tl->t[revs[0]].s;
    const char *dot = strstr(r, "..");
    if (dot) {
        const char *b = dot + 2;
        if (*b == '.') b++;
        snprintf(buf, len, " between %.*s and %s",
                 (int)(dot - r), dot == r ? "HEAD" : r, *b ? b : "HEAD");
    } else if (nrev >= 2) {
        snprintf(buf, len, " between %s and %s", r, tl->t[revs[1]].s);
    } else {
        snprintf(buf, len, " %s %s", one, r);
    }
}

static void analyze_git_diff(const TokList *tl, Analysis *out)
{
    bool cached = tok_has_long(tl, "--cached") || tok_has_long(tl, "--staged");
    bool stat   = tok_has_long(tl, "--stat") || tok_has_long(tl, "--shortstat");
    bool names  = tok_has_long(tl, "--name-only") || tok_has_long(tl, "--name-status");

    int  revs[TOK_MAX], nrev;
    char paths[512], span[300];
    git_split(tl, NULL, NULL, revs, &nrev, paths, sizeof paths);
    git_span(tl, revs, nrev, "since", span, sizeof span);

    if (stat || names) {
        const char *what = nrev ? "the files changed" :
                           cached ? "the files staged for the next commit" :
                                    "the files with uncommitted changes";
        snprintf(out->explanation, sizeof out->explanation, "Lists %s%s%s%s%s.",
                 what, paths[0] ? " in " : "", paths, span,
                 stat ? ", with a count of changed lines" : "");
    } else {
        snprintf(out->explanation, sizeof out->explanation, "Shows the %schanges%s%s%s.",
                 cached ? "staged " : nrev ? "" : "uncommitted ",
                 paths[0] ? " to " : "", paths, span);
    }
    out->risk = RISK_SAFE;
}

/* the subject of a commit message, even one given as "$(cat <<'EOF' ...)" */
static void commit_subject(const char *m, char *buf, size_t len)
{
    if (strncmp(m, "$(cat <<", 8) == 0) {
        const char *nl = strchr(m, '\n');
        m = nl ? nl + 1 : "";
    }
    /* count letters, not bytes: a UTF-8 continuation byte is 10xxxxxx */
    size_t l = strcspn(m, "\n"), cut = l;
    int letters = 0;
    for (size_t i = 0; i < l; i++) {
        if (((unsigned char)m[i] & 0xC0) == 0x80) continue;
        if (++letters == 70) cut = i;
    }
    if (letters > 72) snprintf(buf, len, "%.*s...", (int)cut, m);
    else              snprintf(buf, len, "%.*s", (int)l, m);
}

static void analyze_git_commit(const TokList *tl, Analysis *out)
{
    bool amend   = tok_has_long(tl, "--amend");
    bool all     = tok_has_short(tl, 'a') || tok_has_long(tl, "--all");
    bool no_edit = tok_has_long(tl, "--no-edit");
    const char *m = tok_short_val(tl, 'm');
    if (!m) m = tok_long_val(tl, "--message");

    char subject[320] = "", quoted[352] = "";
    if (m) commit_subject(m, subject, sizeof subject);
    if (subject[0]) snprintf(quoted, sizeof quoted, ": \"%s\"", subject);

    if (amend) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Rewrites the last commit with the staged changes%s.",
                 no_edit ? ", keeping its message" : quoted);
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "--amend rewrites history. If you've already pushed, you'll need --force.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Records %s as a new commit%s.",
                 all ? "every change to tracked files" : "the staged changes", quoted);
        out->risk = RISK_SAFE;
    }
}

static void analyze_git_status(const TokList *tl, Analysis *out)
{
    int  idx[TOK_MAX];
    int  n = words_of(tl, g_sub + 1, NULL, NULL, idx, TOK_MAX);
    char paths[512];
    list_of(tl, idx, n, paths, sizeof paths);
    snprintf(out->explanation, sizeof out->explanation,
             "Shows which files are changed, staged or untracked%s%s.",
             n ? " in " : "", paths);
    out->risk = RISK_SAFE;
}

static void analyze_git_log(const TokList *tl, Analysis *out)
{
    static const char *const vlong[] = {
        "--author", "--grep", "--since", "--until", "--after", "--before",
        "--format", "--pretty", "--max-count", NULL,
    };
    const char *count = count_of(tl, 'n', "--max-count");
    const char *pick  = short_val(tl, 'S');
    const char *grep  = tok_long_val(tl, "--grep");
    const char *who   = tok_long_val(tl, "--author");

    int  revs[TOK_MAX], nrev;
    char paths[512], span[300] = "";
    git_split(tl, "nSG", vlong, revs, &nrev, paths, sizeof paths);
    if (nrev && strstr(tl->t[revs[0]].s, "..")) {
        const char *r = tl->t[revs[0]].s, *dot = strstr(r, "..");
        snprintf(span, sizeof span, " in %s but not in %.*s",
                 dot[2] ? dot + 2 : "HEAD", (int)(dot - r), r);
    } else if (nrev) {
        snprintf(span, sizeof span, " on %s", tl->t[revs[0]].s);
    }

    char head[64], filt[512] = "";
    size_t f = 0;
    if (paths[0]) f += (size_t)snprintf(filt + f, sizeof filt - f, " that changed %s", paths);
    if (pick && f < sizeof filt)
        f += (size_t)snprintf(filt + f, sizeof filt - f, "%s added or removed '%s'",
                              f ? " and" : " that", pick);
    if (grep && f < sizeof filt)
        f += (size_t)snprintf(filt + f, sizeof filt - f, " whose message mentions '%s'", grep);
    if (who && f < sizeof filt)
        snprintf(filt + f, sizeof filt - f, " by %s", who);

    if (count && strcmp(count, "1") == 0) snprintf(head, sizeof head, "the last commit");
    else if (count)               snprintf(head, sizeof head, "the last %s commits", count);
    else if (filt[0] || span[0])  snprintf(head, sizeof head, "the commits");
    else                          snprintf(head, sizeof head, "the commit history");

    bool patch = tok_has_short(tl, 'p') || tok_has_long(tl, "--patch");
    snprintf(out->explanation, sizeof out->explanation, "Shows %s%s%s%s%s.",
             head, filt, span,
             tok_has_long(tl, "--all") ? " on every branch" : "",
             tok_has_long(tl, "--oneline") ? ", one line each" :
             patch ? ", with their diffs" :
             tok_has_long(tl, "--stat") ? ", with the files each changed" : "");
    out->risk = RISK_SAFE;
}

static void analyze_git_add(const TokList *tl, Analysis *out)
{
    int  idx[TOK_MAX];
    int  n = words_of(tl, g_sub + 1, NULL, NULL, idx, TOK_MAX);
    char files[512];
    list_of(tl, idx, n, files, sizeof files);

    if (tok_has_short(tl, 'A') || tok_has_long(tl, "--all"))
        snprintf(out->explanation, sizeof out->explanation,
                 "Stages every change in the repo for the next commit.");
    else if (tok_has_short(tl, 'u') || tok_has_long(tl, "--update"))
        snprintf(out->explanation, sizeof out->explanation,
                 "Stages the changes to tracked files%s%s for the next commit.",
                 n ? " in " : "", files);
    else if (tok_has_short(tl, 'p') || tok_has_long(tl, "--patch"))
        snprintf(out->explanation, sizeof out->explanation,
                 "Asks, change by change, what to stage%s%s.", n ? " in " : "", files);
    else if (n == 1 && (strcmp(files, ".") == 0 || files[strlen(files) - 1] == '/'))
        snprintf(out->explanation, sizeof out->explanation,
                 "Stages every change under %s for the next commit.", files);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Stages %s for the next commit.", n ? files : "nothing");
    out->risk = RISK_SAFE;
}

static void analyze_git_show(const TokList *tl, Analysis *out)
{
    int idx[TOK_MAX];
    int n = words_of(tl, g_sub + 1, NULL, NULL, idx, TOK_MAX);
    const char *rev   = n ? tl->t[idx[0]].s : NULL;
    const char *colon = rev ? strchr(rev, ':') : NULL;

    if (colon)
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows %s as it was at %.*s.", colon + 1,
                 (int)(colon - rev), colon == rev ? "the staging area" : rev);
    else
        snprintf(out->explanation, sizeof out->explanation, "Shows %s%s%s.",
                 rev ? "commit " : "the last commit", rev ? rev : "",
                 tok_has_long(tl, "--stat") || tok_has_long(tl, "--name-only") ||
                 tok_has_long(tl, "--name-status")
                     ? " and the files it changed" : " and its changes");
    out->risk = RISK_SAFE;
}

static void analyze_git_branch(const TokList *tl, Analysis *out)
{
    int idx[TOK_MAX];
    int n = words_of(tl, g_sub + 1, NULL, NULL, idx, TOK_MAX);
    const char *a = n > 0 ? tl->t[idx[0]].s : "";
    const char *b = n > 1 ? tl->t[idx[1]].s : "";
    bool force_del = false;
    for (int i = g_sub + 1; i < tl->n; i++)
        if (strcmp(tl->t[i].s, "-D") == 0) force_del = true;

    out->risk = RISK_SAFE;
    if (tok_has_long(tl, "--show-current")) {
        snprintf(out->explanation, sizeof out->explanation, "Prints the current branch name.");
    } else if (force_del || tok_has_short(tl, 'd') || tok_has_long(tl, "--delete")) {
        snprintf(out->explanation, sizeof out->explanation, "Deletes the branch %s%s.",
                 a, force_del ? ", even if it isn't merged" : "");
        out->risk = force_del ? RISK_MEDIUM : RISK_LOW;
        if (force_del)
            snprintf(out->warning, sizeof out->warning,
                     "Commits only on that branch become unreachable.");
    } else if (tok_has_short(tl, 'm') || tok_has_short(tl, 'M') || tok_has_long(tl, "--move")) {
        if (n > 1)
            snprintf(out->explanation, sizeof out->explanation,
                     "Renames the branch %s to %s.", a, b);
        else
            snprintf(out->explanation, sizeof out->explanation,
                     "Renames the current branch to %s.", a);
    } else if (n > 0) {
        snprintf(out->explanation, sizeof out->explanation, "Creates the branch %s%s%s.",
                 a, n > 1 ? " from " : "", b);
    } else {
        snprintf(out->explanation, sizeof out->explanation, "Lists %s.",
                 tok_has_short(tl, 'a') || tok_has_long(tl, "--all")
                     ? "every branch, local and remote" :
                 tok_has_short(tl, 'r') || tok_has_long(tl, "--remotes")
                     ? "the remote branches" : "the local branches");
    }
}

static void analyze_git_rev_parse(const TokList *tl, Analysis *out)
{
    int idx[TOK_MAX];
    int n = words_of(tl, g_sub + 1, NULL, NULL, idx, TOK_MAX);
    const char *rev = n ? tl->t[idx[0]].s : NULL;

    if (tok_has_long(tl, "--show-toplevel"))
        snprintf(out->explanation, sizeof out->explanation, "Prints the repo's root folder.");
    else if (tok_has_long(tl, "--abbrev-ref"))
        snprintf(out->explanation, sizeof out->explanation, "Prints the current branch name.");
    else if (tok_has_long(tl, "--git-dir") || tok_has_long(tl, "--git-common-dir"))
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints the path of the repo's .git folder.");
    else if (tok_has_long(tl, "--is-inside-work-tree"))
        snprintf(out->explanation, sizeof out->explanation,
                 "Checks whether this folder is inside a git repo.");
    else if (rev)
        snprintf(out->explanation, sizeof out->explanation, "Prints the commit id of %s.", rev);
    else
        snprintf(out->explanation, sizeof out->explanation, "Prints details of the git repo.");
    out->risk = RISK_SAFE;
}


static void analyze_git_merge(const TokList *tl, Analysis *out)
{
    bool no_ff    = tok_has_long(tl, "--no-ff");
    bool squash   = tok_has_long(tl, "--squash");
    bool abort    = tok_has_long(tl, "--abort");

    if (abort) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Aborts an in-progress merge and restores the pre-merge state.");
        out->risk = RISK_LOW;
        return;
    }

    const Tok *branch = NULL;
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type == TT_WORD && strcmp(tl->t[i].s, "merge") != 0)
        { branch = &tl->t[i]; break; }

    char b[128] = "another branch";
    if (branch) snprintf(b, sizeof b, "'%s'", branch->s);

    snprintf(out->explanation, sizeof out->explanation,
             "Merges %s into the current branch%s.",
             b,
             squash ? " (squashing all commits into one)" :
             no_ff  ? " (always creates a merge commit)" : "");
    out->risk = RISK_LOW;
}

static void analyze_git_rebase(const TokList *tl, Analysis *out)
{
    bool interactive = tok_has_short(tl, 'i') || tok_has_long(tl, "--interactive");
    bool abort       = tok_has_long(tl, "--abort");
    bool cont        = tok_has_long(tl, "--continue");
    bool onto        = tok_has_long(tl, "--onto");

    if (abort) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Aborts an in-progress rebase and returns to the original branch state.");
        out->risk = RISK_LOW;
        return;
    }
    if (cont) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Continues a paused rebase after resolving conflicts.");
        out->risk = RISK_LOW;
        return;
    }

    const Tok *base = NULL;
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type == TT_WORD && strcmp(tl->t[i].s, "rebase") != 0)
        { base = &tl->t[i]; break; }

    char b[128] = "another branch";
    if (base) snprintf(b, sizeof b, "'%s'", base->s);

    snprintf(out->explanation, sizeof out->explanation,
             "%srebases current branch onto %s%s - rewrites commit history.",
             interactive ? "Interactively " : "",
             b,
             onto ? " (with explicit base via --onto)" : "");
    out->risk = RISK_HIGH;
    snprintf(out->warning, sizeof out->warning,
             "Rebase rewrites commits. Force-push will be required if already pushed.");
    snprintf(out->safer, sizeof out->safer,
             "Only rebase branches that haven't been pushed, or use git merge instead.");
}

static void analyze_git_stash(const TokList *tl, Analysis *out)
{
    const char *sub = NULL;
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type == TT_WORD && strcmp(tl->t[i].s, "stash") != 0)
        { sub = tl->t[i].s; break; }

    if (!sub || strcmp(sub, "push") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Saves uncommitted changes to a temporary stack and restores a clean working tree.");
        out->risk = RISK_SAFE;
    } else if (strcmp(sub, "pop") == 0 || strcmp(sub, "apply") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Restores the most recent stashed changes back to the working tree.");
        out->risk = RISK_SAFE;
    } else if (strcmp(sub, "drop") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently deletes a stash entry.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Dropped stashes are gone - there's no recovery.");
    } else if (strcmp(sub, "clear") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes ALL stash entries permanently.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "This removes every stash you have - unrecoverable.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs git stash %s.", sub);
        out->risk = RISK_SAFE;
    }
}


/* ---------------------------------------------------------------- more docker */

static void analyze_docker_build(const TokList *tl, Analysis *out)
{
    bool no_cache = tok_has_long(tl, "--no-cache");
    const char *tag = tok_long_val(tl, "-t");
    char t[128] = "(untagged)";
    if (tag) snprintf(t, sizeof t, "'%s'", tag);

    snprintf(out->explanation, sizeof out->explanation,
             "Builds a Docker image %s from a Dockerfile%s.",
             t, no_cache ? " (ignoring cache)" : "");
    out->risk = RISK_LOW;
    snprintf(out->warning, sizeof out->warning,
             "The Dockerfile may run arbitrary commands during build - inspect it first.");
}

static void analyze_docker_ps(const TokList *tl, Analysis *out)
{
    bool all = tok_has_short(tl, 'a') || tok_has_long(tl, "--all");
    snprintf(out->explanation, sizeof out->explanation,
             "Lists %s Docker containers.",
             all ? "all (including stopped)" : "running");
    out->risk = RISK_SAFE;
}

static void analyze_docker_stop(const TokList *tl, Analysis *out)
{
    bool all_flag = false;
    for (int i = 1; i < tl->n; i++)
        if (strcmp(tl->t[i].s, "$(docker ps -q)") == 0) all_flag = true;

    const Tok *ct = NULL;
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type == TT_WORD && strcmp(tl->t[i].s, "stop") != 0)
        { ct = &tl->t[i]; break; }

    char c[128] = "a container";
    if (ct) snprintf(c, sizeof c, "container '%s'", ct->s);

    snprintf(out->explanation, sizeof out->explanation,
             "Gracefully stops %s%s (sends SIGTERM, waits, then SIGKILL).",
             all_flag ? "ALL running containers" : c,
             all_flag ? "" : "");
    out->risk = all_flag ? RISK_HIGH : RISK_LOW;
}

/* sheer knows itself. obviously. */
static void analyze_sheer(const TokList *tl, Analysis *out)
{
    /* find the subcommand if any */
    const char *sub = tl->n > 1 ? tl->t[1].s : NULL;

    if (!sub) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs sheer with no arguments - shows usage.");
    } else if (strcmp(sub, "run") == 0) {
        const char *cmd = tl->n > 2 ? tl->t[2].s : NULL;
        if (cmd)
            snprintf(out->explanation, sizeof out->explanation,
                     "Explains '%s', shows its risk level, then asks before running it.", cmd);
        else
            snprintf(out->explanation, sizeof out->explanation,
                     "Explains a command, shows its risk level, then asks before running it.");
    } else if (strcmp(sub, "gen") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Generates a shell command from a plain-English description using an LLM.");
    } else if (strcmp(sub, "config") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows or edits sheer's config (~/.config/sheer/config).");
    } else {
        /* treat the rest as a command being explained */
        snprintf(out->explanation, sizeof out->explanation,
                 "Explains '%s' - shows what it does and how risky it is.", sub);
    }
    out->risk = RISK_SAFE;
}

static void analyze_shrun(const TokList *tl, Analysis *out)
{
    const char *cmd = tl->n > 1 ? tl->t[1].s : NULL;
    if (cmd)
        snprintf(out->explanation, sizeof out->explanation,
                 "Shell wrapper: explains '%s', shows risk, prompts y/N before running it. "
                 "Defined by sheer's install - it's `eval \"$(sheer run ...)\"` under the hood.",
                 cmd);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Shell wrapper around `sheer run`: explains the command, shows risk, "
                 "prompts y/N before running. Defined by sheer's install.");
    out->risk = RISK_SAFE;
}

/* ---------------------------------------------------------------- misc utilities */

static void analyze_unzip(const TokList *tl, Analysis *out)
{
    bool overwrite = tok_has_short(tl, 'o');
    bool list      = tok_has_short(tl, 'l');
    const Tok *t   = word_of(tl, "dP", NULL, 0);
    char f[256] = "an archive";
    if (t) snprintf(f, sizeof f, "'%s'", t->s);

    if (list) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists the contents of %s without extracting.", f);
        out->risk = RISK_SAFE;
        return;
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Extracts %s%s.",
             f, overwrite ? " (overwrites existing files without prompting)" : "");
    out->risk = overwrite ? RISK_MEDIUM : RISK_SAFE;
    if (overwrite)
        snprintf(out->safer, sizeof out->safer,
                 "Drop -o to be prompted before overwriting existing files.");
}

static void analyze_env(const TokList *tl, Analysis *out)
{
    bool clear = tok_has_short(tl, 'i') || tok_has_long(tl, "--ignore-environment");

    if (tl->n == 1) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints all current environment variables.");
    } else if (clear) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a command in a clean environment (no inherited variables).");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a command with specific environment variables set.");
    }
    out->risk = RISK_SAFE;
}

static void analyze_which(const TokList *tl, Analysis *out)
{
    const Tok *t = tok_first_word(tl, 1);
    char c[128] = "a command";
    if (t) snprintf(c, sizeof c, "'%s'", t->s);
    snprintf(out->explanation, sizeof out->explanation,
             "Finds the full path of %s in your PATH.", c);
    out->risk = RISK_SAFE;
}

/* ---------------------------------------------------------------- builtins
 * cd, echo, ls... the stuff everyone uses every second.
 * embarrassing that these weren't here from the start, but here we are.
 * ----------------------------------------------------------------- */

static void analyze_cd(const TokList *tl, Analysis *out)
{
    const Tok *dest = tok_first_word(tl, 1);
    char dir[256] = "the home directory";
    Risk r = RISK_SAFE;
    if (dest) {
        snprintf(dir, sizeof dir, "%s", dest->s);
        /* cd / or cd /root is worth flagging */
        r = path_risk(dest->s) >= RISK_HIGH ? RISK_LOW : RISK_SAFE;
    }
    if (dest && strcmp(dest->s, "-") == 0)
        snprintf(out->explanation, sizeof out->explanation,
                 "Changes back to the previous directory.");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Changes the current directory to %s.", dir);
    out->risk = r;
}

static void analyze_echo(const TokList *tl, Analysis *out)
{
    bool no_newline = tok_has_short(tl, 'n');
    bool interpret  = tok_has_short(tl, 'e');

    /* grab whatever comes after the flags as the message */
    char msg[256] = "";
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) {
            snprintf(msg, sizeof msg, " '%s'", tl->t[i].s);
            break;
        }
    }

    snprintf(out->explanation, sizeof out->explanation,
             "Prints%s to stdout%s.",
             msg,
             no_newline ? " (no trailing newline)" :
             interpret  ? " (interpreting escape sequences)" : "");
    out->risk = RISK_SAFE;
}

static void analyze_ls(const TokList *tl, Analysis *out)
{
    bool all     = tok_has_short(tl, 'a') || tok_has_short(tl, 'A');
    bool human   = tok_has_short(tl, 'h');
    bool long_f  = tok_has_short(tl, 'l');
    bool recurse = tok_has_short(tl, 'R');

    const Tok *target = word_of(tl, "IwT", NULL, 0);
    char dir[256] = "the current directory";
    if (target) snprintf(dir, sizeof dir, "%s", target->s);

    char detail[64] = "";
    if (long_f && human) snprintf(detail, sizeof detail, " (long format, human sizes)");
    else if (long_f)     snprintf(detail, sizeof detail, " (long format)");

    snprintf(out->explanation, sizeof out->explanation,
             "Lists %s%s%s%s.",
             all ? "all files (including hidden) in " : "files in ",
             dir, detail,
             recurse ? " recursively" : "");
    out->risk = RISK_SAFE;
}

static void analyze_mkdir(const TokList *tl, Analysis *out)
{
    bool parents = tok_has_short(tl, 'p') || tok_has_long(tl, "--parents");
    const Tok *t = word_of(tl, "m", NULL, 0);
    char dir[256] = "a directory";
    Risk r = RISK_SAFE;
    if (t) {
        snprintf(dir, sizeof dir, "%s", t->s);
        r = path_risk(t->s) >= RISK_HIGH ? RISK_LOW : RISK_SAFE;
    }
    snprintf(out->explanation, sizeof out->explanation,
             "Creates directory %s%s.",
             dir, parents ? " (and any missing parent directories)" : "");
    out->risk = r;
}

static void analyze_touch(const TokList *tl, Analysis *out)
{
    const Tok *t = word_of(tl, "tdr", NULL, 0);
    char file[256] = "a file";
    if (t) snprintf(file, sizeof file, "%s", t->s);
    snprintf(out->explanation, sizeof out->explanation,
             "Creates %s if it doesn't exist, or updates its timestamp.", file);
    out->risk = RISK_SAFE;
}

static void analyze_export(const TokList *tl, Analysis *out)
{
    const Tok *t = tok_first_word(tl, 1);
    char var[128] = "a variable";
    if (t) snprintf(var, sizeof var, "%s", t->s);
    snprintf(out->explanation, sizeof out->explanation,
             "Sets environment variable %s for this shell and any child processes.", var);
    out->risk = RISK_SAFE;
}

static void analyze_source(const TokList *tl, Analysis *out)
{
    const Tok *t = tok_first_word(tl, 1);
    char file[256] = "a script";
    if (t) snprintf(file, sizeof file, "%s", t->s);
    snprintf(out->explanation, sizeof out->explanation,
             "Runs %s in the current shell - any variables or functions it defines stick around.",
             file);
    out->risk = RISK_LOW;
    snprintf(out->warning, sizeof out->warning,
             "The script runs with your privileges and can modify your environment.");
}

static void analyze_printf(const TokList *tl, Analysis *out)
{
    (void)tl;
    snprintf(out->explanation, sizeof out->explanation,
             "Prints formatted output to stdout (like C's printf).");
    out->risk = RISK_SAFE;
}

/* --------------------------------------------------------------- pipe utils
 * grep, awk, sed, xargs, ps... mostly harmless.
 * xargs is the sneaky one - it multiplies whatever dangerous thing you pipe in.
 * ----------------------------------------------------------------- */

static void analyze_apt(const TokList *tl, Analysis *out)
{
    const char *sub = NULL;
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) { sub = tl->t[i].s; break; }
    }

    if (!sub) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Debian/Ubuntu package manager.");
        out->risk = RISK_SAFE;
        return;
    }

    if (strcmp(sub, "install") == 0 || strcmp(sub, "add") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Installs one or more packages from the repository.");
        out->risk = RISK_LOW;
    } else if (strcmp(sub, "remove") == 0 || strcmp(sub, "purge") == 0) {
        bool purge = strcmp(sub, "purge") == 0;
        snprintf(out->explanation, sizeof out->explanation,
                 "%s a package%s.",
                 purge ? "Removes" : "Uninstalls",
                 purge ? " and all its config files" : "");
        out->risk = RISK_LOW;
        if (purge)
            snprintf(out->safer, sizeof out->safer,
                     "Use 'apt remove' to keep config files (easier to restore).");
    } else if (strcmp(sub, "autoremove") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Removes packages that were installed as dependencies and are no longer needed.");
        out->risk = RISK_LOW;
    } else if (strcmp(sub, "upgrade") == 0 || strcmp(sub, "full-upgrade") == 0 ||
               strcmp(sub, "dist-upgrade") == 0) {
        bool dist = strcmp(sub, "full-upgrade") == 0 || strcmp(sub, "dist-upgrade") == 0;
        snprintf(out->explanation, sizeof out->explanation,
                 "Upgrades all installed packages%s.",
                 dist ? " (may remove conflicting packages)" : "");
        out->risk = dist ? RISK_MEDIUM : RISK_LOW;
        if (dist)
            snprintf(out->warning, sizeof out->warning,
                     "dist-upgrade/full-upgrade can remove packages to resolve conflicts.");
    } else if (strcmp(sub, "update") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Refreshes the list of available packages. Doesn't install anything.");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs apt %s.", sub);
        out->risk = RISK_SAFE;
    }
}

static void analyze_chattr(const TokList *tl, Analysis *out)
{
    bool immutable = false;
    bool append    = false;
    bool removing  = false; /* chattr -i removes, chattr +i adds */

    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (s[0] == '+' || s[0] == '-' || s[0] == '=') {
            if (s[0] == '-') removing = true;
            if (strchr(s, 'i')) immutable = true;
            if (strchr(s, 'a')) append    = true;
        }
    }

    /* skip the attribute spec (+i, -a, =i...) to get the actual path */
    const Tok *target = NULL;
    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (tl->t[i].type == TT_WORD &&
            s[0] != '+' && s[0] != '-' && s[0] != '=')
        { target = &tl->t[i]; break; }
    }
    char tgt[256] = "the file";
    if (target) snprintf(tgt, sizeof tgt, "%s", target->s);

    if (immutable && !removing) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Sets the immutable flag on %s - even root can't modify or delete it.", tgt);
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "An immutable file can't be changed until you run chattr -i.");
    } else if (immutable && removing) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Removes the immutable flag from %s, making it writable again.", tgt);
        out->risk = RISK_LOW;
    } else if (append) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Sets append-only mode on %s - data can be added but not removed.", tgt);
        out->risk = RISK_LOW;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Changes extended filesystem attributes on %s.", tgt);
        out->risk = RISK_LOW;
    }
}

static void analyze_df(const TokList *tl, Analysis *out)
{
    bool human  = tok_has_short(tl, 'h') || tok_has_long(tl, "--human-readable");
    bool inodes = tok_has_short(tl, 'i') || tok_has_long(tl, "--inodes");

    if (inodes)
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows inode usage for all mounted filesystems.");
    else if (human)
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows disk space usage in human-readable units (KB/MB/GB).");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Shows disk space usage for all mounted filesystems (in 1K blocks).");

    out->risk = RISK_SAFE;
}

static void analyze_awk(const TokList *tl, Analysis *out)
{
    const char *sep = tok_long_val(tl, "-F");

    char prog[64] = "";
    for (int i = 1; i < tl->n; i++) {
        const Tok *t = &tl->t[i];
        if (t->type != TT_WORD || t->s[0] == '-') continue;
        if (i > 1 && strcmp(tl->t[i-1].s, "-F") == 0) continue; /* -F value, skip */
        snprintf(prog, sizeof prog, " '%s'", t->s);
        break;
    }

    if (sep)
        snprintf(out->explanation, sizeof out->explanation,
                 "Processes text line by line (field separator '%s')%s.",
                 sep, prog);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Processes text line by line%s.", prog);

    out->risk = RISK_SAFE;
}

static void analyze_cat(const TokList *tl, Analysis *out)
{
    int  idx[TOK_MAX];
    int  n = words_of(tl, 1, NULL, NULL, idx, TOK_MAX);
    char files[512];
    list_of(tl, idx, n, files, sizeof files);

    if (g_redir.out[0]) {
        g_redir.used = true;
        if (n == 0)
            snprintf(out->explanation, sizeof out->explanation,
                     g_redir.append ? "Adds the %s to the end of %s."
                                    : "Writes the %s into %s.",
                     g_redir.heredoc ? "text that follows" : "input", g_redir.out);
        else
            snprintf(out->explanation, sizeof out->explanation,
                     g_redir.append ? "Appends %s to %s." : "Copies %s into %s.",
                     files, g_redir.out);
    } else if (n == 0) {
        snprintf(out->explanation, sizeof out->explanation, "Prints %s.",
                 g_redir.heredoc ? "the text that follows" : "its input");
    } else {
        snprintf(out->explanation, sizeof out->explanation, "Prints %s.", files);
    }
    out->risk = g_redir.out[0] ? path_risk(g_redir.out) : RISK_SAFE;
}

static const char *const ends_lines[] = { "--lines", "--bytes", NULL };

/* 'a' or 'b' for a pattern that is an alternation, split on `sep` */
static void quote_alts(const char *pat, const char *sep, char *buf, size_t len)
{
    const char *parts[16];
    size_t      plen[16];
    int         n = 0;
    size_t      sl = strlen(sep);
    for (const char *p = pat; n < 16; ) {
        const char *q = strstr(p, sep);
        parts[n] = p;
        plen[n++] = q ? (size_t)(q - p) : strlen(p);
        if (!q) break;
        p = q + sl;
    }
    size_t o = 0;
    buf[0] = '\0';
    for (int k = 0; k < n && o < len; k++)
        o += (size_t)snprintf(buf + o, len - o, "%s'%.*s'",
                              k == 0 ? "" : k == n - 1 ? " or " : ", ",
                              (int)plen[k], parts[k]);
}

static void analyze_grep(const TokList *tl, Analysis *out)
{
    static const char *const vlong[] = {
        "--regexp", "--file", "--include", "--exclude", "--exclude-dir",
        "--max-count", "--context", "--after-context", "--before-context",
        "--glob", "--type", "--type-not", "--max-depth", NULL,
    };
    const char *cmd = strrchr(tl->t[0].s, '/');
    cmd = cmd ? cmd + 1 : tl->t[0].s;
    bool rg = strcmp(cmd, "rg") == 0;

    int idx[TOK_MAX];
    int n = words_of(tl, 1, rg ? "efgtmABCMT" : "efABCmdD", vlong, idx, TOK_MAX);

    const char *pattern = tok_short_val(tl, 'e');
    if (!pattern) pattern = tok_long_val(tl, "--regexp");
    int first = 0;
    if (!pattern && n > 0 && !(rg && tok_has_long(tl, "--files"))) {
        pattern = tl->t[idx[0]].s;
        first   = 1;
    }

    /* the files searched: --include, or rg's -g glob (one starting with !
     * leaves files out, it doesn't name the ones searched) or -t type */
    const char *inc = tok_long_val(tl, "--include");
    for (int i = 1; rg && !inc && i < tl->n - 1; i++)
        if ((strcmp(tl->t[i].s, "-g") == 0 || strcmp(tl->t[i].s, "--glob") == 0) &&
            tl->t[i + 1].s[0] != '!')
            inc = tl->t[i + 1].s;
    if (!inc && rg) inc = tok_short_val(tl, 't');

    bool recursive = rg || tok_has_short(tl, 'r') || tok_has_short(tl, 'R') ||
                     tok_has_long(tl, "--recursive");

    char paths[512], where[640];
    list_of(tl, idx + first, n - first, paths, sizeof paths);
    const char *base = n - first > 0 ? paths : "the current folder";
    if (recursive && inc)
        snprintf(where, sizeof where, "every '%s' file under %s", inc, base);
    else if (recursive)
        snprintf(where, sizeof where, "every file under %s", base);
    else
        snprintf(where, sizeof where, "%s", n - first > 0 ? paths : "its input");

    if (rg && tok_has_long(tl, "--files")) {
        snprintf(out->explanation, sizeof out->explanation, "Lists %s.", where);
        out->risk = RISK_SAFE;
        return;
    }

    bool ere = rg || tok_has_short(tl, 'E') || tok_has_short(tl, 'P') ||
               tok_has_long(tl, "--extended-regexp") || strcmp(cmd, "egrep") == 0;
    char pat[512];
    /* agents write grep's \| in rg and grep -E too; read it as they meant it */
    const char *alt = tok_has_short(tl, 'F') ? "\n" :
                      (!ere || (pattern && strstr(pattern, "\\|"))) ? "\\|" : "|";
    quote_alts(pattern ? pattern : "", alt, pat, sizeof pat);

    snprintf(out->explanation, sizeof out->explanation,
             "Searches %s for %s%s%s%s%s.",
             where, pat,
             tok_has_short(tl, 'i') || tok_has_long(tl, "--ignore-case")
                 ? ", ignoring case" : "",
             tok_has_short(tl, 'v') || tok_has_long(tl, "--invert-match")
                 ? ", keeping the lines that don't match" : "",
             tok_has_short(tl, 'l') || tok_has_long(tl, "--files-with-matches")
                 ? ", listing the files that match" : "",
             tok_has_short(tl, 'c') || tok_has_long(tl, "--count")
                 ? ", counting the matches" : "");
    out->risk = RISK_SAFE;
}

static void analyze_head(const TokList *tl, Analysis *out)
{
    const char *bytes = count_of(tl, 'c', "--bytes");
    const char *lines = count_of(tl, 'n', "--lines");
    int  idx[TOK_MAX];
    int  n = words_of(tl, 1, "nc", ends_lines, idx, TOK_MAX);
    char files[512];

    snprintf(out->explanation, sizeof out->explanation,
             "Prints the first %s %s of %s.",
             bytes ? bytes : lines ? lines : "10", bytes ? "bytes" : "lines",
             files_or_input(tl, idx, n, files, sizeof files));
    out->risk = RISK_SAFE;
}

static void analyze_tail(const TokList *tl, Analysis *out)
{
    const char *lines  = count_of(tl, 'n', "--lines");
    bool        follow = tok_has_short(tl, 'f') || tok_has_short(tl, 'F') ||
                         tok_has_long(tl, "--follow");
    int  idx[TOK_MAX];
    int  n = words_of(tl, 1, "nc", ends_lines, idx, TOK_MAX);
    char files[512];
    const char *what = files_or_input(tl, idx, n, files, sizeof files);

    if (lines && lines[0] == '+')
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints %s from line %s on%s.", what, lines + 1,
                 follow ? ", and keeps printing new lines" : "");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints the last %s lines of %s%s.", lines ? lines : "10", what,
                 follow ? " and keeps printing new ones" : "");
    out->risk = RISK_SAFE;
}

/* one sed address: a line number, $ or /regex/. returns the char after it */
static const char *sed_addr(const char *p, char *buf, size_t len, bool *re)
{
    size_t k = 0;
    *re = false;
    if (*p == '/') {
        *re = true;
        for (p++; *p && *p != '/'; p++) {
            if (*p == '\\' && p[1]) { if (k + 1 < len) buf[k++] = *p++; }
            if (k + 1 < len) buf[k++] = *p;
        }
        if (*p == '/') p++;
    } else {
        while ((*p >= '0' && *p <= '9') || *p == '$')
            if (k + 1 < len) buf[k++] = *p++; else p++;
    }
    buf[k] = '\0';
    return p;
}

/* the parts of s/a/b/flags, split on the delimiter after the s */
static bool sed_subst(const char *p, char *a, char *b, size_t len, bool *global)
{
    if (*p != 's' || !p[1]) return false;
    char        d = p[1];
    char       *parts[2] = { a, b };
    const char *q = p + 2;
    for (int k = 0; k < 2; k++) {
        size_t o = 0;
        for (; *q && *q != d; q++) {
            if (*q == '\\' && q[1] == d) q++;
            if (o + 1 < len) parts[k][o++] = *q;
        }
        parts[k][o] = '\0';
        if (*q != d) return false;
        q++;
    }
    *global = strchr(q, 'g') != NULL;
    return true;
}

/* "55-80 and 86-100" for a script of numeric `Np` / `N,Mp` commands */
static bool sed_ranges(const char *p, char *buf, size_t len)
{
    char   part[16][40];
    int    n = 0;
    while (*p && n < 16) {
        char a[32] = "", b[32] = "";
        bool re;
        while (*p == ' ') p++;
        p = sed_addr(p, a, sizeof a, &re);
        if (re || !a[0] || a[0] == '$') return false;
        if (*p == ',') {
            p = sed_addr(p + 1, b, sizeof b, &re);
            if (re || !b[0]) return false;
        }
        if (*p++ != 'p') return false;
        while (*p == ' ') p++;
        if (*p && *p != ';') return false;
        if (*p == ';') p++;
        if (!b[0])                    snprintf(part[n++], sizeof part[0], "%s", a);
        else if (strcmp(b, "$") == 0) snprintf(part[n++], sizeof part[0], "%s to the end", a);
        else                          snprintf(part[n++], sizeof part[0], "%s-%s", a, b);
    }
    size_t o = 0;
    buf[0] = '\0';
    for (int k = 0; k < n && o < len; k++)
        o += (size_t)snprintf(buf + o, len - o, "%s%s",
                              k == 0 ? "" : k == n - 1 ? " and " : ", ", part[k]);
    return n > 1;
}

static void analyze_sed(const TokList *tl, Analysis *out)
{
    static const char *const vlong[] = { "--expression", "--file", NULL };
    bool inplace = tok_has_long(tl, "--in-place");
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type == TT_FLAG_SHORT && strncmp(tl->t[i].s, "-i", 2) == 0)
            inplace = true;
    bool quiet = tok_has_short(tl, 'n') || tok_has_long(tl, "--quiet") ||
                 tok_has_long(tl, "--silent");

    int idx[TOK_MAX];
    int n = words_of(tl, 1, "ef", vlong, idx, TOK_MAX);

    int nscripts = 0;
    const char *script = NULL;
    for (int i = 1; i < tl->n - 1; i++)
        if (strcmp(tl->t[i].s, "-e") == 0 || strcmp(tl->t[i].s, "--expression") == 0) {
            if (!script) script = tl->t[i + 1].s;
            nscripts++;
        }
    int first = 0;
    if (!script && n > 0) { script = tl->t[idx[0]].s; first = 1; nscripts = 1; }

    char files[512];
    const char *what = files_or_input(tl, idx + first, n - first, files, sizeof files);
    const char *it   = n - first > 1 ? "them" : "it";
    char tail[64] = "";
    if (inplace) snprintf(tail, sizeof tail, ", editing %s in place", it);

    char a1[128] = "", a2[128] = "", sa[160], sb[160];
    bool re1 = false, re2 = false, global = false, done = true;
    const char *p = script ? script : "";
    while (*p == ' ') p++;

    char ranges[256];
    if (nscripts == 1 && quiet && strchr(p, ';') && sed_ranges(p, ranges, sizeof ranges)) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints lines %s of %s.", ranges, what);
    } else if (nscripts != 1 || strpbrk(p, ";\n")) {
        done = false;
    } else if (sed_subst(p, sa, sb, sizeof sa, &global)) {
        if (inplace)
            snprintf(out->explanation, sizeof out->explanation,
                     global ? "Replaces every '%s' with '%s' in %s%s."
                            : "Replaces the first '%s' on each line with '%s' in %s%s.",
                     sa, sb, what, tail);
        else
            snprintf(out->explanation, sizeof out->explanation,
                     global ? "Prints %s with every '%s' replaced by '%s'."
                            : "Prints %s with the first '%s' on each line replaced by '%s'.",
                     what, sa, sb);
    } else {
        p = sed_addr(p, a1, sizeof a1, &re1);
        if (*p == ',') p = sed_addr(p + 1, a2, sizeof a2, &re2);
        char c = *p;
        if (!a1[0] || !(c == 'p' || c == 'd') || p[1]) {
            done = false;
        } else if (c == 'p' && quiet) {
            if (re1 && a2[0])
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints %s from the line that matches '%s' to the one that matches '%s'.",
                         what, a1, a2);
            else if (re1)
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints the lines of %s that match '%s'.", what, a1);
            else if (strcmp(a2, "$") == 0)
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints %s from line %s to the end.", what, a1);
            else if (a2[0])
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints lines %s-%s of %s.", a1, a2, what);
            else
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints line %s of %s.", a1, what);
        } else if (c == 'd' && !inplace) {
            if (re1 && !a2[0])
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints %s without the lines that match '%s'.", what, a1);
            else if (a2[0])
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints %s without lines %s-%s.", what, a1, a2);
            else
                snprintf(out->explanation, sizeof out->explanation,
                         "Prints %s without line %s.", what, a1);
        } else {
            done = false;
        }
    }
    if (!done)
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs the sed script '%s' on %s%s.", script ? script : "", what, tail);

    out->risk = inplace ? RISK_MEDIUM : RISK_SAFE;
    if (inplace)
        snprintf(out->safer, sizeof out->safer,
                 "Drop -i to preview changes first, then add it once sure.");
}


static void analyze_cut(const TokList *tl, Analysis *out)
{
    const char *delim = tok_long_val(tl, "-d");
    bool fields = tok_has_short(tl, 'f');
    bool bytes  = tok_has_short(tl, 'b');

    if (bytes)
        snprintf(out->explanation, sizeof out->explanation,
                 "Extracts a byte range from each line.");
    else if (fields && delim)
        snprintf(out->explanation, sizeof out->explanation,
                 "Splits lines on '%s' and extracts the selected fields.", delim);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Extracts a column or byte range from each input line.");

    out->risk = RISK_SAFE;
}



static void analyze_ps(const TokList *tl, Analysis *out)
{
    /* ps aux: BSD-style flags come without a leading dash, so tok_has_short
     * won't find them. check for the literal token "aux" or "ax" too. */
    bool bsd_aux = false;
    for (int i = 1; i < tl->n; i++) {
        const char *s = tl->t[i].s;
        if (strcmp(s, "aux") == 0 || strcmp(s, "ax") == 0 ||
            strcmp(s, "axu") == 0 || strcmp(s, "axuf") == 0)
            bsd_aux = true;
    }
    bool aux  = bsd_aux ||
                (tok_has_short(tl, 'a') &&
                 tok_has_short(tl, 'u') &&
                 tok_has_short(tl, 'x'));
    bool ef   = tok_has_short(tl, 'e') && tok_has_short(tl, 'f');

    if (aux || ef)
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists all running processes with CPU, memory, and command details.");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists running processes for the current terminal.");

    out->risk = RISK_SAFE;
}


static void analyze_sort(const TokList *tl, Analysis *out)
{
    bool unique  = tok_has_short(tl, 'u');
    bool reverse = tok_has_short(tl, 'r');
    bool numeric = tok_has_short(tl, 'n');

    char details[128] = "";
    int  dc = 0;
    if (numeric) { snprintf(details + dc, sizeof details - (size_t)dc, "numerically"); dc = (int)strlen(details); }
    if (reverse) { snprintf(details + dc, sizeof details - (size_t)dc, "%sin reverse",  dc ? ", " : ""); dc = (int)strlen(details); }
    if (unique)  { snprintf(details + dc, sizeof details - (size_t)dc, "%s(dedup)",     dc ? " " : ""); }

    if (details[0])
        snprintf(out->explanation, sizeof out->explanation,
                 "Sorts lines %s.", details);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Sorts lines alphabetically.");

    out->risk = RISK_SAFE;
}


static void analyze_tee(const TokList *tl, Analysis *out)
{
    bool append = tok_has_short(tl, 'a');
    char file[256] = "(file)";
    for (int i = 1; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) {
            snprintf(file, sizeof file, "%s", tl->t[i].s);
            break;
        }
    }
    snprintf(out->explanation, sizeof out->explanation,
             "Writes stdin to both stdout and %s%s (useful for logging mid-pipeline).",
             file, append ? " (appending)" : "");
    out->risk = RISK_SAFE;
}

static void analyze_uniq(const TokList *tl, Analysis *out)
{
    bool dup    = tok_has_short(tl, 'd');
    bool unique = tok_has_short(tl, 'u');
    bool count  = tok_has_short(tl, 'c');

    if (dup)
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints only duplicate adjacent lines.");
    else if (unique)
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints only unique (non-repeated) adjacent lines.");
    else if (count)
        snprintf(out->explanation, sizeof out->explanation,
                 "Collapses duplicate adjacent lines and prefixes each with its count.");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Removes consecutive duplicate lines from input.");

    out->risk = RISK_SAFE;
}

static void analyze_wc(const TokList *tl, Analysis *out)
{
    bool lines = tok_has_short(tl, 'l');
    bool words = tok_has_short(tl, 'w');
    bool bytes = tok_has_short(tl, 'c');

    if (lines && !words && !bytes)
        snprintf(out->explanation, sizeof out->explanation, "Counts lines.");
    else if (words && !lines && !bytes)
        snprintf(out->explanation, sizeof out->explanation, "Counts words.");
    else if (bytes && !lines && !words)
        snprintf(out->explanation, sizeof out->explanation, "Counts bytes.");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Counts lines, words, and bytes in input.");

    out->risk = RISK_SAFE;
}

static void analyze_xargs(const TokList *tl, Analysis *out)
{
    /* find the command xargs will run - first WORD that isn't a flag or a flag value
     * known flags with values: -I, -n, -P, -L, -s, -d (we skip the following token)
     */
    static const char *const valued[] = { "-I", "-n", "-P", "-L", "-s", "-d", NULL };
    int skip_next = 0;
    const char *subcmd = "a command";
    for (int i = 1; i < tl->n; i++) {
        if (skip_next) { skip_next = 0; continue; }
        const Tok *t = &tl->t[i];
        if (t->type != TT_WORD) continue;
        if (t->s[0] == '-') {
            for (int j = 0; valued[j]; j++)
                if (strcmp(t->s, valued[j]) == 0) { skip_next = 1; break; }
            continue;
        }
        subcmd = t->s;
        break;
    }

    bool null_sep = tok_has_short(tl, '0') || tok_has_long(tl, "--null");

    /* risk follows the sub-command: rm/kill/etc are dangerous */
    Risk sub_risk = RISK_SAFE;
    if (strcmp(subcmd, "rm")    == 0 || strcmp(subcmd, "shred") == 0)  sub_risk = RISK_HIGH;
    if (strcmp(subcmd, "kill")  == 0 || strcmp(subcmd, "killall") == 0) sub_risk = RISK_HIGH;
    if (strcmp(subcmd, "chmod") == 0 || strcmp(subcmd, "chown") == 0)   sub_risk = RISK_MEDIUM;
    if (strcmp(subcmd, "sudo")  == 0)                                   sub_risk = RISK_HIGH;

    snprintf(out->explanation, sizeof out->explanation,
             "Runs %s once per input item%s.",
             subcmd,
             null_sep ? " (NUL-delimited, safe for filenames with spaces)" : "");

    out->risk = sub_risk;
    if (sub_risk >= RISK_HIGH)
        snprintf(out->warning, sizeof out->warning,
                 "xargs amplifies the effect of '%s' - one line per run.", subcmd);
    if (sub_risk >= RISK_HIGH)
        snprintf(out->safer, sizeof out->safer,
                 "Try: xargs echo %s first to preview what would be passed.", subcmd);
}

/* ------------------------------------------------------------ gcloud cli */

static void analyze_gcloud_compute(const TokList *tl, Analysis *out)
{
    const Tok *res    = tok_first_word(tl, 2);
    const char *rname = res ? res->s : NULL;
    const Tok *act    = res
        ? tok_first_word(tl, (int)(res - tl->t) + 1)
        : NULL;
    const char *aname = act ? act->s : NULL;

    if (!rname) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud Compute Engine command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(rname, "instances") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Permanently deletes one or more Compute Engine VM instances.");
            out->risk = RISK_CRITICAL;
            snprintf(out->warning, sizeof out->warning,
                     "Deleted instances and their boot disks are permanently destroyed "
                     "(unless the disk's auto-delete setting is off).");
        } else if (aname && strcmp(aname, "stop") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Stops (shuts down) one or more Compute Engine VM instances.");
            out->risk = RISK_MEDIUM;
            snprintf(out->warning, sizeof out->warning,
                     "Stopped instances still incur charges for reserved static IPs "
                     "and attached persistent disks.");
        } else if (aname && strcmp(aname, "start") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Starts one or more stopped Compute Engine VM instances.");
            out->risk = RISK_LOW;
        } else if (aname && strcmp(aname, "reset") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Hard-resets (power-cycles) Compute Engine VM instances.");
            out->risk = RISK_MEDIUM;
            snprintf(out->warning, sizeof out->warning,
                     "Hard reset is equivalent to pulling the power cord - "
                     "unsaved data may be lost.");
        } else if (aname && strcmp(aname, "create") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Creates new Compute Engine VM instances (incurs GCP charges).");
            out->risk = RISK_LOW;
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages Compute Engine VM instances.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "disks") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Permanently deletes one or more Compute Engine persistent disks.");
            out->risk = RISK_HIGH;
            snprintf(out->warning, sizeof out->warning,
                     "Disk deletion is irreversible and all stored data will be lost.");
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages Compute Engine persistent disks.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "firewall-rules") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Deletes a Compute Engine VPC firewall rule.");
            out->risk = RISK_HIGH;
            snprintf(out->warning, sizeof out->warning,
                     "Removing a firewall rule may block traffic to running instances.");
        } else if (aname && strcmp(aname, "create") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Creates a new Compute Engine VPC firewall rule.");
            out->risk = RISK_MEDIUM;
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages Compute Engine VPC firewall rules.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "snapshots") == 0 &&
               aname && strcmp(aname, "delete") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently deletes one or more disk snapshots.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Deleted snapshots cannot be recovered.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud compute %s'.", rname);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_sql(const TokList *tl, Analysis *out)
{
    const Tok *res    = tok_first_word(tl, 2);
    const char *rname = res ? res->s : NULL;
    const Tok *act    = res
        ? tok_first_word(tl, (int)(res - tl->t) + 1)
        : NULL;
    const char *aname = act ? act->s : NULL;

    if (!rname) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud Cloud SQL command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(rname, "instances") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Permanently deletes a Cloud SQL database instance.");
            out->risk = RISK_CRITICAL;
            snprintf(out->warning, sizeof out->warning,
                     "All data and automated backups for this instance are "
                     "permanently destroyed.");
        } else if (aname && strcmp(aname, "restart") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Restarts a Cloud SQL database instance.");
            out->risk = RISK_MEDIUM;
            snprintf(out->warning, sizeof out->warning,
                     "Restarting causes a brief database outage. "
                     "Plan for a maintenance window.");
        } else if (aname && strcmp(aname, "create") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Creates a new Cloud SQL instance (incurs GCP charges).");
            out->risk = RISK_LOW;
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages Cloud SQL instances.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "databases") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Permanently deletes a database from a Cloud SQL instance.");
            out->risk = RISK_HIGH;
            snprintf(out->warning, sizeof out->warning,
                     "All tables and data in this database are permanently destroyed.");
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages databases within a Cloud SQL instance.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "backups") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Deletes a Cloud SQL backup.");
            out->risk = RISK_MEDIUM;
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages Cloud SQL backups.");
            out->risk = RISK_SAFE;
        }
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud sql %s'.", rname);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_container(const TokList *tl, Analysis *out)
{
    const Tok *res    = tok_first_word(tl, 2);
    const char *rname = res ? res->s : NULL;
    const Tok *act    = res
        ? tok_first_word(tl, (int)(res - tl->t) + 1)
        : NULL;
    const char *aname = act ? act->s : NULL;

    if (!rname) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud GKE / Container command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(rname, "clusters") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Permanently deletes a GKE Kubernetes cluster.");
            out->risk = RISK_CRITICAL;
            snprintf(out->warning, sizeof out->warning,
                     "All nodes, workloads, and cluster configuration will be "
                     "destroyed. Persistent volumes may also be deleted.");
        } else if (aname && strcmp(aname, "get-credentials") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Updates the local kubeconfig with credentials for the GKE "
                     "cluster.");
            out->risk = RISK_LOW;
        } else if (aname && strcmp(aname, "create") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Creates a new GKE Kubernetes cluster (incurs GCP charges).");
            out->risk = RISK_LOW;
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages GKE clusters.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "node-pools") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Deletes a GKE node pool, draining and terminating its nodes.");
            out->risk = RISK_HIGH;
            snprintf(out->warning, sizeof out->warning,
                     "Workloads on these nodes will be evicted. Ensure they can be "
                     "rescheduled on other pools.");
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages GKE node pools.");
            out->risk = RISK_LOW;
        }
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud container %s'.", rname);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_iam(const TokList *tl, Analysis *out)
{
    const Tok *res    = tok_first_word(tl, 2);
    const char *rname = res ? res->s : NULL;
    const Tok *act    = res
        ? tok_first_word(tl, (int)(res - tl->t) + 1)
        : NULL;
    const char *aname = act ? act->s : NULL;

    if (!rname) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud IAM command.");
        out->risk = RISK_MEDIUM;
        return;
    }

    if (strcmp(rname, "service-accounts") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Permanently deletes a GCP service account.");
            out->risk = RISK_HIGH;
            snprintf(out->warning, sizeof out->warning,
                     "Services and workloads authenticating as this account will "
                     "immediately lose access.");
        } else if (aname && strcmp(aname, "create") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Creates a new GCP service account.");
            out->risk = RISK_LOW;
        } else if (aname && strcmp(aname, "keys") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages service account keys.");
            out->risk = RISK_MEDIUM;
            snprintf(out->warning, sizeof out->warning,
                     "Service account keys are long-lived credentials. "
                     "Prefer Workload Identity Federation over downloadable key "
                     "files.");
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages GCP service accounts.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "roles") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Manages GCP IAM custom roles.");
        out->risk = RISK_MEDIUM;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud iam %s'.", rname);
        out->risk = RISK_MEDIUM;
    }
}

static void analyze_gcloud_projects(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud projects command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "delete") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Schedules the entire GCP project for deletion.");
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "All resources in the project (VMs, databases, buckets, etc.) "
                 "will be permanently destroyed after a 30-day grace period. "
                 "This cannot easily be reversed.");
    } else if (strcmp(oper, "create") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new GCP project.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "list") == 0 ||
               strcmp(oper, "describe") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists or describes GCP projects (read-only).");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "add-iam-policy-binding") == 0) {
        const char *role  = tok_long_val(tl, "--role");
        bool owner = role && strstr(role, "roles/owner") != NULL;
        snprintf(out->explanation, sizeof out->explanation,
                 "Grants a GCP IAM role at the project level%s.",
                 owner ? " (roles/owner - full project control)" : "");
        out->risk = owner ? RISK_CRITICAL : RISK_HIGH;
        if (owner)
            snprintf(out->warning, sizeof out->warning,
                     "Granting 'roles/owner' gives the principal unrestricted control "
                     "over the entire GCP project and all its resources.");
        else
            snprintf(out->warning, sizeof out->warning,
                     "Project-level IAM bindings affect all resources in the project.");
    } else if (strcmp(oper, "remove-iam-policy-binding") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Revokes a GCP IAM role binding at the project level.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Revoking a binding may break access for services relying on it.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud projects %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_functions(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud Cloud Functions command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "delete") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently deletes a Cloud Function.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "All function versions and event triggers are removed.");
    } else if (strcmp(oper, "deploy") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deploys or updates a Cloud Function to production.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "The new function code is live immediately after deployment.");
    } else if (strcmp(oper, "call") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Invokes a Cloud Function (executes real code with possible "
                 "side effects).");
        out->risk = RISK_MEDIUM;
    } else if (strcmp(oper, "list") == 0   ||
               strcmp(oper, "describe") == 0 ||
               strcmp(oper, "logs") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries Cloud Function information or logs (read-only).");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud functions %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_app(const TokList *tl, Analysis *out)
{
    const Tok *res    = tok_first_word(tl, 2);
    const char *rname = res ? res->s : NULL;
    const Tok *act    = res
        ? tok_first_word(tl, (int)(res - tl->t) + 1)
        : NULL;
    const char *aname = act ? act->s : NULL;

    if (!rname) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud App Engine command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(rname, "deploy") == 0) {
        bool no_promote = tok_has_long(tl, "--no-promote");
        snprintf(out->explanation, sizeof out->explanation,
                 "Deploys an App Engine application%s.",
                 no_promote ? " (without shifting traffic)" : " to production");
        out->risk = no_promote ? RISK_LOW : RISK_MEDIUM;
        if (!no_promote) {
            snprintf(out->warning, sizeof out->warning,
                     "Traffic is migrated to the new version immediately.");
            snprintf(out->safer, sizeof out->safer,
                     "Use --no-promote to deploy first and validate before "
                     "shifting traffic.");
        }
    } else if (strcmp(rname, "versions") == 0) {
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Deletes one or more App Engine versions.");
            out->risk = RISK_MEDIUM;
        } else if (aname && strcmp(aname, "stop") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Stops one or more App Engine versions from serving traffic.");
            out->risk = RISK_MEDIUM;
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages App Engine versions.");
            out->risk = RISK_LOW;
        }
    } else if (strcmp(rname, "browse") == 0  ||
               strcmp(rname, "describe") == 0 ||
               strcmp(rname, "logs") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries App Engine information or opens the app in a browser.");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud app %s'.", rname);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_storage(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;
    bool recursive   = tok_has_short(tl, 'r') || tok_has_short(tl, 'R') ||
                       tok_has_long(tl, "--recursive");

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a gcloud Cloud Storage command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "rm") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes Cloud Storage objects%s.",
                 recursive ? " recursively" : "");
        out->risk = recursive ? RISK_HIGH : RISK_MEDIUM;
        if (recursive)
            snprintf(out->warning, sizeof out->warning,
                     "-r deletes all objects under the prefix permanently "
                     "(unless object versioning is enabled).");
    } else if (strcmp(oper, "cp") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Copies Cloud Storage objects.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "mv") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Moves (renames) Cloud Storage objects.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "ls") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists Cloud Storage buckets or objects.");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "buckets") == 0) {
        const Tok *act    = op
            ? tok_first_word(tl, (int)(op - tl->t) + 1)
            : NULL;
        const char *aname = act ? act->s : NULL;
        if (aname && strcmp(aname, "delete") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Permanently deletes a Cloud Storage bucket.");
            out->risk = RISK_HIGH;
            snprintf(out->warning, sizeof out->warning,
                     "Bucket must be empty. Deletion is irreversible.");
        } else if (aname && strcmp(aname, "create") == 0) {
            snprintf(out->explanation, sizeof out->explanation,
                     "Creates a new Cloud Storage bucket.");
            out->risk = RISK_LOW;
        } else {
            snprintf(out->explanation, sizeof out->explanation,
                     "Manages Cloud Storage buckets.");
            out->risk = RISK_LOW;
        }
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud storage %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_auth(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper || strcmp(oper, "login") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Opens a browser to authenticate with Google Cloud.");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "revoke") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Revokes GCP credentials for one or all accounts.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Revoking credentials requires re-authentication before "
                 "any further gcloud use.");
    } else if (strcmp(oper, "configure-docker") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Configures Docker to use gcloud credentials for "
                 "GCR / Artifact Registry.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "activate-service-account") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Activates a service account as the active gcloud identity.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "All subsequent gcloud commands run as this service account. "
                 "Ensure it holds only the permissions it needs.");
    } else if (strcmp(oper, "print-access-token") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Prints the current GCP OAuth2 access token to stdout.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "The token appears in terminal output and shell history. "
                 "Treat it like a password.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud auth %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_gcloud_config(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper || strcmp(oper, "list") == 0 ||
        strcmp(oper, "get-value") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Displays gcloud configuration (active project, region, "
                 "account).");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "set") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Sets a gcloud configuration property (e.g. project, region, "
                 "account).");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "unset") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Clears a gcloud configuration property.");
        out->risk = RISK_LOW;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'gcloud config %s'.", oper);
        out->risk = RISK_LOW;
    }
}

/* ----------------------------------------------------------- heroku cli */

static void analyze_heroku_destroy(const TokList *tl, Analysis *out)
{
    const char *app = tok_long_val(tl, "--app");
    if (!app) app   = tok_short_val(tl, 'a');

    if (app)
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently destroys the Heroku app '%s' and all its resources "
                 "(dynos, add-ons, config vars, release history).", app);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently destroys a Heroku app and all its resources "
                 "(dynos, add-ons, config vars, release history).");
    out->risk = RISK_CRITICAL;
    snprintf(out->warning, sizeof out->warning,
             "This action is irreversible. All dynos, add-ons, SSL certificates, "
             "config vars, and release history are permanently deleted.");
    snprintf(out->safer, sizeof out->safer,
             "You will be prompted to type the app name to confirm before deletion.");
}

static void analyze_heroku_run(const TokList *tl, Analysis *out)
{
    bool detached = tok_has_short(tl, 'd') || tok_has_long(tl, "--no-tty");

    const char *subcmd = NULL;
    for (int i = 2; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) { subcmd = tl->t[i].s; break; }
    }

    if (subcmd)
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs '%s' as a one-off dyno on Heroku%s.",
                 subcmd, detached ? " (detached)" : " (interactive)");
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a one-off command in a new Heroku dyno.");
    out->risk = RISK_MEDIUM;
    snprintf(out->warning, sizeof out->warning,
             "One-off dynos have full access to app config vars and the database. "
             "Each dyno incurs charges while it runs.");
}

static void analyze_heroku_ps_scale(const TokList *tl, Analysis *out)
{
    const char *spec = NULL;
    for (int i = 2; i < tl->n; i++) {
        if (tl->t[i].type == TT_WORD) { spec = tl->t[i].s; break; }
    }

    if (spec)
        snprintf(out->explanation, sizeof out->explanation,
                 "Scales Heroku dyno formation to %s.", spec);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Scales the Heroku dyno formation.");
    out->risk = RISK_MEDIUM;

    if (spec && strstr(spec, "=0"))
        snprintf(out->warning, sizeof out->warning,
                 "Scaling a process type to 0 stops all dynos of that type, "
                 "which may take the app offline.");
}

static void analyze_heroku_config_set(const TokList *tl, Analysis *out)
{
    (void)tl;
    snprintf(out->explanation, sizeof out->explanation,
             "Sets one or more Heroku config vars (environment variables). "
             "All dynos will restart automatically.");
    out->risk = RISK_MEDIUM;
    snprintf(out->warning, sizeof out->warning,
             "Changing config vars triggers an automatic restart of all dynos. "
             "Avoid passing secrets directly on the CLI - they end up in shell "
             "history.");
}

static void analyze_heroku_config_unset(const TokList *tl, Analysis *out)
{
    (void)tl;
    snprintf(out->explanation, sizeof out->explanation,
             "Removes one or more Heroku config vars. All dynos will restart.");
    out->risk = RISK_MEDIUM;
    snprintf(out->warning, sizeof out->warning,
             "Removing a config var that the app depends on will cause errors "
             "after the restart.");
}

static void analyze_heroku_releases_rollback(const TokList *tl, Analysis *out)
{
    const Tok *ver = tok_first_word(tl, 2);

    if (ver)
        snprintf(out->explanation, sizeof out->explanation,
                 "Rolls the Heroku app back to release %s.", ver->s);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Rolls the Heroku app back to the previous release.");
    out->risk = RISK_MEDIUM;
    snprintf(out->warning, sizeof out->warning,
             "Rollback reverts the slug and config vars only - it does NOT roll "
             "back database migrations or other stateful changes.");
}

static void analyze_heroku_pg_reset(const TokList *tl, Analysis *out)
{
    (void)tl;
    snprintf(out->explanation, sizeof out->explanation,
             "Wipes and resets a Heroku Postgres database to a blank state.");
    out->risk = RISK_CRITICAL;
    snprintf(out->warning, sizeof out->warning,
             "All data in the database will be permanently destroyed. "
             "There is no undo.");
    snprintf(out->safer, sizeof out->safer,
             "Run 'heroku pg:backups:capture' first to create a backup.");
}

static void analyze_heroku_pg_backups(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper || strcmp(oper, "info") == 0 ||
        strcmp(oper, "schedules") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists or inspects Heroku Postgres backups (read-only).");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "capture") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Captures a manual Heroku Postgres backup.");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "restore") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Restores a Heroku Postgres database from a backup.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Restoring overwrites the entire current database contents.");
    } else if (strcmp(oper, "delete") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes a Heroku Postgres backup.");
        out->risk = RISK_MEDIUM;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs a Heroku Postgres backup operation.");
        out->risk = RISK_LOW;
    }
}

static void analyze_heroku_addons_destroy(const TokList *tl, Analysis *out)
{
    const Tok *addon = tok_first_word(tl, 2);

    if (addon)
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently destroys the Heroku add-on '%s'.", addon->s);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently destroys a Heroku add-on.");
    out->risk = RISK_HIGH;
    snprintf(out->warning, sizeof out->warning,
             "All data stored in this add-on (database contents, queue messages, "
             "cache entries, etc.) will be permanently lost.");
}

static void analyze_heroku_addons_create(const TokList *tl, Analysis *out)
{
    const Tok *plan = tok_first_word(tl, 2);

    if (plan)
        snprintf(out->explanation, sizeof out->explanation,
                 "Provisions a new Heroku add-on: %s.", plan->s);
    else
        snprintf(out->explanation, sizeof out->explanation,
                 "Provisions a new Heroku add-on.");
    out->risk = RISK_LOW;
    snprintf(out->warning, sizeof out->warning,
             "Paid add-on plans will add to your monthly Heroku bill.");
}

static void analyze_heroku_logs(const TokList *tl, Analysis *out)
{
    bool tail = tok_has_short(tl, 't') || tok_has_long(tl, "--tail");
    snprintf(out->explanation, sizeof out->explanation,
             "Displays recent Heroku application logs%s.",
             tail ? " (streaming)" : "");
    out->risk = RISK_SAFE;
}

static void analyze_heroku_restart(const TokList *tl, Analysis *out)
{
    (void)tl;
    snprintf(out->explanation, sizeof out->explanation,
             "Restarts all or specific dynos for the Heroku app.");
    out->risk = RISK_MEDIUM;
    snprintf(out->warning, sizeof out->warning,
             "In-flight requests will be dropped during restart. "
             "Expect a brief period of downtime.");
}

/* --------------------------------------------------------------- aws cli */

static void analyze_aws_s3(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;
    bool recursive   = tok_has_long(tl, "--recursive");
    bool del         = tok_has_long(tl, "--delete");
    bool force       = tok_has_long(tl, "--force");
    bool dryrun      = tok_has_long(tl, "--dryrun");

    /* collect positional args that appear after the operation word */
    const char *src = NULL, *dst = NULL;
    bool past_op = false;
    for (int i = 2; i < tl->n; i++) {
        if (tl->t[i].type != TT_WORD) continue;
        if (!past_op) { past_op = true; continue; } /* skip the operation itself */
        if (!src)      src = tl->t[i].s;
        else if (!dst) dst = tl->t[i].s;
    }

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS S3 command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "ls") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Lists S3 buckets or objects%s%s.",
                 src ? " at " : "", src ? src : "");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "mb") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new S3 bucket%s%s.",
                 src ? " named " : "", src ? src : "");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "cp") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Copies S3 objects%s%s%s%s%s.",
                 src ? " from " : "", src ? src : "",
                 dst ? " to "   : "", dst ? dst : "",
                 recursive ? " (recursive)" : "");
        out->risk = dryrun ? RISK_SAFE : RISK_LOW;
    } else if (strcmp(oper, "mv") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Moves (renames) S3 object(s)%s%s%s%s.",
                 src ? " from " : "", src ? src : "",
                 dst ? " to "   : "", dst ? dst : "");
        out->risk = dryrun ? RISK_SAFE : RISK_LOW;
    } else if (strcmp(oper, "sync") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Syncs%s%s%s%s%s.",
                 src ? " "    : "", src ? src : "",
                 dst ? " to " : "", dst ? dst : "",
                 del ? " and deletes extra destination files" : "");
        if (del) {
            out->risk = dryrun ? RISK_SAFE : RISK_HIGH;
            snprintf(out->warning, sizeof out->warning,
                     "--delete removes objects in the destination that are absent in "
                     "the source. Syncing from an empty path with --delete wipes the "
                     "destination.");
            snprintf(out->safer, sizeof out->safer,
                     "Add --dryrun to preview what would be deleted first.");
        } else {
            out->risk = dryrun ? RISK_SAFE : RISK_LOW;
        }
    } else if (strcmp(oper, "rm") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes S3 object(s)%s%s%s.",
                 src ? " at " : "", src ? src : "",
                 recursive ? " recursively" : "");
        out->risk = dryrun ? RISK_SAFE : (recursive ? RISK_HIGH : RISK_MEDIUM);
        if (recursive && !dryrun) {
            snprintf(out->warning, sizeof out->warning,
                     "--recursive deletes every object under the prefix permanently "
                     "(unless versioning is enabled on the bucket).");
            snprintf(out->safer, sizeof out->safer,
                     "Add --dryrun to preview which objects would be deleted.");
        }
    } else if (strcmp(oper, "rb") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Removes S3 bucket%s%s%s.",
                 src ? " " : "", src ? src : "",
                 force ? " and all its contents (--force)" : "");
        out->risk = force ? RISK_CRITICAL : RISK_HIGH;
        if (force)
            snprintf(out->warning, sizeof out->warning,
                     "--force deletes all objects before removing the bucket. "
                     "All data is permanently and irrecoverably lost.");
        else
            snprintf(out->warning, sizeof out->warning,
                     "Bucket must be empty before removal. Bucket deletion is irreversible.");
    } else if (strcmp(oper, "presign") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Generates a time-limited pre-signed URL for an S3 object%s%s.",
                 src ? " at " : "", src ? src : "");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Anyone with this URL can access the object until it expires. "
                 "Share only with intended recipients.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws s3 %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_aws_ec2(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS EC2 command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "terminate-instances") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently terminates one or more EC2 instances.");
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "Terminated instances cannot be restarted. "
                 "Root EBS volumes are deleted by default.");
        snprintf(out->safer, sizeof out->safer,
                 "Use 'stop-instances' if you only need to pause the instance.");
    } else if (strcmp(oper, "stop-instances") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Stops one or more running EC2 instances (EBS data is retained).");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Stopping production instances causes downtime.");
    } else if (strcmp(oper, "start-instances") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Starts one or more stopped EC2 instances.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "reboot-instances") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Reboots one or more EC2 instances.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Rebooting causes a brief interruption. "
                 "Plan during a maintenance window.");
    } else if (strcmp(oper, "run-instances") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Launches new EC2 instances (will incur AWS charges).");
        out->risk = RISK_LOW;
    } else if (strncmp(oper, "describe-", 9) == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries EC2 resource information (read-only).");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "delete-security-group") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes an EC2 security group.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Removing a security group may block network access to attached instances.");
    } else if (strcmp(oper, "authorize-security-group-ingress") == 0) {
        const char *cidr    = tok_long_val(tl, "--cidr");
        bool open_world = cidr && (strcmp(cidr, "0.0.0.0/0") == 0 ||
                                   strcmp(cidr, "::/0") == 0);
        snprintf(out->explanation, sizeof out->explanation,
                 "Adds an inbound rule to a security group%s.",
                 open_world ? " open to the entire internet (0.0.0.0/0)" : "");
        out->risk = open_world ? RISK_HIGH : RISK_MEDIUM;
        if (open_world)
            snprintf(out->warning, sizeof out->warning,
                     "Allowing 0.0.0.0/0 exposes the port to the entire internet. "
                     "Restrict --cidr to known IP ranges.");
    } else if (strcmp(oper, "revoke-security-group-ingress") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Removes an inbound rule from a security group.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Removing an ingress rule may block traffic to running instances.");
    } else if (strcmp(oper, "create-key-pair") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates an EC2 key pair for SSH instance access.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Protect the private key - it grants SSH access to all instances "
                 "associated with this pair.");
    } else if (strcmp(oper, "delete-key-pair") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes an EC2 key pair.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Deleting the key pair does not remove the public key from "
                 "running instances.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws ec2 %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_aws_iam(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS IAM command.");
        out->risk = RISK_MEDIUM;
        return;
    }

    if (strncmp(oper, "list-", 5) == 0 || strncmp(oper, "get-", 4) == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries IAM information (read-only).");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "create-user") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new IAM user.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "delete-user") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently deletes an IAM user.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "All access keys, MFA devices, signing certificates, and group "
                 "memberships must be removed before the user can be deleted.");
    } else if (strcmp(oper, "create-access-key") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new AWS access key pair for programmatic API access.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Store the secret access key securely - it cannot be retrieved "
                 "again after this call.");
    } else if (strcmp(oper, "delete-access-key") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes an IAM access key.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Any application using this key will immediately lose API access.");
    } else if (strcmp(oper, "attach-user-policy") == 0 ||
               strcmp(oper, "attach-role-policy") == 0 ||
               strcmp(oper, "attach-group-policy") == 0) {
        const char *arn = tok_long_val(tl, "--policy-arn");
        bool admin = arn && strstr(arn, "AdministratorAccess") != NULL;
        snprintf(out->explanation, sizeof out->explanation,
                 "Attaches a managed IAM policy%s.",
                 admin ? " (AdministratorAccess - grants full AWS control)" : "");
        out->risk = admin ? RISK_CRITICAL : RISK_HIGH;
        if (admin)
            snprintf(out->warning, sizeof out->warning,
                     "AdministratorAccess grants unrestricted access to all AWS "
                     "services and resources. Apply least-privilege policies instead.");
        else
            snprintf(out->warning, sizeof out->warning,
                     "Attaching policies expands what a principal can do across "
                     "your entire AWS account.");
    } else if (strcmp(oper, "put-user-policy") == 0 ||
               strcmp(oper, "put-role-policy") == 0 ||
               strcmp(oper, "put-group-policy") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Embeds an inline policy directly into a user, role, or group.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Inline policies can grant broad permissions and are harder to "
                 "audit than managed policies.");
    } else if (strcmp(oper, "create-role") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new IAM role.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "delete-role") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently deletes an IAM role.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Services and resources assuming this role will immediately "
                 "lose access.");
    } else if (strcmp(oper, "update-assume-role-policy") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Updates the trust policy that controls who can assume an IAM role.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "Changing the trust policy can grant external accounts the ability "
                 "to assume this role.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws iam %s'.", oper);
        out->risk = RISK_MEDIUM;
    }
}

static void analyze_aws_rds(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS RDS command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "delete-db-instance") == 0) {
        bool no_snap = tok_has_long(tl, "--skip-final-snapshot");
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes an RDS database instance%s.",
                 no_snap ? " without a final snapshot" : "");
        out->risk = no_snap ? RISK_CRITICAL : RISK_HIGH;
        if (no_snap) {
            snprintf(out->warning, sizeof out->warning,
                     "--skip-final-snapshot means all data is permanently destroyed "
                     "with no recovery point.");
            snprintf(out->safer, sizeof out->safer,
                     "Remove --skip-final-snapshot to take a final backup before deletion.");
        } else {
            snprintf(out->warning, sizeof out->warning,
                     "RDS deletion is irreversible. A final snapshot will be created.");
        }
    } else if (strcmp(oper, "delete-db-cluster") == 0) {
        bool no_snap = tok_has_long(tl, "--skip-final-snapshot");
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes an entire RDS Aurora cluster%s.",
                 no_snap ? " without a final snapshot" : "");
        out->risk = no_snap ? RISK_CRITICAL : RISK_HIGH;
        if (no_snap)
            snprintf(out->warning, sizeof out->warning,
                     "--skip-final-snapshot: all Aurora cluster data will be "
                     "permanently lost with no recovery option.");
    } else if (strcmp(oper, "create-db-instance") == 0 ||
               strcmp(oper, "create-db-cluster") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new RDS database instance or cluster "
                 "(incurs AWS charges).");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "reboot-db-instance") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Reboots an RDS database instance.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Rebooting causes a brief database outage. "
                 "Plan for a maintenance window.");
    } else if (strcmp(oper, "restore-db-instance-from-db-snapshot") == 0 ||
               strcmp(oper, "restore-db-instance-to-point-in-time") == 0 ||
               strcmp(oper, "restore-db-cluster-from-snapshot") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Restores an RDS database from a snapshot or point-in-time backup.");
        out->risk = RISK_LOW;
    } else if (strncmp(oper, "describe-", 9) == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries RDS resource information (read-only).");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws rds %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_aws_lambda(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS Lambda command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "delete-function") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently deletes a Lambda function, all its versions "
                 "and aliases.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "All versions, aliases, and associated event source mappings "
                 "are removed.");
    } else if (strcmp(oper, "invoke") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Invokes a Lambda function (may trigger real side effects).");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Lambda invocations execute real code and may modify databases, "
                 "send messages, or interact with other AWS services.");
    } else if (strcmp(oper, "update-function-code") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Replaces the deployment package of a live Lambda function.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Updated code is live immediately on the default alias.");
    } else if (strcmp(oper, "create-function") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new Lambda function.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "add-permission") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Adds a resource-based permission to a Lambda function.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "This controls which AWS services or accounts can invoke the "
                 "function.");
    } else if (strncmp(oper, "list-", 5) == 0 ||
               strncmp(oper, "get-",  4) == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries Lambda information (read-only).");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws lambda %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_aws_cloudformation(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS CloudFormation command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "delete-stack") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes a CloudFormation stack and all AWS resources it manages.");
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "Every AWS resource created by this stack (EC2, RDS, S3, etc.) "
                 "will be permanently destroyed.");
        snprintf(out->safer, sizeof out->safer,
                 "Run 'aws cloudformation describe-stack-resources --stack-name NAME' "
                 "first to review what will be deleted.");
    } else if (strcmp(oper, "deploy") == 0 ||
               strcmp(oper, "create-stack") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deploys or creates a CloudFormation stack "
                 "(provisions real AWS resources).");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "New AWS resources will be provisioned and charges will be incurred.");
    } else if (strcmp(oper, "update-stack") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Updates an existing CloudFormation stack in-place.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Stack updates can replace or delete existing resources depending "
                 "on the template changes.");
        snprintf(out->safer, sizeof out->safer,
                 "Use 'create-change-set' then 'describe-change-set' to preview "
                 "changes before applying.");
    } else if (strcmp(oper, "create-change-set") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a change set to preview CloudFormation stack updates "
                 "before applying them.");
        out->risk = RISK_SAFE;
    } else if (strncmp(oper, "describe-", 9) == 0 ||
               strncmp(oper, "list-",    5) == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries CloudFormation stack information (read-only).");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws cloudformation %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_aws_eks(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS EKS command.");
        out->risk = RISK_LOW;
        return;
    }

    if (strcmp(oper, "delete-cluster") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Permanently deletes an EKS Kubernetes cluster.");
        out->risk = RISK_CRITICAL;
        snprintf(out->warning, sizeof out->warning,
                 "All node groups, add-ons, and workloads will be destroyed. "
                 "Persistent volumes backed by EBS may also be deleted.");
    } else if (strcmp(oper, "delete-nodegroup") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes an EKS managed node group, terminating its EC2 instances.");
        out->risk = RISK_HIGH;
        snprintf(out->warning, sizeof out->warning,
                 "All EC2 instances in this node group will be terminated and "
                 "workloads evicted.");
    } else if (strcmp(oper, "update-kubeconfig") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Updates the local kubeconfig to grant kubectl access to an EKS "
                 "cluster.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "create-cluster") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Creates a new EKS Kubernetes cluster (incurs AWS charges).");
        out->risk = RISK_LOW;
    } else if (strncmp(oper, "describe-", 9) == 0 ||
               strncmp(oper, "list-",    5) == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries EKS cluster information (read-only).");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws eks %s'.", oper);
        out->risk = RISK_LOW;
    }
}

static void analyze_aws_secretsmanager(const TokList *tl, Analysis *out)
{
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs an AWS Secrets Manager command.");
        out->risk = RISK_MEDIUM;
        return;
    }

    if (strcmp(oper, "delete-secret") == 0) {
        bool no_recovery = tok_has_long(tl,
                               "--force-delete-without-recovery");
        snprintf(out->explanation, sizeof out->explanation,
                 "Deletes a secret from AWS Secrets Manager%s.",
                 no_recovery ? " immediately (no recovery window)" : "");
        out->risk = no_recovery ? RISK_CRITICAL : RISK_HIGH;
        if (no_recovery)
            snprintf(out->warning, sizeof out->warning,
                     "--force-delete-without-recovery bypasses the 7-30 day "
                     "recovery window. The secret cannot be restored.");
        else
            snprintf(out->warning, sizeof out->warning,
                     "Secret is recoverable for 7-30 days before permanent "
                     "deletion.");
    } else if (strcmp(oper, "get-secret-value") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Retrieves a plaintext secret value from AWS Secrets Manager.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "The secret value will appear in terminal output and shell history. "
                 "Avoid logging or redirecting to insecure locations.");
    } else if (strcmp(oper, "create-secret") == 0 ||
               strcmp(oper, "put-secret-value") == 0 ||
               strcmp(oper, "update-secret") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Stores or updates a secret value in AWS Secrets Manager.");
        out->risk = RISK_LOW;
    } else if (strncmp(oper, "list-",    5) == 0 ||
               strncmp(oper, "describe-", 9) == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Queries Secrets Manager information (read-only).");
        out->risk = RISK_SAFE;
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Runs 'aws secretsmanager %s'.", oper);
        out->risk = RISK_MEDIUM;
    }
}

static void analyze_aws_configure(const TokList *tl, Analysis *out)
{
    /* tl: aws configure [set|get|list|import|...] [key [value]] */
    const Tok *op    = tok_first_word(tl, 2);
    const char *oper = op ? op->s : NULL;

    if (!oper || strcmp(oper, "list") == 0 ||
        strcmp(oper, "list-profiles") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Displays the current AWS CLI configuration "
                 "(profile, region, output format, credentials source).");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "get") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Reads a single AWS CLI configuration value.");
        out->risk = RISK_SAFE;
    } else if (strcmp(oper, "set") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Writes a value to the AWS CLI configuration file.");
        out->risk = RISK_LOW;
    } else if (strcmp(oper, "import") == 0) {
        snprintf(out->explanation, sizeof out->explanation,
                 "Imports AWS credentials from a CSV file into the config.");
        out->risk = RISK_MEDIUM;
        snprintf(out->warning, sizeof out->warning,
                 "Imported credentials overwrite the existing ones for "
                 "the profile.");
    } else {
        snprintf(out->explanation, sizeof out->explanation,
                 "Configures the AWS CLI interactively (profile, region, "
                 "credentials, output format).");
        out->risk = RISK_LOW;
    }
}

static CmdEntry cmd_table[] = {

    /* shell builtins & everyday utils */
    { "cat",            analyze_cat           },
    { "cd",             analyze_cd            },
    { "echo",           analyze_echo          },
    { "env",            analyze_env           },
    { "export",         analyze_export        },
    { "ls",             analyze_ls            },
    { "mkdir",          analyze_mkdir         },
    { "nohup",          analyze_nohup         },
    { "printf",         analyze_printf        },
    { "source",         analyze_source        },
    { "touch",          analyze_touch         },
    { "which",          analyze_which         },

    /* file operations */
    { "chattr",         analyze_chattr        },
    { "chmod",          analyze_chmod         },
    { "chown",          analyze_chown         },
    { "cp",             analyze_cp            },
    { "dd",             analyze_dd            },
    { "find",           analyze_find          },
    { "ln",             analyze_ln            },
    { "mv",             analyze_mv            },
    { "rm",             analyze_rm            },
    { "rsync",          analyze_rsync         },
    { "tar",            analyze_tar           },
    { "unzip",          analyze_unzip         },

    /* text processing (pipeline staples) */
    { "awk",            analyze_awk           },
    { "cut",            analyze_cut           },
    { "grep",           analyze_grep          },
    { "rg",             analyze_grep          },
    { "head",           analyze_head          },
    { "sed",            analyze_sed           },
    { "sort",           analyze_sort          },
    { "tail",           analyze_tail          },
    { "tee",            analyze_tee           },
    { "uniq",           analyze_uniq          },
    { "wc",             analyze_wc            },
    { "xargs",          analyze_xargs         },

    /* system info / monitoring */
    { "df",             analyze_df            },
    { "du",             analyze_du            },
    { "journalctl",     analyze_journalctl    },
    { "lsblk",          analyze_lsblk         },
    { "ps",             analyze_ps            },

    /* process management */
    { "kill",           analyze_kill          },
    { "killall",        analyze_killall       },
    { "pkill",          analyze_pkill         },

    /* network */
    { "curl",           analyze_curl          },
    { "ip",             analyze_ip            },
    { "nc",             analyze_nc            },
    { "netcat",         analyze_nc            },
    { "ping",           analyze_ping          },
    { "scp",            analyze_scp           },
    { "ssh",            analyze_ssh           },
    { "wget",           analyze_wget          },

    /* filesystem / disk management */
    { "chroot",         analyze_chroot        },
    { "fdisk",          analyze_fdisk         },
    { "mkfs",           analyze_mkfs          },
    { "mount",          analyze_mount         },
    { "parted",         analyze_parted        },
    { "umount",         analyze_umount        },

    /* system administration */
    { "adduser",        analyze_useradd       },
    { "chroot",         analyze_chroot        },  /* duplicate intentional - alias */
    { "crontab",        analyze_crontab       },
    { "iptables",       analyze_iptables      },
    { "sudo",           analyze_sudo          },
    { "systemctl",      analyze_systemctl     },
    { "useradd",        analyze_useradd       },
    { "userdel",        analyze_userdel       },

    /* script / code execution - the scary ones */
    { "base64",         analyze_base64        },
    { "bash",           analyze_bash          },
    { "eval",           analyze_eval          },
    { "python",         analyze_python        },
    { "python3",        analyze_python        },
    { "sh",             analyze_bash          },

    /* package managers */
    { "apt",            analyze_apt           },
    { "apt-get",        analyze_apt           },
    { "npm",            analyze_npm           },
    { "pip",            analyze_pip           },
    { "pip3",           analyze_pip           },

    /* git */
    { "git add",        analyze_git_add       },
    { "git branch",     analyze_git_branch    },
    { "git checkout",   analyze_git_checkout  },
    { "git clean",      analyze_git_clean     },
    { "git clone",      analyze_git_clone     },
    { "git commit",     analyze_git_commit    },
    { "git diff",       analyze_git_diff      },
    { "git log",        analyze_git_log       },
    { "git rev-parse",  analyze_git_rev_parse },
    { "git show",       analyze_git_show      },
    { "git status",     analyze_git_status    },
    { "git merge",      analyze_git_merge     },
    { "git pull",       analyze_git_pull      },
    { "git push",       analyze_git_push      },
    { "git rebase",     analyze_git_rebase    },
    { "git reset",      analyze_git_reset     },
    { "git stash",      analyze_git_stash     },

    /* gcloud cli */
    { "gcloud app",       analyze_gcloud_app       },
    { "gcloud auth",      analyze_gcloud_auth      },
    { "gcloud compute",   analyze_gcloud_compute   },
    { "gcloud config",    analyze_gcloud_config    },
    { "gcloud container", analyze_gcloud_container },
    { "gcloud functions", analyze_gcloud_functions },
    { "gcloud iam",       analyze_gcloud_iam       },
    { "gcloud projects",  analyze_gcloud_projects  },
    { "gcloud sql",       analyze_gcloud_sql       },
    { "gcloud storage",   analyze_gcloud_storage   },

    /* heroku cli */
    { "heroku addons:create",      analyze_heroku_addons_create      },
    { "heroku addons:destroy",     analyze_heroku_addons_destroy     },
    { "heroku config:set",         analyze_heroku_config_set         },
    { "heroku config:unset",       analyze_heroku_config_unset       },
    { "heroku destroy",            analyze_heroku_destroy            },
    { "heroku logs",               analyze_heroku_logs               },
    { "heroku pg:backups",         analyze_heroku_pg_backups         },
    { "heroku pg:reset",           analyze_heroku_pg_reset           },
    { "heroku ps:scale",           analyze_heroku_ps_scale           },
    { "heroku releases:rollback",  analyze_heroku_releases_rollback  },
    { "heroku restart",            analyze_heroku_restart            },
    { "heroku run",                analyze_heroku_run                },

    /* aws cli */
    { "aws cloudformation", analyze_aws_cloudformation },
    { "aws configure",      analyze_aws_configure      },
    { "aws ec2",            analyze_aws_ec2            },
    { "aws eks",            analyze_aws_eks            },
    { "aws iam",            analyze_aws_iam            },
    { "aws lambda",         analyze_aws_lambda         },
    { "aws rds",            analyze_aws_rds            },
    { "aws s3",             analyze_aws_s3             },
    { "aws secretsmanager", analyze_aws_secretsmanager },

    /* sheer itself - why not */
    { "sheer",          analyze_sheer         },
    { "shrun",          analyze_shrun         },

    /* docker */
    { "docker build",   analyze_docker_build  },
    { "docker exec",    analyze_docker_exec   },
    { "docker ps",      analyze_docker_ps     },
    { "docker rm",      analyze_docker_rm     },
    { "docker run",     analyze_docker_run    },
    { "docker stop",    analyze_docker_stop   },
};

#define TABLE_LEN ((int)(sizeof cmd_table / sizeof cmd_table[0]))

/* bsearch: key is a plain string, entry is CmdEntry */
static int entry_cmp(const void *key, const void *entry)
{
    return strcmp((const char *)key, ((const CmdEntry *)entry)->key);
}

/* qsort: both sides are CmdEntry */
static int entry_cmp_sort(const void *a, const void *b)
{
    return strcmp(((const CmdEntry *)a)->key, ((const CmdEntry *)b)->key);
}

static void ensure_table_sorted(void)
{
    static bool done = false;
    if (done) return;
    qsort(cmd_table, (size_t)TABLE_LEN, sizeof cmd_table[0], entry_cmp_sort);
    done = true;
}

static bool is_compound_prefix(const char *name)
{
    static const char *const prefixes[] = {
        "aws", "docker", "gcloud", "git", "heroku", "kubectl", "npm", "pip",
    };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (strcmp(name, prefixes[i]) == 0) return true;
    return false;
}

static void analyze_one(const TokList *tl, Analysis *out);

/* append ", in <dir>" before the explanation's final period */
static void say_where(Analysis *out, const char *dir)
{
    size_t l = strlen(out->explanation);
    if (l && out->explanation[l - 1] == '.') out->explanation[--l] = '\0';
    snprintf(out->explanation + l, sizeof out->explanation - l, ", in %s.", dir);
}

void cmd_analyze(const TokList *raw, Analysis *out)
{
    static TokList tl;
    redir_strip(raw, &tl, &g_redir);
    g_gitdir[0] = '\0';
    analyze_one(&tl, out);
    if (!out->matched) return;

    if (g_gitdir[0]) say_where(out, g_gitdir);
    if (g_redir.out[0] && !g_redir.used) {
        size_t l = strlen(out->explanation);
        snprintf(out->explanation + l, sizeof out->explanation - l, " %s the output to %s.",
                 g_redir.append ? "Appends" : "Writes", g_redir.out);
        out->risk = risk_max(out->risk, path_risk(g_redir.out));
    }
}

static void analyze_one(const TokList *tl, Analysis *out)
{
    ensure_table_sorted();
    memset(out, 0, sizeof *out);

    if (tl->n == 0) return;

    const char *raw   = tl->t[0].s;
    const char *slash = strrchr(raw, '/');
    const char *cmd   = slash ? slash + 1 : raw; /* strip path prefix */

    if (strncmp(cmd, "mkfs", 4) == 0) {
        analyze_mkfs(tl, out);
        out->matched = true;
        snprintf(out->source, sizeof out->source, "local rules");
        return;
    }

    /* ip6tables: same semantics as iptables */
    if (strcmp(cmd, "ip6tables") == 0) {
        analyze_iptables(tl, out);
        out->matched = true;
        snprintf(out->source, sizeof out->source, "local rules");
        return;
    }

    char key[128];

    if (is_compound_prefix(cmd)) {
        const char *sub = NULL;
        /* skip flags between cmd and subcommand, e.g. git -C /path reset */
        for (int i = 1; i < tl->n; i++) {
            const char *f = tl->t[i].s;
            if (strcmp(cmd, "git") == 0 && i + 1 < tl->n &&
                (strcmp(f, "-C") == 0 || strcmp(f, "-c") == 0 ||
                 strcmp(f, "--git-dir") == 0 || strcmp(f, "--work-tree") == 0)) {
                if (f[1] == 'C') snprintf(g_gitdir, sizeof g_gitdir, "%s", tl->t[i + 1].s);
                i++;
                continue;
            }
            if (tl->t[i].type == TT_WORD) { sub = f; g_sub = i; break; }
        }
        if (sub) {
            snprintf(key, sizeof key, "%s %s", cmd, sub);
            const CmdEntry *e = bsearch(key, cmd_table,
                                        (size_t)TABLE_LEN, sizeof cmd_table[0],
                                        entry_cmp);
            if (e) {
                e->fn(tl, out);
                out->matched = true;
                snprintf(out->source, sizeof out->source, "local rules");
                return;
            }
        }
    }

    snprintf(key, sizeof key, "%s", cmd);
    const CmdEntry *e = bsearch(key, cmd_table,
                                (size_t)TABLE_LEN, sizeof cmd_table[0],
                                entry_cmp);
    if (e) {
        e->fn(tl, out);
        out->matched = true;
        snprintf(out->source, sizeof out->source, "local rules");
    }
}

/* ---- --plain: a pipe read as one action ---- */

static const char *base_name(const TokList *tl)
{
    const char *s = strrchr(tl->t[0].s, '/');
    return s ? s + 1 : tl->t[0].s;
}

bool cmd_clause(const TokList *raw, char *out, size_t len)
{
    static TokList tl;
    Redir r;
    redir_strip(raw, &tl, &r);
    out[0] = '\0';
    if (tl.n == 0) return false;
    const char *cmd = base_name(&tl);
    int idx[TOK_MAX];

    if (strcmp(cmd, "head") == 0 || strcmp(cmd, "tail") == 0) {
        if (words_of(&tl, 1, "nc", ends_lines, idx, TOK_MAX) > 0) return false;
        bool head = cmd[0] == 'h';
        const char *bytes = head ? count_of(&tl, 'c', "--bytes") : NULL;
        const char *lines = count_of(&tl, 'n', "--lines");
        if (!head && lines && lines[0] == '+')
            snprintf(out, len, ", from line %s on", lines + 1);
        else
            snprintf(out, len, ", showing the %s %s %s", head ? "first" : "last",
                     bytes ? bytes : lines ? lines : "10", bytes ? "bytes" : "lines");
        return true;
    }
    if (strcmp(cmd, "grep") == 0 || strcmp(cmd, "egrep") == 0) {
        static const char *const vlong[] = { "--regexp", "--max-count", "--context",
            "--after-context", "--before-context", NULL };
        int n = words_of(&tl, 1, "efABCm", vlong, idx, TOK_MAX);
        const char *pattern = tok_short_val(&tl, 'e');
        if (!pattern) pattern = tok_long_val(&tl, "--regexp");
        int files = n - (pattern ? 0 : 1);
        if (!pattern && n > 0) pattern = tl.t[idx[0]].s;
        if (!pattern || files > 0 || tok_has_short(&tl, 'r') || tok_has_short(&tl, 'R'))
            return false;
        bool ere = tok_has_short(&tl, 'E') || strcmp(cmd, "egrep") == 0;
        char pat[512];
        quote_alts(pattern, tok_has_short(&tl, 'F') ? "\n" :
                   (!ere || strstr(pattern, "\\|")) ? "\\|" : "|", pat, sizeof pat);
        const char *how = tok_has_short(&tl, 'v') ? "without the lines matching" :
                          tok_has_short(&tl, 'c') ? "counting the lines matching" :
                                                    "keeping the lines matching";
        snprintf(out, len, ", %s %s%s", how, pat,
                 tok_has_short(&tl, 'i') ? ", ignoring case" : "");
        return true;
    }
    if (strcmp(cmd, "wc") == 0) {
        if (words_of(&tl, 1, "", NULL, idx, TOK_MAX) > 0) return false;
        bool l = tok_has_short(&tl, 'l'), w = tok_has_short(&tl, 'w'),
             c = tok_has_short(&tl, 'c');
        snprintf(out, len, ", counting the %s",
                 l && !w && !c ? "lines" : w && !l && !c ? "words" :
                 c && !l && !w ? "bytes" : "lines, words and bytes");
        return true;
    }
    if (strcmp(cmd, "sort") == 0) {
        if (words_of(&tl, 1, "kto", NULL, idx, TOK_MAX) > 0) return false;
        snprintf(out, len, ", sorted%s%s%s",
                 tok_has_short(&tl, 'n') ? " by number" : "",
                 tok_has_short(&tl, 'r') ? " in reverse" : "",
                 tok_has_short(&tl, 'u') ? ", without repeats" : "");
        return true;
    }
    if (strcmp(cmd, "uniq") == 0) {
        if (words_of(&tl, 1, "fs", NULL, idx, TOK_MAX) > 0) return false;
        snprintf(out, len, "%s",
                 tok_has_short(&tl, 'c') ? ", counting repeats" :
                 tok_has_short(&tl, 'd') ? ", only the lines that repeat" :
                 tok_has_short(&tl, 'u') ? ", only the lines that don't repeat" :
                                           ", without repeats");
        return true;
    }
    if (strcmp(cmd, "cat") == 0)
        return tl.n == 1;   /* passes its input on: nothing to say */
    if (strcmp(cmd, "sed") == 0) {
        /* sed -n 'A,Bp' or 'Ap' on its input: a range of lines */
        int n = words_of(&tl, 1, "e", NULL, idx, TOK_MAX);
        if (!tok_has_short(&tl, 'n') || n != 1) return false;
        unsigned a = 0, b = 0;
        char p = 0;
        const char *s = tl.t[idx[0]].s;
        if (sscanf(s, "%u,%u%c", &a, &b, &p) == 3 && p == 'p' && !s[strcspn(s, "p") + 1])
            snprintf(out, len, ", lines %u-%u", a, b);
        else if (sscanf(s, "%u,$%c", &a, &p) == 2 && p == 'p' && !s[strcspn(s, "p") + 1])
            snprintf(out, len, ", from line %u on", a);
        else if (sscanf(s, "%u%c", &a, &p) == 2 && p == 'p' && !s[strcspn(s, "p") + 1])
            snprintf(out, len, ", line %u", a);
        else return false;
        return true;
    }
    if (strcmp(cmd, "cut") == 0) {
        if (words_of(&tl, 1, "dfcb", NULL, idx, TOK_MAX) > 0) return false;
        const char *f = short_val(&tl, 'f'), *c = short_val(&tl, 'c');
        if (f)      snprintf(out, len, ", only field%s %s", strpbrk(f, ",-") ? "s" : "", f);
        else if (c) snprintf(out, len, ", only characters %s", c);
        else return false;
        return true;
    }
    if (strcmp(cmd, "tee") == 0) {
        int n = words_of(&tl, 1, "", NULL, idx, TOK_MAX);
        if (n != 1) return false;
        snprintf(out, len, ", also %s it to %s",
                 tok_has_short(&tl, 'a') ? "appending" : "saving", tl.t[idx[0]].s);
        return true;
    }
    return false;
}

/* an echo whose text is only a line of marks, or a label between them
 * ("---", "=== tests ===") */
static bool separator(const TokList *tl)
{
    char text[TOK_SLEN] = "";
    for (int i = 1; i < tl->n; i++)
        if (tl->t[i].type != TT_FLAG_SHORT || i > 1 || strlen(tl->t[i].s) > 2) {
            snprintf(text, sizeof text, "%s", tl->t[i].s);
            break;
        }
    const char *marks = "-=#*_~+";
    size_t lead = strspn(text, marks);
    if (text[0] && text[lead] == '\0') return true;
    return lead >= 3;
}

bool cmd_quiet(const TokList *raw)
{
    static TokList tl;
    Redir r;
    redir_strip(raw, &tl, &r);
    if (tl.n == 0) return false;
    const char *cmd = base_name(&tl);
    static const char *const noise[] = { "cd", "pushd", "popd", "sleep", "true",
                                         ":", "set", NULL };
    for (int i = 0; noise[i]; i++)
        if (strcmp(cmd, noise[i]) == 0) return true;
    return (strcmp(cmd, "echo") == 0 || strcmp(cmd, "printf") == 0) && separator(&tl);
}
