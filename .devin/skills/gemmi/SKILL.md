---
name: gemmi
description: Report agent activity to the Gemmi physical device (ESP32 panel) via the local bridge. Use when an agent should show status, stream activity lines, ask yes/no or multiple-choice questions, or share artifact references with the human watching Gemmi.
---

# Gemmi

Gemmi is a physical Tamagotchi-style panel supervising agents. The `gemmi` CLI
talks to a local bridge (`http://127.0.0.1:8732`, auth token from
`$GEMMI_TOKEN` or `bridge/.env`) which queues events for the device.

CLI: `./gemware/bin/gemmi` (repo root: `/home/hiatus/Projects/htv11`).

## Commands

```bash
gemmi notify --agent NAME --state STATE "short message"   # status event
gemmi log    --agent NAME "tool: pytest -x"               # activity line (feeds agents view)
gemmi ask    --agent NAME --state needs-attention \
             --choice "Allow" --choice "Deny" "question"  # up to 3 choices
gemmi ask    --wait 120 ...                               # block until answered
gemmi wait   EVENT_ID                                     # collect a posted ask's answer
gemmi artifact --agent NAME --artifact "shot.png" "desc"  # artifact reference
gemmi events                                            # queue + open asks
gemmi sim    --auto                                       # software device for testing
```

## States

`idle` `working` `thinking` `reading` `needs-attention` `success` `done`
`completed` `error` `failed`

Posting `success`/`done`/`completed` after the agent has been active earns the
pet gems/XP — report real terminal states, not noise. Log lines prefixed
`tool:` `think:` `warn:` `err:` render color-coded in the per-agent feed.

## Rules

- Keep messages short — the panel is 240px wide.
- Never post secrets, tokens, or private source content.
- If the bridge is unreachable, continue the task; mention the failure only
  if it matters.
- Asks persist on the device until answered; don't repost the same question.
