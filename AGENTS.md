<!-- memmy:start v=1 -->
# Memmy Memory

The `memmy-memory` skill is installed at `skills/memmy-memory/SKILL.md`.
Use that skill when prior memory may be relevant to the current request.
<!-- memmy:end v=1 -->

# Project maintenance

- Keep the public plugin SDK under `src/client/api/`, application implementation under `src/client/main/`, plugin projects under `src/plugins/<plugin>/client/`, and tests under `src/tests/`. The API must not depend on host implementation headers or sources. Respect an explicitly requested alternative plugin directory.
- The Linux server follows the same separation under `src/server/api/` and `src/server/main/`, with plugins under `src/plugins/<plugin>/server/`. Shared public tools and wire protocol live under `src/shared/include/`; plugin-specific shared code lives under `src/plugins/<plugin>/shared/`, including the voice protocol in `src/plugins/voice-chat/shared/`. Both applications and SDKs derive versions from root `VERSION`, with distinct client/server plugin IIDs and packages.
- Application version numbers are managed by the developer. Do not automatically edit root `VERSION` for functionality, build, documentation, fixes, or optimizations; change it only when explicitly instructed by the developer. Record pending changes in an unreleased `CHANGELOG.md` section when no new version has been assigned.
- Before the first official release, use application versions `0.<core>.<patch>` and API versions `0.<core>`. The major version stays 0; the current core/API generation is 4. When the developer assigns a non-core update version, change only the patch; core/API generation changes update the middle number and reset the patch to 0. Never change the major version or core generation without an explicit user request.
- `VERSION` is the single source of truth for application, About dialog, Windows file metadata, and macOS bundle versions. Do not hard-code separate version values in source code.
- Derive API version constants and SDK package metadata from the first two components of `VERSION`. Pre-release plugins must match both API components exactly; application patch changes do not change plugin compatibility.
- The developer explicitly assigned application `0.4.1` (API `0.4`) for the current release. Record future unassigned changes under the unreleased changelog section; see the version-upgrade validation in `docs/maintenance.md`. The repository currently retains only `main`; do not recreate the retired `dev-net` or `codex/restore-ui-functions` branches without a developer request.

# Plugin development skill

When creating, modifying, or independently compiling a plugin, read [ztgui-plugin-dev](.agent/skills/ztgui-plugin-dev/SKILL.md). This project skill covers directory creation, public API dependencies, implementation, and compilation.

# Build and packaging skill

For application builds and release packaging, read [ztgui-build](.agent/skills/ztgui-build/SKILL.md). Build and compress the main application; update and compress the public API SDK only when its inputs or package changed. Do not build plugins unless the user requests them. Packaging implementation lives in the build skill's `scripts/` directory.

# Documentation

Keep README concise: architecture, structure, platforms, building, and plugin development. Preserve non-obvious deployment assumptions, design constraints, and validation history in `docs/`; do not archive removed UI usage instructions or facts readily discoverable from source. See [maintenance notes](docs/maintenance.md).

Plugin documentation is owned by each plugin project and lives under `src/plugins/<plugin>/client/docs/` or `src/plugins/<plugin>/server/docs/`. Root documentation may link to it, but should not maintain a separate copy. Application icons and resource templates live under `src/client/assets/`.
