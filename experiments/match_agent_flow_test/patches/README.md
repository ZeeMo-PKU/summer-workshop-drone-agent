# Student Practice monitor patch

`student-practice-portable-site.patch` records the project-specific changes to
the original `SummerWorkshop-Student-Practice` monitor package:

- enable a portable site with a 20 m horizontal radius and 10 m altitude limit;
- add `scripts/set-portable-site.ps1` for setting the site anchor;
- validate the portable-site environment in the launcher;
- enforce the same circular horizontal and vertical envelope in the server.

Apply it from the root of a pristine extracted monitor package:

```powershell
git apply --check -p2 --unsafe-paths path\to\student-practice-portable-site.patch
git apply -p2 --unsafe-paths path\to\student-practice-portable-site.patch
```

The `--check` step must succeed before applying. This patch changes only the
monitor/simulation package; it does not start a controller or send flight
commands.
