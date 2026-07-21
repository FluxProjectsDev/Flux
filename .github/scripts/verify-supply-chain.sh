#!/usr/bin/env bash
#
# Copyright (C) 2026 FebriCahyaa
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Supply-chain audit of the GitHub Actions workflows.
#
# Reads every .github/workflows/*.yml and asserts the properties that decide what a compromised or
# careless workflow could do: least-privilege token scopes, no publishing credential reachable on
# a pull_request, immutable third-party action refs, and explicitly named artifacts. YAML is parsed
# with a real parser (python3) rather than grep, because a permissions block or an `if:` guard that
# a text match would miss is exactly the kind of thing this must not miss.
#
# FAILS on:
#   - a workflow with no permissions declared anywhere (top-level or per-job)
#   - top-level `permissions: write-all` or a top-level write scope (write belongs on the one job
#     that needs it, not on the whole workflow)
#   - a third-party action pinned to a mutable ref (@master/@main/@latest/@HEAD)
#   - a step that binds a secret but is not guarded against running on pull_request
#   - an upload-artifact step with no explicit name
#
# REPORTS (does not fail) the pin type of every third-party action, so tag-pinned actions — the
# repository's current convention, with SHA pinning tracked as a follow-up — stay visible.
#
# Usage: verify-supply-chain.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

python3 - <<'PY'
import glob, json, re, sys

FAIL = 0
def fail(msg):
    global FAIL
    print(f"\033[31mFAIL:\033[0m {msg}")
    FAIL += 1
def ok(msg):    print(f"\033[32m  {msg}\033[0m")
def info(msg):  print(f"\033[36m•\033[0m {msg}")
def head(msg):  print(f"\n\033[1m{msg}\033[0m")

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML is required (pip install pyyaml).", file=sys.stderr)
    sys.exit(1)

FIRST_PARTY = ("actions/", "github/")            # GitHub-published; trusted, tag refs accepted
MUTABLE = ("master", "main", "latest", "HEAD")
WRITE_SCOPES_OK_TOPLEVEL = ("id-token", "pages", "attestations")  # not repo-content write

# A step "binds a secret" if it references the GitHub `${{ secrets.NAME }}` expression context —
# not merely the substring "secrets." (which appears in, e.g., the filename scan-secrets.sh). A
# step is "guarded" against pull_request if its own if — or its job's if — excludes that event.
SECRET_RE = re.compile(r"\$\{\{[^}]*\bsecrets\.")
GUARD_RE = re.compile(
    r"event_name\s*!=\s*'pull_request'"
    r"|event_name\s*==\s*'(workflow_dispatch|push|schedule)'"
)

def norm_on(on):
    if isinstance(on, str):  return {on: None}
    if isinstance(on, list): return {k: None for k in on}
    if isinstance(on, dict): return on
    return {}

workflows = sorted(glob.glob(".github/workflows/*.yml"))
if not workflows:
    fail("no workflows found under .github/workflows/")

all_actions = []  # (workflow, ref)

for wf in workflows:
    head(wf)
    with open(wf) as fh:
        doc = yaml.safe_load(fh)
    if not isinstance(doc, dict):
        fail(f"{wf}: not a mapping")
        continue

    # `on:` parses to the key True in YAML 1.1 (yes/on) — accept either.
    on = doc.get("on", doc.get(True, {}))
    triggers = norm_on(on)
    is_pr = "pull_request" in triggers or "pull_request_target" in triggers

    top_perms = doc.get("permissions")
    jobs = doc.get("jobs", {}) or {}

    # 1. Permissions declared somewhere.
    job_perms = {jid: j.get("permissions") for jid, j in jobs.items()}
    if top_perms is None and all(p is None for p in job_perms.values()):
        fail(f"{wf}: no permissions declared (top-level or per-job) — the token defaults wide")
    else:
        ok("permissions are declared")

    # 2. Top-level must not carry a repo-content write scope or be write-all.
    if top_perms == "write-all":
        fail(f"{wf}: top-level 'permissions: write-all'")
    elif isinstance(top_perms, dict):
        for scope, level in top_perms.items():
            if level == "write" and scope not in WRITE_SCOPES_OK_TOPLEVEL:
                fail(f"{wf}: top-level write scope '{scope}: write' — scope it to the job that needs it")
    if isinstance(top_perms, (dict, str)) and top_perms not in ("write-all",):
        ok("top-level permissions carry no repo-content write scope")

    # Per-job permissions on a PR-triggered workflow that grant contents:write are reported (the
    # publish step must be event-guarded, checked in §4) rather than failed: a build job that also
    # publishes on dispatch legitimately needs the scope, and GitHub has no per-event job scope.
    for jid, jp in job_perms.items():
        if is_pr and isinstance(jp, dict) and jp.get("contents") == "write":
            info(f"job '{jid}' has contents:write on a PR-triggered workflow — publish steps must be event-guarded (checked below)")

    # 3. Action refs: collect and classify.
    for jid, job in jobs.items():
        for step in (job.get("steps") or []):
            uses = step.get("uses")
            if not uses or "@" not in uses:
                continue
            name, ref = uses.rsplit("@", 1)
            all_actions.append((wf, uses))
            first_party = any(name.startswith(p) for p in FIRST_PARTY)
            if ref in MUTABLE and not first_party:
                fail(f"{wf}: third-party action '{uses}' is pinned to a mutable ref — pin to a commit SHA")

    # 4. Secret-binding steps must be guarded; upload-artifact steps must be named.
    for jid, job in jobs.items():
        job_if = str(job.get("if", ""))
        for step in (job.get("steps") or []):
            blob = json.dumps(step)
            # A secret only needs a pull_request guard on a workflow that pull_request can trigger.
            # A release/dispatch/push-only workflow binding a secret is not exposed to fork PRs.
            if SECRET_RE.search(blob) and is_pr:
                guard = str(step.get("if", "")) + " " + job_if
                if not GUARD_RE.search(guard):
                    fail(f"{wf} job '{jid}': a step binds a secret without a pull_request guard")
                else:
                    ok(f"job '{jid}': secret-binding step is guarded against pull_request")
            uses = step.get("uses", "")
            if "upload-artifact" in uses:
                nm = (step.get("with") or {}).get("name")
                if not nm:
                    fail(f"{wf} job '{jid}': upload-artifact step has no explicit name")
                else:
                    ok(f"job '{jid}': artifact '{nm}' is explicitly named")

# ── Pin-type report ───────────────────────────────────────────────────────────
head("Third-party action pin report")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
seen = set()
for wf, uses in all_actions:
    name, ref = uses.rsplit("@", 1)
    if any(name.startswith(p) for p in FIRST_PARTY):
        continue
    if uses in seen:
        continue
    seen.add(uses)
    kind = "sha" if SHA_RE.match(ref) else ("MUTABLE" if ref in MUTABLE else "tag")
    info(f"{kind:8} {uses}")

print()
if FAIL:
    print(f"\033[31m{FAIL} supply-chain violation(s).\033[0m")
    print("\033[31mSupply-chain audit: NOT PROVEN\033[0m")
    sys.exit(1)
print("\033[32mSupply-chain audit: PROVEN\033[0m")
print("\033[32m  - every workflow declares permissions; no top-level repo-content write\033[0m")
print("\033[32m  - no third-party action on a mutable ref\033[0m")
print("\033[32m  - every secret-binding step is guarded against pull_request\033[0m")
print("\033[32m  - every uploaded artifact is explicitly named\033[0m")
PY
