# Security Policy

`knk` is an embedded, in-process library and a local stdio MCP server (no network listener, no
socket, no auth surface of its own — see `AGENTS.md`'s "Do Not Do Yet" section). It's an early-stage,
solo-maintained project, so treat this policy as best-effort rather than a formal SLA.

## Reporting a vulnerability

Please **do not** open a public GitHub issue for a suspected security vulnerability. Instead, email
cristiano.solarino@gmail.com with:

- A description of the issue and its potential impact.
- Steps to reproduce (a minimal repro is very helpful).
- Any relevant version/commit information.

You should get an acknowledgment within a few days. There is no bug-bounty program.

## Supported versions

The project has not yet cut a tagged release — only `main` is currently supported. Fixes land there
first.
