# Server experiment directory cleanup: 2026-08-07

The isolated server directory `/opt/iking/match_agent_flow_test` was organized
after confirming that no flight-control process was active. The stable teammate
directory `/opt/iking/match_agent` was not modified.

## Result

- Before: 326,021,120 bytes.
- After: 70,815,744 bytes.
- Reclaimed: 255,205,376 bytes.

Removed material consisted of obsolete or damaged build directories,
`build_probe`, the root `Testing` directory, archived run/back-up directories,
and a duplicate `src/match_flow` binary. The current source, tests, scripts,
configuration, locally protected secret files, and current build directory were
kept.

The protected teammate files retained the following SHA-256 values after the
cleanup:

```text
71fbbb918aebe9dd2e586d9207435cf38f24152011424cdb194a3b049d1793cb  /opt/iking/match_agent/match.cpp
d5758183a5e54a62c830fc900050915daf9608b521a2f1959a2c7e84e5a48476  /opt/iking/match_agent/match
```

A full pre-cleanup archive is retained privately outside this public Git
repository. It is excluded here because it contains historical binaries and
may contain credential remnants.
