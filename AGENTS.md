<!-- memmy:start v=1 -->
# Memmy Memory

The `memmy-memory` skill is installed at `skills/memmy-memory/SKILL.md`.
Use that skill when prior memory may be relevant to the current request.
<!-- memmy:end v=1 -->

# Project maintenance

- Keep the public plugin SDK under `src/api/`, application implementation under `src/main/`, plugin projects under `src/plugins/`, and tests under `tests/`. The API must not depend on host implementation headers or sources. Respect an explicitly requested alternative plugin directory.
- Before the first official release, use application versions `0.<core>.<patch>` and API versions `0.<core>`. The major version stays 0; the current core/API generation is 2. Non-core feature updates, fixes, and optimizations increment only the application patch (currently 2). Core/API generation changes increment the middle number, update the API version with it, and reset the patch to 0. Never change the major version or core generation without an explicit user request.
- For changes to application functionality or build behavior, increment the application version in root `VERSION` and add a `CHANGELOG.md` entry in the same change. Documentation-only edits do not require a version increment unless explicitly requested. Do not increment again for intermediate edits within the same update.
- `VERSION` is the single source of truth for application, About dialog, Windows file metadata, and macOS bundle versions. Do not hard-code separate version values in source code.
- Derive API version constants and SDK package metadata from the first two components of `VERSION`. Pre-release plugins must match both API components exactly; application patch changes do not change plugin compatibility.

# Plugin development skill

When creating, modifying, or independently compiling a plugin, read [ztgui-plugin-dev](.agent/skills/ztgui-plugin-dev/SKILL.md). This project skill covers directory creation, public API dependencies, implementation, and compilation.

# Documentation

Keep README concise: architecture, structure, platforms, building, and plugin development. Preserve non-obvious deployment assumptions, design constraints, and validation history in `docs/`; do not archive removed UI usage instructions or facts readily discoverable from source. See [maintenance notes](docs/maintenance.md).
