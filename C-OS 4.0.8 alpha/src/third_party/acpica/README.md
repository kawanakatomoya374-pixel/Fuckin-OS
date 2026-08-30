# ACPICA source tree

This vendored tree contains only the kernel runtime ACPICA sources needed by the OS build.

Build integration expects the runtime sources under `third_party/acpica/source` and the
kernel-side OS services layer in `kernel/power/acpica_backend.c`.

Kept:
- `source/include`
- `source/components/dispatcher`
- `source/components/events`
- `source/components/executer`
- `source/components/hardware`
- `source/components/namespace`
- `source/components/parser`
- `source/components/resources`
- `source/components/tables`
- `source/components/utilities`

Excluded: compiler, tools, tests, and documentation.
