#!/usr/bin/env bash
# Files one GitHub issue per bug report in this folder, labelled "bug".
# Needs the GitHub CLI, logged in: gh auth login
# Usage (from anywhere): bash docs/bugs/create_issues.sh
set -euo pipefail
cd "$(dirname "$0")"
REPO="${REPO:-aarav710/sf-skate}"
for f in [0-9][0-9]-*.md; do
  title=$(head -n1 "$f" | sed 's/^# //')
  tail -n +3 "$f" > /tmp/issue_body.md
  gh issue create --repo "$REPO" --title "$title" --body-file /tmp/issue_body.md --label bug
done
