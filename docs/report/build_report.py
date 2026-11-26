#!/usr/bin/env python3
"""
build_report.py - generates docs/REPORT.pdf from docs/report/template.html.

Fills the template with live data, then prints it to PDF with a headless
Chromium-based browser (Edge, Chrome or Chromium). Python standard library only.

Data sources (all optional; a highlighted placeholder is shown if missing):
  docs/report/members.json   names, student numbers, modules, git author names
  git log                    commit history and per-member commit counts
  docs/test_results.txt      output of:  bash tests/test_scenarios.sh | tee docs/test_results.txt
  docs/sample_watchdog.log   produced by: SAVE_SAMPLE=1 make test

Usage (from anywhere):
  python docs/report/build_report.py                 # -> docs/REPORT.pdf
  python docs/report/build_report.py --html-only     # -> only the rendered HTML (for a quick look)
  options: --out PATH  --tests PATH  --sample-log PATH  --browser PATH  --git-scope all|head
"""
import argparse
import datetime as dt
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

SCENARIOS = {
    "1": "Normal operation",
    "2": "Normal termination",
    "3": "Unexpected termination",
    "4": "Restart limit",
    "5": "Status reporting (SIGUSR1)",
    "6": "Graceful shutdown (SIGTERM)",
    "7": "Graceful shutdown (SIGINT / Ctrl+C)",
    "8": "SIGKILL after timeout (stubborn workers)",
}

SAMPLE_LOG_MAX_LINES = 70


def esc(s):
    return html.escape(str(s), quote=True)


def todo(text):
    return f'<span class="todo">{esc(text)}</span>'


def val(s):
    """Shows values that start with TODO as highlighted placeholders."""
    s = str(s)
    return todo(s) if s.strip().upper().startswith("TODO") else esc(s)


def git(*args):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout


# ------------------------------------------------------------------ sections

def members_cover(cfg):
    rows = "".join(
        f"<tr><td>Student {m['student']}</td><td>{val(m['name'])}</td>"
        f"<td>{val(m['number'])}</td><td>{val(m['github'])}</td></tr>"
        for m in cfg["members"]
    )
    return ("<table><tr><th>Role</th><th>Name</th><th>Student No.</th><th>GitHub</th></tr>"
            f"{rows}</table>")


def git_history(scope):
    fmt = ["--pretty=format:%h %an %ad %s", "--date=short"]
    if scope == "all":
        refs = ["--branches", "--remotes"]
        label = "all branches"
    else:
        refs = ["HEAD"]
        label = "branch " + git("rev-parse", "--abbrev-ref", "HEAD").strip()
    log = git("log", *refs, *fmt).strip()
    # Per-member counts ignore merge commits (they only join branches).
    authors = [a for a in git("log", *refs, "--no-merges", "--format=%an").splitlines() if a]
    return log, authors, label


def task_table(cfg, authors):
    rows = []
    for m in cfg["members"]:
        names = set(m.get("git_authors") or [])
        count = sum(1 for a in authors if a in names) if names else None
        files = "<br>".join(f"<code>{esc(f)}</code>" for f in m["files"])
        tasks = "".join(f"<li>{esc(t)}</li>" for t in m["tasks"])
        commits = str(count) if count is not None else todo("TODO")
        rows.append(
            f"<tr><td>Student {m['student']}<br>{val(m['name'])}<br>"
            f"<span class='note'>{val(m['github'])}</span></td>"
            f"<td><strong>{esc(m['module'])}</strong><ul>{tasks}</ul></td>"
            f"<td>{files}</td><td style='text-align:center'>{commits}</td></tr>"
        )
    return ("<table><tr><th style='width:22%'>Member</th><th>Module and responsibilities</th>"
            "<th style='width:30%'>Files</th><th style='width:9%'>Commits</th></tr>"
            + "".join(rows) + "</table>")


def commit_summary(cfg, authors):
    known = {}
    for m in cfg["members"]:
        for a in m.get("git_authors") or []:
            known[a] = f"Student {m['student']} ({m['github']})"
    counts = {}
    for a in authors:
        counts[a] = counts.get(a, 0) + 1
    rows = "".join(
        f"<tr><td><code>{esc(a)}</code></td><td>{esc(known.get(a, 'unmapped - add to members.json'))}</td>"
        f"<td style='text-align:center'>{n}</td></tr>"
        for a, n in sorted(counts.items(), key=lambda kv: -kv[1])
    )
    return ("<table><tr><th>Git author</th><th>Group member</th><th style='width:12%'>Commits</th></tr>"
            f"{rows}<tr><th colspan='2'>Total</th><th style='text-align:center'>{len(authors)}</th></tr></table>")


def pr_table():
    """Pull requests from GitHub via the gh CLI (optional)."""
    if not shutil.which("gh"):
        return f"<p>{todo('TODO: install/log in to the gh CLI to list the pull requests')}</p>"
    r = subprocess.run(["gh", "pr", "list", "--state", "all", "--limit", "100", "--json",
                        "number,title,author,state,mergedBy,reviews"],
                       cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        return f"<p>{todo('TODO: gh pr list failed: ' + r.stderr.strip()[:120])}</p>"
    rows = []
    for pr in sorted(json.loads(r.stdout), key=lambda x: x["number"]):
        reviews = ", ".join(sorted({f"{v['author']['login']} ({v['state'].lower().replace('_', ' ')})"
                                    for v in pr.get("reviews") or []})) or "&ndash;"
        merged = (pr.get("mergedBy") or {}).get("login") or "&ndash;"
        rows.append(f"<tr><td>#{pr['number']}</td><td>{esc(pr['title'])}</td>"
                    f"<td>{esc(pr['author']['login'])}</td><td>{esc(reviews) if reviews != '&ndash;' else reviews}</td>"
                    f"<td>{esc(merged) if merged != '&ndash;' else merged}</td><td>{esc(pr['state'].lower())}</td></tr>")
    return ("<table><tr><th style='width:6%'>PR</th><th>Title</th><th style='width:15%'>Author</th>"
            "<th style='width:20%'>Review</th><th style='width:14%'>Merged by</th><th style='width:9%'>State</th></tr>"
            + "".join(rows) + "</table>")


def tests_sections(path):
    if not path.exists():
        summary = "".join(
            f"<tr><td>{k}</td><td>{esc(v)}</td><td>&ndash;</td><td class='pending'>pending</td></tr>"
            for k, v in SCENARIOS.items()
        )
        table = ("<table><tr><th style='width:6%'>#</th><th>Scenario</th><th style='width:14%'>Checks</th>"
                 f"<th style='width:14%'>Result</th></tr>{summary}</table>")
        note = (f"<p>{todo('TODO: run the final tests on the integrated main branch: '
                           'bash tests/test_scenarios.sh | tee docs/test_results.txt')}</p>")
        return table, note, ""

    text = path.read_text(encoding="utf-8", errors="replace")
    groups = {k: [0, 0] for k in SCENARIOS}  # passed, total
    build = None
    for line in text.splitlines():
        m = re.match(r"^(PASS|FAIL) (\d+) ", line)
        if m and m.group(2) in groups:
            groups[m.group(2)][1] += 1
            if m.group(1) == "PASS":
                groups[m.group(2)][0] += 1
        elif re.match(r"^(PASS|FAIL) build", line):
            build = line.startswith("PASS")

    rows = []
    for k, name in SCENARIOS.items():
        p, t = groups[k]
        if t == 0:
            res = "<span class='pending'>not run</span>"
        elif p == t:
            res = "<span class='pass'>PASS</span>"
        else:
            res = "<span class='fail'>FAIL</span>"
        rows.append(f"<tr><td>{k}</td><td>{esc(name)}</td><td style='text-align:center'>{p}/{t}</td><td>{res}</td></tr>")
    if build is not None:
        res = "<span class='pass'>PASS</span>" if build else "<span class='fail'>FAIL</span>"
        rows.insert(0, f"<tr><td>&ndash;</td><td>Build with <code>make</code></td><td style='text-align:center'>1/1</td><td>{res}</td></tr>")

    total_line = next((l for l in text.splitlines() if l.startswith("passed=")), "")
    table = ("<table><tr><th style='width:6%'>#</th><th>Scenario</th><th style='width:14%'>Checks</th>"
             "<th style='width:14%'>Result</th></tr>" + "".join(rows) + "</table>")
    if total_line:
        table += f"<p><strong>Total:</strong> <code>{esc(total_line)}</code></p>"

    lines = [l for l in text.splitlines() if "heartbeat" not in l]
    raw = ("<h3>8.2 Full test output</h3><pre>" + esc("\n".join(lines).strip()) + "</pre>")
    env = f"The results below were produced on {esc(dt.date.fromtimestamp(path.stat().st_mtime).isoformat())}."
    return table, env, raw


def sample_log(path):
    if not path.exists():
        return (f"<p>{todo('TODO: generate with  SAVE_SAMPLE=1 make test  (creates docs/sample_watchdog.log)')}</p>")
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    shown = lines[:SAMPLE_LOG_MAX_LINES]
    more = len(lines) - len(shown)
    tail = f"\n... ({more} more lines in docs/sample_watchdog.log)" if more > 0 else ""
    return ("<p>Log of the first test run (normal life cycle, crash and restart, restart limit, "
            "SIGUSR1 report, SIGTERM shutdown), from <code>docs/sample_watchdog.log</code>:</p>"
            "<pre>" + esc("\n".join(shown)) + esc(tail) + "</pre>")


# ------------------------------------------------------------------ browser

def find_browser(explicit):
    cands = [explicit, os.environ.get("REPORT_BROWSER")]
    pf = [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA")]
    for base in filter(None, pf):
        cands += [os.path.join(base, "Microsoft", "Edge", "Application", "msedge.exe"),
                  os.path.join(base, "Google", "Chrome", "Application", "chrome.exe")]
    for name in ("microsoft-edge", "google-chrome", "google-chrome-stable", "chromium", "chromium-browser"):
        cands.append(shutil.which(name))
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def print_pdf(browser, html_path, out):
    profile = tempfile.mkdtemp(prefix="report-browser-")
    try:
        cmd = [browser, "--headless=new", "--disable-gpu", "--no-first-run",
               "--no-pdf-header-footer", "--print-to-pdf-no-header",
               f"--user-data-dir={profile}", f"--print-to-pdf={out}", html_path.as_uri()]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if not out.exists() or out.stat().st_size == 0:
            raise RuntimeError(f"browser did not write the PDF:\n{r.stdout}\n{r.stderr}")
    finally:
        shutil.rmtree(profile, ignore_errors=True)


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "REPORT.pdf")
    ap.add_argument("--tests", type=Path, default=ROOT / "docs" / "test_results.txt")
    ap.add_argument("--sample-log", type=Path, default=ROOT / "docs" / "sample_watchdog.log")
    ap.add_argument("--browser")
    ap.add_argument("--git-scope", choices=["all", "head"], default="all",
                    help="'all' = every local and remote branch (default), 'head' = current branch only")
    ap.add_argument("--html-only", action="store_true")
    a = ap.parse_args()

    cfg = json.loads((HERE / "members.json").read_text(encoding="utf-8"))
    log, authors, scope_label = git_history(a.git_scope)
    test_table, test_env, test_raw = tests_sections(a.tests)

    fill = {
        "COURSE": esc(cfg["course"]),
        "REPO_URL": esc(cfg["repo_url"]),
        "DATE": dt.date.today().isoformat(),
        "MEMBERS_COVER": members_cover(cfg),
        "TEST_ENV": test_env,
        "TEST_SUMMARY": test_table,
        "TEST_RAW": test_raw,
        "SAMPLE_LOG": sample_log(a.sample_log),
        "TASK_TABLE": task_table(cfg, authors),
        "PR_TABLE": pr_table(),
        "CTRL_C_MANUAL": ("Ctrl+C was also pressed by hand in an interactive terminal (WSL Ubuntu). The watchdog logged "
                          "<code>SHUTDOWN requested by SIGINT</code>, stopped the remaining worker with "
                          "<code>SIGTERM</code> (exit code 0), logged <code>SHUTDOWN complete</code> and "
                          "<code>WATCHDOG EXIT</code>, and no process was left running."
                          if cfg.get("ctrl_c_tested_by_hand")
                          else todo("TODO: press Ctrl+C by hand once in a terminal running ./watchdog, check "
                                    "watchdog.log, then set \"ctrl_c_tested_by_hand\": true in members.json")),
        "COMMIT_SUMMARY": commit_summary(cfg, authors),
        "GIT_SCOPE": esc(scope_label),
        "GIT_LOG": esc(log),
    }
    page = (HERE / "template.html").read_text(encoding="utf-8")
    page = re.sub(r"\{\{(\w+)\}\}", lambda m: fill.get(m.group(1), m.group(0)), page)
    leftovers = sorted(set(re.findall(r"\{\{(\w+)\}\}", page)))
    if leftovers:
        print("warning: unfilled placeholders:", ", ".join(leftovers), file=sys.stderr)

    tmp = Path(tempfile.mkdtemp(prefix="report-"))
    html_path = tmp / "REPORT.html"
    html_path.write_text(page, encoding="utf-8")

    if a.html_only:
        print(html_path)
        return 0

    browser = find_browser(a.browser)
    if not browser:
        print("error: no Edge/Chrome/Chromium found; pass --browser PATH", file=sys.stderr)
        return 1
    a.out.parent.mkdir(parents=True, exist_ok=True)
    if a.out.exists():
        a.out.unlink()
    print_pdf(browser, html_path, a.out.resolve())
    shutil.rmtree(tmp, ignore_errors=True)

    n_todo = page.count('class="todo"')
    print(f"wrote {a.out}")
    if n_todo:
        print(f"note: {n_todo} TODO placeholder(s) are still highlighted in the report")
    return 0


if __name__ == "__main__":
    sys.exit(main())
