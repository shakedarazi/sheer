#include "token.h"
#include "cmd.h"
#include "out.h"
#include "llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "1.0.0"

/*
 * config file: ~/.config/sheer/config  (or $XDG_CONFIG_HOME/sheer/config)
 * fallback:    ~/.sheerrc
 *
 * format: key=value, # comments, blank lines ignored.
 *
 *   llm=ollama
 *   model=llama3
 *   no-color=true
 *
 * CLI flags always win over config, config wins over env vars.
 */
typedef struct {
    char llm[256];
    char model[128];
    bool no_color;
} Config;

static void config_load(Config *cfg); /* forward - defined below config_set/show */

static void config_path(char *out, size_t len)
{
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg)  snprintf(out, len, "%s/sheer/config", xdg);
    else if (home) snprintf(out, len, "%s/.config/sheer/config", home);
    else out[0] = '\0';
}

/*
 * sheer config key=value  - write a key into the config file.
 * replaces the line if the key exists, appends if not.
 * creates the file (and directory) if missing.
 */
static int config_set(const char *kv)
{
    char path[512];
    config_path(path, sizeof path);
    if (!path[0]) { fprintf(stderr, "sheer: can't find HOME\n"); return 1; }

    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        /* mkdir -p equivalent for one level - good enough */
        char mk[560];
        snprintf(mk, sizeof mk, "mkdir -p '%s'", dir);
        int _unused = system(mk); (void)_unused;
    }

    char key[128] = "", val[256] = "";
    const char *eq = strchr(kv, '=');
    if (!eq) {
        fprintf(stderr, "sheer config: expected key=value, got '%s'\n", kv);
        return 1;
    }
    snprintf(key, sizeof key, "%.*s", (int)(eq - kv), kv);
    snprintf(val, sizeof val, "%s", eq + 1);

    char  lines[64][512];
    int   n    = 0;
    bool  found = false;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f) && n < 64) {
            line[strcspn(line, "\n")] = '\0';

            /* does this line already have our key? replace it */
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            char *leq = strchr(p, '=');
            if (leq && *p != '#') {
                char lkey[128];
                snprintf(lkey, sizeof lkey, "%.*s", (int)(leq - p), p);
                if (strcmp(lkey, key) == 0) {
                    snprintf(lines[n++], 512, "%s=%s", key, val);
                    found = true;
                    continue;
                }
            }
            snprintf(lines[n++], 512, "%s", line);
        }
        fclose(f);
    }

    if (!found) snprintf(lines[n++], 512, "%s=%s", key, val);

    f = fopen(path, "w");
    if (!f) { perror("sheer config"); return 1; }
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);

    printf("  set %s=%s  (in %s)\n", key, val, path);
    return 0;
}

/*
 * sheer config  - print current config and its source file.
 */
static int config_show(void)
{
    Config cfg;
    config_load(&cfg);

    char path[512];
    config_path(path, sizeof path);

    printf("\n");
    if (cfg.llm[0])   printf("  llm      %s\n", cfg.llm);
    else              printf("  llm      (not set - use --llm or set llm=... in config)\n");
    if (cfg.model[0]) printf("  model    %s\n", cfg.model);
    else              printf("  model    (not set - will use SHEER_LLM_MODEL or default)\n");
    printf("  no-color %s\n", cfg.no_color ? "true" : "false");
    printf("\n  config   %s\n\n", path[0] ? path : "(not found)");
    return 0;
}

static void config_load(Config *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    /* find the file */
    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg)
        snprintf(path, sizeof path, "%s/sheer/config", xdg);
    else if (home)
        snprintf(path, sizeof path, "%s/.config/sheer/config", home);
    else
        return;

    FILE *f = fopen(path, "r");
    if (!f && home) {
        /* try ~/.sheerrc */
        snprintf(path, sizeof path, "%s/.sheerrc", home);
        f = fopen(path, "r");
    }
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = p;
        const char *val = eq + 1;

        if (strcmp(key, "llm") == 0)
            snprintf(cfg->llm, sizeof cfg->llm, "%s", val);
        else if (strcmp(key, "model") == 0)
            snprintf(cfg->model, sizeof cfg->model, "%s", val);
        else if (strcmp(key, "no-color") == 0)
            cfg->no_color = strcmp(val, "true") == 0 || strcmp(val, "1") == 0;
    }
    fclose(f);
}

/*
 * split `input` on |, &&, ||, ; (not inside quotes).
 * stores each stage in stages[i] and the separator that preceded it in seps[i].
 * seps[0] is always meaningless (no separator before the first stage).
 */
#define MAX_STAGES 16

/*
 * cut the body of each unquoted here-doc out of the line, keeping its
 * `<<EOF` marker: the body is text, not commands, and its own | and &&
 * would otherwise split it into stages. a here-doc inside quotes, as in
 * git commit -m "$(cat <<'EOF' ...)", stays whole: it is one argument.
 */
static void strip_heredocs(const char *in, char *out, size_t len)
{
    size_t o = 0;
    bool in_sq = false, in_dq = false;
    char delim[64] = "";

    for (const char *p = in; *p && o + 1 < len; p++) {
        char c = *p;
        if (c == '\\' && !in_sq && p[1] && o + 2 < len) {
            out[o++] = c;
            out[o++] = *++p;
            continue;
        }
        if (c == '\'' && !in_dq) in_sq = !in_sq;
        else if (c == '"' && !in_sq) in_dq = !in_dq;
        else if (!in_sq && !in_dq && c == '<' && p[1] == '<' && p[2] != '<') {
            const char *d = p + 2;
            if (*d == '-') d++;
            while (*d == ' ') d++;
            char q = (*d == '\'' || *d == '"') ? *d++ : 0;
            size_t dl = 0;
            while (*d && *d != '\n' && *d != ' ' && *d != q && dl + 1 < sizeof delim)
                delim[dl++] = *d++;
            delim[dl] = '\0';
        } else if (c == '\n' && delim[0] && !in_sq && !in_dq) {
            /* skip whole lines until the one that is the delimiter */
            const char *line = p + 1;
            for (;;) {
                const char *end = strchr(line, '\n');
                size_t      ll  = end ? (size_t)(end - line) : strlen(line);
                const char *t   = line;
                while (*t == '\t' || *t == ' ') { t++; ll--; }
                bool last = ll == strlen(delim) && strncmp(t, delim, ll) == 0;
                if (!end) { p = line + strlen(line) - 1; break; }
                line = end + 1;
                if (last) { p = end - 1; break; }
            }
            delim[0] = '\0';
            continue;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

static int cmd_split(const char *raw, char stages[][512], SepKind seps[], int max)
{
    int  n     = 0;
    char buf[512];
    int  blen  = 0;
    bool in_sq = false, in_dq = false;
    SepKind next_sep = SEP_SEMI; /* sep before stages[0] - ignored */
    char input[8192];
    strip_heredocs(raw, input, sizeof input);

    for (const char *p = input; ; p++) {
        char c = *p;

        if (c == '\\' && !in_sq && p[1]) {     /* an escaped char never splits */
            if (blen < 510) { buf[blen++] = c; buf[blen++] = p[1]; }
            p++;
            continue;
        }
        if      (c == '\'' && !in_dq) { in_sq = !in_sq; }
        else if (c == '"'  && !in_sq) { in_dq = !in_dq; }
        else if (!in_sq && !in_dq) {
            SepKind sep   = SEP_SEMI;
            int     skip  = 0;

            if (c == '|' && *(p+1) == '|') { sep = SEP_OR;   skip = 2; }
            else if (c == '&' && *(p+1) == '&') { sep = SEP_AND;  skip = 2; }
            else if (c == '|')               { sep = SEP_PIPE; skip = 1; }
            else if (c == ';')               { sep = SEP_SEMI; skip = 1; }
            else if (c == '\n')              { sep = SEP_SEMI; skip = 1; }
            else if (c == '\0')              { skip = 0; /* flush */ }

            if (skip > 0 || c == '\0') {
                while (blen > 0 && buf[blen-1] == ' ') blen--;
                buf[blen] = '\0';
                const char *s = buf;
                while (*s == ' ') s++;
                if (*s && n < max) {
                    seps[n] = next_sep;
                    snprintf(stages[n++], 512, "%s", s);
                }
                blen     = 0;
                next_sep = sep;
                if (c == '\0') break;
                p += skip - 1; /* -1 because loop does p++ */
                continue;
            }
        }
        if (blen < 511) buf[blen++] = c;
        if (c == '\0') break;
    }
    return n;
}

/* keep old name as a thin wrapper for the pipeline path */
/* not used right now but useful for tests */
__attribute__((unused))
static int pipe_split(const char *input, char stages[][512], int max)
{
    SepKind seps[MAX_STAGES];
    return cmd_split(input, stages, seps, max);
}

/*
 * read the last `nlines` entries from bash/zsh history into buf.
 * best-effort: if we can't find/read the history file we just return empty.
 * history context helps the LLM make smarter gen suggestions - if you've
 * been doing docker stuff all session, "run postgres" should give you
 * "docker run postgres", not something from another universe.
 */
static void read_history(char *buf, size_t buflen, int nlines)
{
    buf[0] = '\0';

    const char *hist = getenv("HISTFILE");
    char path[512];

    if (!hist) {
        const char *home = getenv("HOME");
        if (!home) return;
        /* try bash first, then zsh */
        snprintf(path, sizeof path, "%s/.bash_history", home);
        if (access(path, R_OK) != 0)
            snprintf(path, sizeof path, "%s/.zsh_history", home);
        hist = path;
    }

    FILE *f = fopen(hist, "r");
    if (!f) return;

    /* ring buffer of the last nlines */
    char lines[32][512];
    int  count = 0;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;
        /* skip zsh extended history format (": 1234567890:0;cmd") */
        if (line[0] == ':' && line[1] == ' ') {
            char *semi = strchr(line, ';');
            if (semi) memmove(line, semi + 1, strlen(semi));
            else continue;
        }
        snprintf(lines[count % 32], 512, "%s", line);
        count++;
    }
    fclose(f);

    int want = nlines < count ? nlines : count;
    int start = count - want;
    for (int i = start; i < count; i++) {
        strncat(buf, lines[i % 32], buflen - strlen(buf) - 1);
        strncat(buf, "\n",          buflen - strlen(buf) - 1);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [OPTIONS] <command>          explain a shell command\n"
            "  %s gen [OPTIONS] <description>  generate a command from plain English\n"
            "  %s config [key=value ...]        show or edit config\n"
            "\n"
            "Options:\n"
            "  --llm[=PROVIDER]   use an LLM (required for gen)\n"
            "                       ollama         localhost:11434, no key needed\n"
            "                       ollama:PORT    custom port\n"
            "                       http://host/v1 any OpenAI-compatible endpoint\n"
            "                       (nothing)      reads SHEER_API_KEY + SHEER_API_URL\n"
            "  --model=NAME       model name (overrides SHEER_LLM_MODEL env var)\n"
            "  --context          include recent shell history as LLM context\n"
            "  --plain            one line per step (&&, ||, ; or a newline);\n"
            "                     a pipe is one sentence (| head -20 reads\n"
            "                     \", showing the first 20 lines\"); cd, sleep\n"
            "                     and echo '---' say nothing in a longer\n"
            "                     command; an unrecognised step is an empty line\n"
            "  --no-color         disable ANSI colours\n"
            "  --version          print version\n"
            "  --help             this message\n"
            "\n"
            "Config file (~/.config/sheer/config or ~/.sheerrc):\n"
            "  llm=ollama          set default LLM so you never need --llm\n"
            "  model=llama3        default model\n"
            "  no-color=true\n"
            "\n"
            "Examples:\n"
            "  %s \"rm -rf /tmp/*\"\n"
            "  %s \"git push --force origin main\"\n"
            "  %s gen \"delete all log files older than 7 days\"\n"
            "  %s gen --context \"something with the last service I deployed\"\n"
            "\n",
            prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    Config cfg;
    config_load(&cfg);

    bool        gen_mode     = false;
    bool        use_llm      = cfg.llm[0] != '\0'; /* config sets a default */
    bool        use_context  = false;
    const char *llm_provider = cfg.llm[0]   ? cfg.llm   : NULL;
    const char *llm_model    = cfg.model[0] ? cfg.model : NULL;
    bool        no_color     = cfg.no_color || !isatty(STDOUT_FILENO);
    bool        plain        = false;
    int         cmd_start    = 1;

    /* sheer run ["cmd"] - explain then prompt before executing.
     * no args = read command from stdin (so `alias shrun='eval "$(sheer run)"'` works) */
    if (strcmp(argv[1], "run") == 0) {
        char input[8192] = "";
        if (argc >= 3) {
            for (int i = 2; i < argc; i++) {
                if (i > 2) strncat(input, " ", sizeof input - strlen(input) - 1);
                strncat(input, argv[i], sizeof input - strlen(input) - 1);
            }
        } else {
            /* interactive: prompt on stderr, read from stdin */
            fprintf(stderr, "  command: ");
            fflush(stderr);
            if (!fgets(input, sizeof input, stdin)) return 0;
            input[strcspn(input, "\n")] = '\0';
            if (!input[0]) return 0;
        }

        /* analysis goes to stderr so eval "$(sheer run)" only captures
         * the command string (stdout), not the explanation text */
        int saved = dup(STDOUT_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);

        char    stages[MAX_STAGES][512];
        SepKind seps[MAX_STAGES];
        int     nstages = cmd_split(input, stages, seps, MAX_STAGES);

        if (nstages > 1) {
            Analysis analyses[MAX_STAGES];
            Risk     overall = RISK_SAFE;
            for (int i = 0; i < nstages; i++) {
                analyses[i].matched = false;
                TokList stl = { .n = 0 };
                tok_parse(stages[i], &stl);
                cmd_analyze(&stl, &analyses[i]);
                overall = risk_max(overall, analyses[i].risk);
            }
            const char *sptrs[MAX_STAGES];
            for (int i = 0; i < nstages; i++) sptrs[i] = stages[i];
            out_render_pipeline(sptrs, analyses, seps, nstages, overall, true);
        } else {
            TokList  tl  = { .n = 0 };
            Analysis res = { .matched = false };
            tok_parse(nstages == 1 ? stages[0] : input, &tl);
            cmd_analyze(&tl, &res);
            out_render(&res, input, true);
        }

        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);

        fprintf(stderr, "\n  run it? [y/N] ");
        char ans[8] = "";
        if (!fgets(ans, sizeof ans, stdin)) return 0;

        /* print command to stdout only on yes - eval captures this */
        if (ans[0] == 'y' || ans[0] == 'Y')
            printf("%s", input);

        return 0;
    }

    if (strcmp(argv[1], "config") == 0) {
        if (argc == 2) return config_show();
        /* "sheer config key=value [key=value ...]" */
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= config_set(argv[i]);
        return rc;
    }

    if (strcmp(argv[1], "gen") == 0) {
        gen_mode  = true;
        use_llm   = true;
        cmd_start = 2;
    }

    for (int i = cmd_start; i < argc; i++) {
        const char *a = argv[i];

        if (strncmp(a, "--llm=", 6) == 0) {
            use_llm      = true;
            llm_provider = a + 6;
            cmd_start    = i + 1;
        } else if (strcmp(a, "--llm") == 0) {
            use_llm   = true;
            cmd_start = i + 1;
        } else if (strncmp(a, "--model=", 8) == 0) {
            llm_model = a + 8;
            cmd_start = i + 1;
        } else if (strcmp(a, "--context") == 0) {
            use_context = true;
            cmd_start   = i + 1;
        } else if (strcmp(a, "--plain") == 0) {
            plain     = true;
            cmd_start = i + 1;
        } else if (strcmp(a, "--no-color") == 0) {
            no_color  = true;
            cmd_start = i + 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("sheer " VERSION "\n");
            return 0;
        } else {
            cmd_start = i;
            break;
        }
    }

    if (cmd_start >= argc) {
        fprintf(stderr, "sheer: no %s given\n",
                gen_mode ? "description" : "command");
        return 1;
    }

    /* join remaining args into one string */
    char input[8192] = "";
    for (int i = cmd_start; i < argc; i++) {
        if (i > cmd_start)
            strncat(input, " ", sizeof input - strlen(input) - 1);
        strncat(input, argv[i], sizeof input - strlen(input) - 1);
    }

    /* ---- gen mode ---- */
    if (gen_mode) {
        LlmConf conf;
        if (llm_conf_parse(llm_provider, llm_model, &conf) < 0) {
            fprintf(stderr, "sheer: unknown LLM provider '%s'\n"
                            "       use: ollama, ollama:<port>, or http://...\n",
                    llm_provider);
            return 1;
        }
        if (conf.url[0] == '\0') {
            fprintf(stderr, "sheer gen requires --llm. "
                            "try: sheer gen --llm=ollama \"...\"\n");
            return 1;
        }

        char history[2048] = "";
        if (use_context)
            read_history(history, sizeof history, 10);

        char generated[1024] = "";
        llm_gen(input, use_context ? history : NULL, &conf, generated, sizeof generated);

        /* run the generated command through our risk engine */
        TokList  tl  = { .n = 0 };
        Analysis res = { .matched = false };
        if (generated[0] != '\0') {
            tok_parse(generated, &tl);
            cmd_analyze(&tl, &res);
            if (!res.matched)
                llm_analyze(generated, &conf, &res);
        }

        out_render_gen(input, generated, &res, !no_color);
        return 0;
    }

    /* ---- explain mode ---- */

    /* split on |, &&, ||, ; */
    char     stages[MAX_STAGES][512];
    SepKind  seps[MAX_STAGES];
    int      nstages = cmd_split(input, stages, seps, MAX_STAGES);

    if (nstages > 1 || plain) {
        Analysis analyses[MAX_STAGES];
        bool     quiet[MAX_STAGES], joins[MAX_STAGES];
        char     clauses[MAX_STAGES][256];
        Risk     overall = RISK_SAFE;
        LlmConf  conf;
        bool     conf_ready = false;

        for (int i = 0; i < nstages; i++) {
            analyses[i].matched = false;
            TokList stl = { .n = 0 };
            tok_parse(stages[i], &stl);
            cmd_analyze(&stl, &analyses[i]);
            quiet[i] = plain && nstages > 1 && cmd_quiet(&stl);   /* alone, it speaks */
            joins[i] = plain && cmd_clause(&stl, clauses[i], sizeof clauses[i]);

            if (!analyses[i].matched && use_llm) {
                if (!conf_ready) {
                    if (llm_conf_parse(llm_provider, llm_model, &conf) < 0) {
                        fprintf(stderr, "sheer: unknown LLM provider '%s'\n",
                                llm_provider);
                        return 1;
                    }
                    conf_ready = true;
                }
                llm_analyze(stages[i], &conf, &analyses[i]);
            }
            overall = risk_max(overall, analyses[i].risk);
        }

        if (plain) {
            out_render_plain(analyses, seps, quiet, joins, clauses, nstages);
            return 0;
        }

        const char *sptrs[MAX_STAGES];
        for (int i = 0; i < nstages; i++) sptrs[i] = stages[i];
        out_render_pipeline(sptrs, analyses, seps, nstages, overall, !no_color);
        return 0;
    }

    /* single command */
    TokList  tl  = { .n = 0 };
    Analysis res = { .matched = false };

    if (tok_parse(nstages == 1 ? stages[0] : input, &tl) < 0) {
        fprintf(stderr, "sheer: failed to parse command (too many tokens)\n");
        return 1;
    }

    cmd_analyze(&tl, &res);

    if (!res.matched && use_llm) {
        LlmConf conf;
        if (llm_conf_parse(llm_provider, llm_model, &conf) < 0) {
            fprintf(stderr, "sheer: unknown LLM provider '%s'\n"
                            "       use: ollama, ollama:<port>, or http://...\n",
                    llm_provider);
            return 1;
        }
        llm_analyze(input, &conf, &res);
    }

    out_render(&res, input, !no_color);
    return 0;
}
