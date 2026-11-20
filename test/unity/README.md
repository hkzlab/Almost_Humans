# Unity

Vendored test framework, **unmodified**:

| file | upstream path |
|---|---|
| `unity.c` | `src/unity.c` |
| `unity.h` | `src/unity.h` |
| `unity_internals.h` | `src/unity_internals.h` |
| `LICENSE.txt` | `LICENSE.txt` |

- Upstream: <https://github.com/ThrowTheSwitch/Unity>
- Version: `v2.7.0`
- Licence: MIT (see `LICENSE.txt`), compatible with this project's GPLv3.

Do not edit or reformat these files: keeping them identical makes a future
version bump a plain file replacement.  The CMake target sets `SKIP_LINTING`
for the same reason.  The suite's own glue is `test/test_support.h/.c`.
