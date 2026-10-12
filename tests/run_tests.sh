#!/usr/bin/env bash
# if this breaks, something is wrong. don't ship it.
set -euo pipefail

BIN="${BIN:-./sheer}"
PASS=0
FAIL=0
ERRS=()

ok()   { PASS=$((PASS+1)); }
fail() { FAIL=$((FAIL+1)); ERRS+=("$*"); }

assert_contains() {
    local desc="$1" cmd="$2" want="$3"
    local got
    got=$("$BIN" "$cmd" 2>&1)
    if echo "$got" | grep -qF "$want"; then
        ok
    else
        fail "FAIL [$desc]: expected '$want' not found"
        fail "     output: $(echo "$got" | head -5)"
    fi
}

assert_risk() { assert_contains "$1" "$2" "$3"; }

# the whole output of `sheer --plain`, exactly
assert_plain() {
    local desc="$1" cmd="$2" want="$3"
    local got
    got=$("$BIN" --plain "$cmd" 2>&1)
    if [ "$got" = "$want" ]; then
        ok
    else
        fail "FAIL [$desc]: expected '$want'"
        fail "     output: '$got'"
    fi
}

# risk levels
assert_risk "rm -rf / is CRITICAL"               "rm -rf /"                       "CRITICAL"
assert_risk "rm -rf /tmp/* is HIGH"              "rm -rf /tmp/*"                  "HIGH"
assert_risk "rm single file is LOW"              "rm notes.txt"                   "LOW"
assert_risk "git push --force is CRITICAL"       "git push --force origin main"   "CRITICAL"
assert_risk "git push normal is SAFE"            "git push origin main"           "SAFE"
assert_risk "git push --force-with-lease MODERATE" "git push --force-with-lease"  "MODERATE"
assert_risk "git reset --hard is HIGH"           "git reset --hard HEAD~3"        "HIGH"
assert_risk "git reset --soft is LOW"            "git reset --soft HEAD~1"        "LOW"
assert_risk "git clean -f is HIGH"               "git clean -f"                   "HIGH"
assert_risk "git clean -n is SAFE"               "git clean -n"                   "SAFE"
assert_risk "chmod 777 is HIGH"                  "chmod -R 777 /var/www"          "HIGH"
assert_risk "chmod 644 is LOW"                   "chmod 644 file.txt"             "LOW"
assert_risk "dd wipe /dev/sda is CRITICAL"       "dd if=/dev/urandom of=/dev/sda" "CRITICAL"
assert_risk "mkfs.ext4 is CRITICAL"              "mkfs.ext4 /dev/sdb1"            "CRITICAL"
assert_risk "iptables -F is CRITICAL"            "iptables -F"                    "CRITICAL"
assert_risk "find -delete is HIGH"               "find /var/log -name '*.log' -delete" "HIGH"
assert_risk "find no delete is SAFE"             "find . -name '*.py'"            "SAFE"
assert_risk "rsync --delete is HIGH"             "rsync -a --delete src/ dst/"    "HIGH"
assert_risk "rsync dry-run is SAFE"              "rsync -n src/ dst/"             "SAFE"
assert_risk "tar list is SAFE"                   "tar -tzf backup.tar.gz"         "SAFE"
assert_risk "docker run --privileged is CRITICAL" "docker run --privileged ubuntu" "CRITICAL"
assert_risk "docker rm -f is HIGH"               "docker rm -f mycontainer"       "HIGH"
assert_risk "sudo rm -rf escalates risk"         "sudo rm -rf /etc"               "CRITICAL"

# explanation content
assert_contains "rm explains deletion"           "rm -rf /tmp/*"          "Deletes"
assert_contains "dd explains overwrite"          "dd if=/dev/urandom of=/dev/sda" "Overwrites"
assert_contains "chmod describes mode"           "chmod 644 file.txt"     "owner"
assert_contains "git reset --hard mentions stash" "git reset --hard HEAD~1" "stash"
assert_contains "git push --force has safer alt" "git push --force"       "force-with-lease"
assert_contains "chmod 777 has safer alt"        "chmod 777 /var/www"     "755"
assert_contains "mkfs warns data loss"           "mkfs.ext4 /dev/sdb1"    "destroyed"
assert_contains "find -delete warns irreversible" "find /tmp -name '*.log' -delete" "irreversible"
assert_contains "rsync --delete warns"           "rsync -a --delete src/ dst/" "delete"
assert_contains "source is local rules"          "rm -rf /tmp/*"          "local rules"

# sudo wrapping
assert_contains "sudo prefixes explanation"      "sudo rm -rf /etc"       "As root:"
assert_contains "sudo rm explains deletion"      "sudo rm -rf /etc"       "Deletes"

# path stripping
assert_risk "/usr/bin/rm is recognized"          "/usr/bin/rm -rf /tmp"   "HIGH"
assert_risk "/bin/mkfs.ext4 is recognized"       "/bin/mkfs.ext4 /dev/sda" "CRITICAL"

# unknown command - should not crash and should say something useful
got=$("$BIN" "unknowncmd --foo bar" 2>&1)
if echo "$got" | grep -qi "not recogni"; then ok
else fail "FAIL [unknown command]: expected 'not recognised' message"
fi

# --version
got=$("$BIN" --version 2>&1)
if echo "$got" | grep -q "sheer"; then ok
else fail "FAIL [--version]: expected version string"
fi

# --help exits 0
"$BIN" --help > /dev/null 2>&1 && ok || fail "FAIL [--help]: non-zero exit"

# empty input shouldn't segfault
"$BIN" "" > /dev/null 2>&1; ok

# pipeline - risk should aggregate to HIGH (xargs rm)
got=$("$BIN" "find /tmp -name '*.log' | xargs rm" 2>&1)
if echo "$got" | grep -q "pipeline"; then ok
else fail "FAIL [pipeline]: expected pipeline output"
fi

# ── docker run multi-port/volume (the fix) ───────────────────────────────────

assert_contains "docker run multi-port shows both"    "docker run -p 80:80 -p 443:443 nginx"  "443"
assert_contains "docker run multi-vol shows both"     "docker run -v /data:/data -v /logs:/logs ubuntu" "logs"
assert_contains "docker run multi-port finds image"   "docker run -p 80:80 -p 443:443 nginx"  "nginx"
assert_risk    "docker run host vol is HIGH"          "docker run -v /etc:/etc ubuntu bash"    "HIGH"

# ── file operations ──────────────────────────────────────────────────────────

assert_risk    "mv to /etc is HIGH"                   "mv config.txt /etc/config"              "HIGH"
assert_contains "mv explains move"                    "mv old.txt new.txt"                     "Move"
assert_risk    "cp is SAFE"                           "cp file.txt backup.txt"                 "SAFE"
assert_risk    "cp -r to /usr is HIGH"                "cp -r src/ /usr/local"                  "HIGH"
assert_risk    "chown root is HIGH"                   "chown root:root /var/app"               "HIGH"
assert_risk    "ln -s is SAFE"                        "ln -s /etc/hosts hosts"                 "SAFE"
assert_risk    "ln hard link is SAFE"                 "ln a.txt b.txt"                         "SAFE"

# ── process management ───────────────────────────────────────────────────────

assert_risk    "kill default is MODERATE"             "kill 1234"                              "MODERATE"
assert_risk    "kill -9 is HIGH"                      "kill -9 1234"                           "HIGH"
assert_contains "kill -9 warns unblockable"           "kill -9 1234"                           "SIGKILL"
assert_risk    "pkill -9 is HIGH"                     "pkill -9 nginx"                         "HIGH"
assert_risk    "killall normal is MODERATE"           "killall node"                           "MODERATE"

# ── network ──────────────────────────────────────────────────────────────────

assert_risk    "curl GET is SAFE"                     "curl https://example.com"               "SAFE"
assert_risk    "curl DELETE is HIGH"                  "curl -X DELETE https://api.example.com/resource" "HIGH"
assert_contains "curl detects method"                 "curl -X POST https://example.com"       "POST"
assert_risk    "wget is LOW"                          "wget https://example.com/file.tar.gz"   "LOW"
assert_risk    "ssh normal is SAFE"                   "ssh user@server.com"                    "SAFE"
assert_risk    "ssh agent forward is MODERATE"        "ssh -A user@server.com"                 "MODERATE"
assert_contains "ssh -A warns agent"                  "ssh -A user@server.com"                 "agent"
assert_risk    "nc listen + exec is CRITICAL"         "nc -l -e /bin/sh -p 4444"              "CRITICAL"
assert_risk    "nc listen is MODERATE"                "nc -l -p 8080"                          "MODERATE"
assert_risk    "scp is LOW"                           "scp file.txt user@host:/tmp"            "LOW"
assert_risk    "ping is SAFE"                         "ping 8.8.8.8"                           "SAFE"

# ── system administration ────────────────────────────────────────────────────

assert_risk    "systemctl stop is MODERATE"           "systemctl stop nginx"                   "MODERATE"
assert_risk    "systemctl start is LOW"               "systemctl start nginx"                  "LOW"
assert_risk    "systemctl mask is HIGH"               "systemctl mask sshd"                    "HIGH"
assert_contains "systemctl mask warns"                "systemctl mask sshd"                    "cannot be started"
assert_risk    "mount device is HIGH"                 "mount /dev/sdb1 /mnt"                   "HIGH"
assert_risk    "mount no args is SAFE"                "mount"                                   "SAFE"
assert_risk    "fdisk is CRITICAL"                    "fdisk /dev/sda"                          "CRITICAL"
assert_risk    "parted is CRITICAL"                   "parted /dev/sda"                         "CRITICAL"
assert_risk    "crontab -e is MODERATE"               "crontab -e"                              "MODERATE"
assert_risk    "crontab -r is HIGH"                   "crontab -r"                              "HIGH"
assert_contains "crontab -r warns"                    "crontab -r"                              "Removes ALL"
assert_risk    "useradd is MODERATE"                  "useradd newuser"                         "MODERATE"
assert_risk    "userdel -r is HIGH"                   "userdel -r olduser"                      "HIGH"

# ── script execution ─────────────────────────────────────────────────────────

assert_risk    "bash -c is HIGH"                      "bash -c 'echo hello'"                   "HIGH"
assert_risk    "bash interactive is SAFE"             "bash"                                    "SAFE"
assert_risk    "python -c is HIGH"                    "python -c 'import os; os.system(\"ls\")'" "HIGH"
assert_risk    "eval is CRITICAL"                     "eval \$SOME_VAR"                         "CRITICAL"
assert_contains "eval warns injection"                "eval foo"                                "injection"
assert_risk    "base64 encode is SAFE"                "base64 file.txt"                         "SAFE"
assert_risk    "base64 decode is MODERATE"            "base64 -d payload"                       "MODERATE"

# ── package managers ─────────────────────────────────────────────────────────

assert_risk    "pip install system is MODERATE"       "pip install requests"                    "MODERATE"
assert_risk    "pip install --user is LOW"            "pip install --user requests"             "LOW"
assert_risk    "npm install global is MODERATE"       "npm install -g typescript"               "MODERATE"
assert_risk    "npm install local is LOW"             "npm install"                             "LOW"
assert_contains "apt install explains"                "apt install nginx"                       "nstall"
assert_contains "apt remove explains"                 "apt remove nginx"                        "emov"

# ── git subcommands ──────────────────────────────────────────────────────────

assert_risk    "git checkout is LOW"                  "git checkout main"                       "LOW"
assert_risk    "git clone is SAFE"                    "git clone https://github.com/foo/bar"    "SAFE"
assert_risk    "git merge is LOW"                     "git merge feature"                       "LOW"
assert_risk    "git rebase is HIGH"                   "git rebase main"                         "HIGH"
assert_risk    "git rebase -i is HIGH"                "git rebase -i HEAD~5"                    "HIGH"
assert_risk    "git stash is SAFE"                    "git stash"                               "SAFE"
assert_risk    "git stash drop is MODERATE"           "git stash drop"                          "MODERATE"
assert_risk    "git diff is SAFE"                     "git diff"                                "SAFE"

# ── docker subcommands ───────────────────────────────────────────────────────

assert_risk    "docker exec is LOW"                   "docker exec mycontainer ls"              "LOW"
assert_risk    "docker exec root is HIGH"             "docker exec --user root mycontainer bash" "HIGH"
assert_risk    "docker stop is LOW"                   "docker stop mycontainer"                  "LOW"
assert_risk    "docker ps is SAFE"                    "docker ps"                                "SAFE"
assert_risk    "docker run --network host is HIGH"    "docker run --network host nginx"          "HIGH"

# ── safe/info commands ───────────────────────────────────────────────────────

assert_risk    "ls is SAFE"                           "ls -la"                                   "SAFE"
assert_risk    "cat is SAFE"                          "cat /etc/passwd"                           "SAFE"
assert_risk    "ps is SAFE"                           "ps aux"                                    "SAFE"
assert_risk    "df is SAFE"                           "df -h"                                     "SAFE"
assert_risk    "grep is SAFE"                         "grep -r TODO ."                            "SAFE"
assert_risk    "echo is SAFE"                         "echo hello"                                "SAFE"

# ── ip6tables alias ──────────────────────────────────────────────────────────

assert_risk    "ip6tables -F is CRITICAL"             "ip6tables -F"                              "CRITICAL"

# ── what the command acts on: files, lines, revisions ──────────────────────

assert_contains "head names count and file"   "head -40 scripts/view.py"            "Prints the first 40 lines of scripts/view.py."
assert_contains "head -n N"                   "head -n 5 a.txt b.txt"               "Prints the first 5 lines of a.txt and b.txt."
assert_contains "head reads input"            "head -n5"                            "Prints the first 5 lines of its input."
assert_contains "tail names file"             "tail -20 log.txt"                    "Prints the last 20 lines of log.txt."
assert_contains "tail -n +N"                  "tail -n +3 a.txt"                    "Prints a.txt from line 3 on."
assert_contains "tail -f follows"             "tail -f app.log"                     "Prints the last 10 lines of app.log and keeps printing new ones."
assert_contains "cat names files"             "cat a.txt b.txt c.txt d.txt"         "Prints a.txt, b.txt and 2 more files."
assert_contains "cat into a file"             "cat a.txt > b.txt"                   "Copies a.txt into b.txt."
assert_contains "cat heredoc writes file"     $'cat > notes.md <<\'EOF\'\n# Title\nbody | x && y\nEOF' "Writes the text that follows into notes.md."
assert_contains "heredoc body is not a stage" $'cat > notes.md <<\'EOF\'\nbody | x && y\nEOF' "Writes the text that follows into notes.md."
assert_contains "sed prints a range"          "sed -n 1,34p scripts/transcript.py"  "Prints lines 1-34 of scripts/transcript.py."
assert_contains "sed prints one line"         "sed -n '7p' a.txt"                   "Prints line 7 of a.txt."
assert_contains "sed prints to the end"       "sed -n '40,\$p' a.txt"               "Prints a.txt from line 40 to the end."
assert_contains "sed prints matches"          "sed -n '/def /p' a.py"               "Prints the lines of a.py that match 'def '."
assert_contains "sed replaces"                "sed 's/foo/bar/g' a.txt"             "Prints a.txt with every 'foo' replaced by 'bar'."
assert_contains "sed -i replaces in place"    "sed -i '' 's/foo/bar/' a.txt"        "with 'bar' in a.txt, editing it in"
assert_contains "sed deletes lines"           "sed '/^#/d' a.txt"                   "Prints a.txt without the lines that match '^#'."
assert_contains "grep names folder"           "grep -rn hebLoad scripts/"           "Searches every file under scripts/ for 'hebLoad'."
assert_contains "grep -e and options"         "grep -e foo -A 3 -i a.py"            "Searches a.py for 'foo', ignoring case."
assert_contains "grep input"                  "grep -v '^#'"                        "Searches its input for '^#', keeping the lines that don't match."
assert_contains "grep -l lists files"         "grep -rl TODO src"                   "Searches every file under src for 'TODO', listing the files that match."
assert_contains "grep -c counts"              "grep -c error log.txt"               "Searches log.txt for 'error', counting the matches."
assert_contains "grep include"                "grep -rn --include='*.py' foo ."     "Searches every '*.py' file under . for 'foo'."
assert_contains "grep -r no path"             "grep -r foo"                         "Searches every file under the current folder for 'foo'."
assert_contains "rg is recursive"             "rg -n hebLoad scripts"               "Searches every file under scripts for 'hebLoad'."
assert_contains "rg --files"                  "rg --files src"                      "Lists every file under src."
assert_contains "2>&1 is not an argument"     "head -n 5 a.txt 2>&1"                "Prints the first 5 lines of a.txt."
assert_contains "output redirect is named"    "grep foo a.txt > out.txt"            "Writes the output to out.txt."
assert_contains "append redirect is named"    "grep foo a.txt >> out.txt"           "Appends the output to out.txt."
assert_contains "stderr to /dev/null quiet"   "grep foo a.txt 2>/dev/null"          "Searches a.txt for 'foo'."

assert_contains "git diff since a commit"     "git diff HEAD~1 -- page.html"        "Shows the changes to page.html since HEAD~1."
assert_contains "git diff uncommitted"        "git diff"                            "Shows the uncommitted changes."
assert_contains "git diff staged"             "git diff --cached"                   "Shows the staged changes."
assert_contains "git diff two commits"        "git diff main..feature"              "Shows the changes between main and feature."
assert_contains "git diff --stat"             "git diff --stat HEAD~3"              "Lists the files changed since HEAD~3, with a count of changed lines."
assert_contains "git diff a path"             "git diff scripts/view.py"            "Shows the uncommitted changes to scripts/view.py."
assert_contains "git -C dispatches"           "git -C /tmp/x diff"                  "Shows the uncommitted changes, in /tmp/x."
assert_contains "git commit quotes message"   "git commit -m 'Window: x'"           "Records the staged changes as a new commit: \"Window: x\"."
assert_contains "git commit heredoc message"  $'git commit -m "$(cat <<\'EOF\'\nFix the box\n\nmore\nEOF\n)"' "Records the staged changes as a new commit: \"Fix the box\"."
assert_contains "git status"                  "git status --short"                  "Shows which files are changed, staged or untracked."
assert_contains "git -C status"               "git -C ~/x status"                   "Shows which files are changed, staged or untracked, in ~/x."
assert_contains "git log last n"              "git log --oneline -5"                "Shows the last 5 commits, one line each."
assert_contains "git log -n"                  "git log -n 3 -p"                     "Shows the last 3 commits, with their diffs."
assert_contains "git log a path"              "git log --oneline -- scripts/hook.py" "Shows the commits that changed scripts/hook.py, one line each."
assert_contains "git log -S"                  "git log -S'hebLoad' --oneline"       "Shows the commits that added or removed 'hebLoad', one line each."
assert_contains "git log history"             "git log"                             "Shows the commit history."
assert_contains "git add files"               "git add a.py b.py"                   "Stages a.py and b.py for the next commit."
assert_contains "git add all"                 "git add -A"                          "Stages every change in the repo for the next commit."
assert_contains "git add dot"                 "git add ."                           "Stages every change under . for the next commit."
assert_risk    "git add is SAFE"              "git add a.py"                        "SAFE"
assert_contains "git show last"               "git show"                            "Shows the last commit and its changes."
assert_contains "git show commit"             "git show de561fa --stat"             "Shows commit de561fa and the files it changed."
assert_contains "git show file at rev"        "git show HEAD~2:scripts/page.html"   "Shows scripts/page.html as it was at HEAD~2."
assert_contains "git branch list"             "git branch"                          "Lists the local branches."
assert_contains "git branch create"           "git branch feature"                  "Creates the branch feature."
assert_risk    "git branch -D is MODERATE"    "git branch -D old"                   "MODERATE"
assert_contains "git branch -D deletes"       "git branch -D old"                   "Deletes the branch old, even if it isn't merged."
assert_contains "git branch current"          "git branch --show-current"           "Prints the current branch name."
assert_contains "git rev-parse toplevel"      "git rev-parse --show-toplevel"       "Prints the repo's root folder."
assert_contains "git rev-parse HEAD"          "git rev-parse --short HEAD"          "Prints the commit id of HEAD."

assert_contains "grep alternation BRE"        "grep -n 'foo\|^bar' a.py"            "Searches a.py for 'foo' or '^bar'."
assert_contains "grep alternation ERE"        "grep -E 'a|b|c' a.py"                "Searches a.py for 'a', 'b' or 'c'."
assert_contains "rg alternation"              "rg 'x|y' src"                        "Searches every file under src for 'x' or 'y'."
assert_contains "grep BRE pipe is literal"    "grep 'a|b' a.py"                     "Searches a.py for 'a|b'."
assert_contains "rg negated glob is skipped"  "rg -n foo -g '!node_modules' src"    "Searches every file under src for 'foo'."
assert_contains "git log -1 is singular"      "git log --oneline -1"                "Shows the last commit, one line each."
assert_contains "sed several ranges"          "sed -n '55,80p;86,100p' a.md"        "Prints lines 55-80 and 86-100 of a.md."

assert_contains "escaped quote stays in one stage" 'grep -n "GET /x\"\|call(\"GET" a.ts | head' "Searches a.ts for 'GET /x\"' or 'call(\"GET'."
assert_contains "rg with BRE alternation"     'rg -n "^PAGE\|^HTML" a.py'           "Searches every file under a.py for '^PAGE' or '^HTML'."

# --plain: one sentence per stage, nothing else
assert_plain "plain single"                   "git branch"                          "Lists the local branches."
assert_plain "plain stages"                   "git branch && git show"              "Lists the local branches.
Shows the last commit and its changes."
assert_plain "plain unknown stage is empty"   "git branch | zzqqx -v | git show"    "Lists the local branches.

Shows the last commit and its changes."
assert_plain "plain does not wrap"            'grep -n "GET /x\"\|call(\"GET" a.ts' "Searches a.ts for 'GET /x\"' or 'call(\"GET'."
assert_plain "plain unknown single is empty"  "zzqqx -v"                            ""

# a long subject is cut by letters, not bytes: a Hebrew letter is two
assert_plain "hebrew subject kept whole"     'git commit -m "בבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבב"'   'Records the staged changes as a new commit: "בבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבבב".'
assert_plain "hebrew subject cut by letters" 'git commit -m "אאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאא"'   'Records the staged changes as a new commit: "אאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאאא...".'

# python: what it runs, past the flags that take a value
assert_plain "python script"                 "python3 run.py"                      "Runs the Python script 'run.py'."
assert_plain "python heredoc code"           "/usr/bin/python3 - <<'EOF'
print(1)
EOF"                                                                                   "Runs the Python code that follows."
assert_plain "python heredoc, no dash"       "python3 <<EOF
print(1)
EOF"                                                                                   "Runs the Python code that follows."
assert_plain "python code from a pipe"       "echo x | python3 -"                  "Prints 'x' to stdout.
Runs the Python code from its input."
assert_plain "python module"                 "python3 -m pytest -q tests"          "Runs the Python module 'pytest'."
assert_plain "python bundled module flag"    "python3 -um http.server 8000"        "Runs the Python module 'http.server'."
assert_plain "python -W takes a value"       "python3 -W ignore a.py"              "Runs the Python script 'a.py'."
assert_plain "python -X takes a value"       "python3 -u -X dev run.py"            "Runs the Python script 'run.py'."
assert_plain "python inline"                 "python3 -c 'print(1)'"               "Runs an inline Python expression."

# report
echo ""
printf "  results: %d passed, %d failed\n" "$PASS" "$FAIL"
for e in ${ERRS[@]+"${ERRS[@]}"}; do printf "  %s\n" "$e"; done
echo ""

[ "$FAIL" -eq 0 ]
