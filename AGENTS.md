# AGENTS.md

Operational playbooks for agents and contributors working in this repository.

---

## Secret-scanning audit (gitleaks + trufflehog)

Run this before **making the repo public**, before **tagging a release**, and
periodically on `main`. It scans both the working tree and the **full git history**
(making a repo public exposes all history, not just the current tree) for leaked
credentials, private keys, and tokens.

Two complementary tools are used:

| Tool | Strength | Binary |
| --- | --- | --- |
| **gitleaks** | Fast regex/entropy rules; scans full git history; config-driven allowlist | `/usr/bin/gitleaks` (on `PATH`) |
| **trufflehog** | Detector-based; **live-verifies** whether a secret is an active credential | `~/local/bin/trufflehog` (v3.95.3) |

If `~/local/bin` is not on your `PATH`, call trufflehog by full path as shown below.

### 1. gitleaks — history + working tree

gitleaks reads `.gitleaks.toml` (committed at the repo root). That config extends
the default ruleset and **allowlists known throwaway test-only key material** (crypto
unit-test vectors + a doc example), so a clean run reports **0 findings** — meaning
any new finding is a real alert.

```bash
# Full git HISTORY (every commit on the current branch)
gitleaks detect --source . --config .gitleaks.toml --redact --no-banner \
  --report-format json --report-path gitleaks-history.json

# Working TREE only (includes uncommitted/untracked files; --no-git ignores history)
gitleaks detect --source . --no-git --config .gitleaks.toml --redact --no-banner \
  --report-format json --report-path gitleaks-tree.json
```

**Pass criteria:** both runs log `no leaks found` (`leaks found: 0`). gitleaks exits
non-zero (1) when leaks are found, 0 when clean — usable directly in CI.

### 2. trufflehog — verification pass

trufflehog tells you whether a candidate secret is an **active, working** credential.
`--no-update` skips the self-update network call; `--only-verified` keeps the signal
to confirmed-live secrets.

```bash
# Git history, only live/verified secrets (the must-be-zero signal)
~/local/bin/trufflehog git "file://$(pwd)" --no-update --only-verified --fail

# Working tree incl. untracked files (catches secrets staged before they're committed)
~/local/bin/trufflehog filesystem . --no-update --only-verified --fail

# Deeper pass — also surface UNVERIFIED candidates for manual triage (JSON)
~/local/bin/trufflehog git "file://$(pwd)" --no-update --json > trufflehog-git.json
```

**Pass criteria:** `verified_secrets: 0` in the run summary. With `--fail`, a verified
secret makes trufflehog exit non-zero. Unverified candidates are expected (see baseline)
and are triaged by hand, not auto-failed.

### 3. Interpreting results — known baseline

As of this writing a clean scan looks like:

- **gitleaks:** `0` findings (with `.gitleaks.toml`). Without the allowlist it reports
  the `private-key` rule on these **test-only** files — all safe, all allowlisted:
  `ncrypto/tests/test_rsa.c`, `ncrypto/tests/test_cms.c`,
  `tests/unit/security/test_rsa_pk.cc`, `tests/unit/security/test_asn1_cms.cc`,
  and the doc example in `examples/pdfsign/README.md`.
- **trufflehog:** `verified_secrets: 0`. It flags the same files as **unverified**
  `PrivateKey` candidates — a 1024-bit RSA test key and test certificates, not real
  credentials.

### 4. Triage rule

- **New gitleaks finding** (anything beyond the allowlisted baseline) → **stop**.
- **Any trufflehog `verified` secret** → **stop, treat as a live leak**.

To remediate a real leak:
1. Remove the secret from the code and **rotate/revoke** it at the provider — assume
   it is already compromised once committed.
2. If it was ever committed, it is in history: rewrite history
   (`git filter-repo` / BFG) and force-push **before** the repo goes public.
3. Only add an entry to `.gitleaks.toml` when you have **confirmed** the match is a
   non-secret (test fixture, documentation example). Never allowlist a real secret.

### One-shot

```bash
gitleaks detect --source . --config .gitleaks.toml --redact --no-banner \
  && gitleaks detect --source . --no-git --config .gitleaks.toml --redact --no-banner \
  && ~/local/bin/trufflehog git "file://$(pwd)" --no-update --only-verified --fail \
  && ~/local/bin/trufflehog filesystem . --no-update --only-verified --fail \
  && echo "SECRET SCAN CLEAN"
```
