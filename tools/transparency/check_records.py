# PROJECT: VerOS NT
# LICENSE: GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE: tools/transparency/check_records.py
# PURPOSE: Reject a Cursor commit that changes Windows behavior and omits its source note
# COPYRIGHT: Copyright 2026 Ryan Wilson <ryan@verosnt.com>
# PROGRAMMERS: Ryan Wilson

"""Check transparency notes for Cursor commits in a revision range."""

import json
import re
import subprocess
import sys
from datetime import datetime

SCOPED_PREFIXES = (
    "boot/",
    "ntoskrnl/",
    "hal/",
    "drivers/",
    "dll/",
    "win32ss/",
    "sdk/lib/fslib/ntfslib/",
)

DATE_FILE = re.compile(
    r"^veros/transparency/(\d{4}-\d{2}-\d{2})-[A-Za-z0-9][A-Za-z0-9-]*\.json$"
)
SHA_FILE = re.compile(r"^veros/transparency/([0-9a-f]{40})\.json$")
BANNED_FIELDS = ("forbiddenInputsNotUsed", "humanReview", "cursor")


def git(*args):
    result = subprocess.run(
        ["git", *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def is_scoped(path):
    return path.startswith(SCOPED_PREFIXES)


def require_text(record, field, errors, path):
    value = record.get(field)
    if not isinstance(value, str) or not value.strip():
        errors.append("%s: %s must be a non-empty string" % (path, field))
        return None
    return value


def cites_source(item):
    lowered = item.lower()
    if "https://" in lowered or "http://" in lowered or "/" in item:
        return True
    return lowered.startswith("reported:")


def validate_record(record, path):
    errors = []
    if not isinstance(record, dict):
        return ["%s: the note must be a JSON object" % path]
    for field in BANNED_FIELDS:
        if field in record:
            errors.append("%s: leave %s out of the note" % (path, field))
    spdx = require_text(record, "spdx", errors, path)
    if spdx is not None and spdx != "GPL-2.0-or-later":
        errors.append("%s: spdx must be GPL-2.0-or-later" % path)
    require_text(record, "question", errors, path)
    inputs = record.get("allowedInputs")
    if (
        not isinstance(inputs, list)
        or not inputs
        or not all(isinstance(item, str) and item.strip() for item in inputs)
    ):
        errors.append("%s: allowedInputs must name the sources that were used" % path)
    elif not all(cites_source(item) for item in inputs):
        errors.append(
            "%s: each allowedInputs entry needs a URL, a path, or a Reported: citation"
            % path
        )
    if "harness" in record:
        harness = record["harness"]
        if not isinstance(harness, dict):
            errors.append("%s: harness must name the guest that ran" % path)
        else:
            for field in ("guest", "name", "traceOrReplayId"):
                value = harness.get(field)
                if not isinstance(value, str) or not value.strip() or value == "none":
                    errors.append("%s: harness.%s must name the guest that ran" % (path, field))
    if "measuredEffect" in record:
        effect = record["measuredEffect"]
        if not isinstance(effect, dict):
            errors.append("%s: measuredEffect must come from a committed log" % path)
        else:
            metric = effect.get("metric")
            if not isinstance(metric, str) or not metric.strip():
                errors.append("%s: measuredEffect.metric must name the log or snapshot" % path)
            for field in ("before", "after"):
                if effect.get(field) is None or isinstance(effect.get(field), bool):
                    errors.append(
                        "%s: measuredEffect.%s must come from a committed log" % (path, field)
                    )
    return errors


def notes_for_commit(sha):
    added = git(
        "diff-tree",
        "--no-commit-id",
        "--name-only",
        "-r",
        "--diff-filter=A",
        sha,
    ).splitlines()
    return [path for path in added if path.startswith("veros/transparency/")]


def check_commit(sha):
    body = git("log", "-1", "--format=%B", sha)
    if "co-authored-by: cursor" not in body.lower():
        return []
    changed = git("diff-tree", "--no-commit-id", "--name-only", "-r", sha).splitlines()
    if not any(is_scoped(path) for path in changed):
        return []

    errors = []
    date_files = []
    for path in notes_for_commit(sha):
        date_match = DATE_FILE.match(path)
        sha_match = SHA_FILE.match(path)
        if date_match:
            try:
                datetime.strptime(date_match.group(1), "%Y-%m-%d")
            except ValueError:
                errors.append("%s: %s has a bad date" % (sha, path))
            date_files.append(path)
        elif sha_match:
            if sha_match.group(1) == sha:
                errors.append("%s: %s cannot be named with this commit" % (sha, path))
        else:
            errors.append("%s: %s is not a date slug or an earlier commit SHA" % (sha, path))
    if len(date_files) != 1:
        errors.append(
            "%s: a Windows-behavior commit adds one veros/transparency/<yyyy-mm-dd>-<slug>.json"
            % sha
        )
        return errors

    shown = git("show", "%s:%s" % (sha, date_files[0]))
    try:
        record = json.loads(shown)
    except json.JSONDecodeError as exc:
        errors.append("%s: %s is not JSON (%s)" % (sha, date_files[0], exc))
        return errors
    errors.extend(validate_record(record, date_files[0]))
    return errors


def main(argv):
    if len(argv) != 2:
        print("usage: check_records.py <revision-range>", file=sys.stderr)
        return 2
    listed = git("rev-list", "--reverse", "--no-merges", argv[1])
    errors = []
    for sha in listed.splitlines():
        if sha:
            errors.extend(check_commit(sha))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
