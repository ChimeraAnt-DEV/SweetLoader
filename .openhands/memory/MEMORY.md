# FreeHands — project memory

FreeHands = a fork of OpenHands Agent Canvas (`Lynxifyy/FreeHands`) used as a
coding app with DeepSeek V4.1 Flash, plus a Capacitor Android shell and a
forked agent-server SDK (`Lynxifyy/software-agent-sdk-2.0`).

## Branches and PRs
- Long-lived feature branch: `feat/discord-automations-and-bot-connect`
  (local ref `feat-discord`). It carries all the app-bundle work.
- Open PRs: FreeHands **#5** (draft, ready to merge). SDK **#2** is **merged**
  (merge commit `d0da07d`); the FreeHands SDK pin now points at that merged SHA.
- `main` has **no branch protection**, so the red fork-only checks do not block
  a merge.
- PR #4 (`feat/ui-preview-critique-tools`) **merged** to main (`840934a`).
  #4 and #5 were independent siblings off `e78c813` that both added client tools
  at the same seams, so #5 needed a merge of main first (merge commit `fc63fa24`).
  Conflicts were all "keep both": action/type-guard/base unions, the adapter
  `client_tools` list, the websocket dispatch, action titles, client-tool result
  prefixes, and `translation.json` (union of both branches' keys — merge it
  programmatically by parsing each side as JSON, never by hand).

## Agent-tools workstream (the "phases" from the Chat-recovery- HTML chat)
The original plan lived in `Lynxifyy/Chat-recovery-`
(`conversation-14eef971253347218f1067053099cc7f.html`). Phases are done **two per
PR**, then merged, then stop for the user's confirmation. Progress:
- **PR #6** (merged `2d2b51f`): Phase 1 live task list + **green** done tick
  (`text-success`, in both `task-tracking/task-item.tsx` files) and Phase 2
  in-app settings (no browser escape from the packaged app).
- **PR #7** (merged `6bf357c`): Phase 3 model-aware thinking status
  (`thinkingModelLabel` in `src/utils/format-model-name.ts`, `ACTION_MESSAGE$MODEL_THINKING`)
  and Phase 4 Oli presence (`src/utils/oli-activity.ts` derives `deriveOliStatus`
  from an unresolved `TaskAction` whose `subagent_type` is `oli`;
  `OliRobotHead` in `src/components/features/chat/oli-robot-head.tsx`, blue
  `bg-logo`, `oli-think`/`oli-code`/`oli-antenna` keyframes in `tailwind.css`).
  Also fixed a stale `task-tracking-item-status.test.tsx` assertion left on main.
- **Phases 5+6 → FreeHands PR #8** (open, branch `feat/discord-recommended-and-bot-token` @ `3c971c9`):
  Discord now appears in Recommended automations through a host catalog entry
  `DISCORD_RECOMMENDED_AUTOMATION` in `src/constants/discord.ts`
  (+ `getHostAutomationCatalog()` in `src/utils/recommended-automation-rail.ts`,
  used by both the rail and the section) and a bundled `src/icons/discord.svg`
  registered in `McpLogoBadge.LOCAL_LOGO_ICONS`. The launcher routes the Discord
  card to the guided setup. Bot-token entry lives in the DiscordConnectModal;
  the token is saved as `DISCORD_BOT_TOKEN` via `SecretsService` and never posted
  to chat (`src/services/discord-connect.ts`).
- **Phase 7 → FreeHands PR #9** (open, branch `feat/keep-sandboxes-alive-setting` @ `a412552`):
  new app preference `always_keep_sandbox_alive` (App Settings toggle
  `SETTINGS$ALWAYS_KEEP_SANDBOX_ALIVE`); `pauseConversation` honors
  `{ keepSandboxAlive }` so cloud mode skips pausing the sandbox and the stop
  hooks keep the runtime URL.
- **Phases 8+9 → SDK PR #3** (open, branch `feat/cloud-terminal-tool` @ `6d94a0f`,
  two commits `419b22b` + `6d94a0f`):
  - `openhands.tools.cloud_terminal`: one-shot command via workspace
    `execute_command` (works local + remote/cloud); default exec tool.
  - `openhands.tools.file_builder`: writes text/md, PNG (base64), ZIP, DOCX;
    default exec tool, paths confined to working dir.
  - builtin subagent `oli` (`preset/subagents/oli.md`): coding agent with
    terminal/file_editor/task_tracker/file_builder/cloud_terminal/verify.
  - All wired into `DEFAULT_EXEC_TOOL_NAMES` + `preset/default.py`; test lists
    updated (`test_settings.py`, `cross/test_default_tool_names.py`,
    `persisted_settings_baselines/v2/agent_profile_tool_switches.json`,
    `test_builtin_agents.py`).
- **Phase 10 → litellm PR #2** (open, branch `feat/deepseek-v4stream-metadata` @
  `997dd4fc`): add `supports_streaming`+`supports_parallel_function_calling` to
  deepseek-v4-pro/flash/v4.1-flash/v4.1-flash-vision-exp and `supports_vision` to
  vision-exp; test `tests/litellm/llms/deepseek/test_deepseek_v4_models.py`.

SDK/litellm test envs are created ad hoc with `uv venv /tmp/sdkvenv` +
`uv pip install --python <venv> -e <pkg> pytest [pytest-asyncio]`. litellm is
installed from the repo root (`-e .`).

## Git auth (important)
- `$GITHUB_TOKEN` / `$Github` are read-only for git push on Lynxifyy repos (403).
- `$GithubLynxifyy` has full `repo` scope: use it for `git push` and for the
  GitHub API (`create_pr` tool uses the read-only token and 403s — create PRs
  with a `curl` POST to the API instead).
- Merge convention: PRs are **squash**-merged. Use the async API
  `PUT /repos/{o}/{r}/pulls/{n}/merge-async` with `{"merge_method":"squash","sha":"<head>"}`
  (returns 202 + a UUID; poll `.../merge-async/{uuid}` until `status` is `merged`).
- PR titles must be conventional (`feat:`/`fix:`/…) or the `pr-title` check fails.
  A wrong title is fixable with `PATCH /pulls/{n}` after opening.
- PR #7 CI: `test-and-build (ubuntu)` + `(windows)`, `Build Android APK`,
  `pr-title`, `check-pr-artifacts` all green. Fork-only reds: `Validate PR
  description` (issues disabled) and `Build & Push` (pushes OpenHands' ghcr).

## Fork limitations
- **Issues are disabled** on both forks (`has_issues: false`), so the
  `Validate PR description` check can never pass (it requires a linked
  `ready-for-dev` issue). `Build & Push` also fails (pushes OpenHands' ghcr).
- The `HUMAN:` section of the PR body is reserved for the human author; agents
  must not fill it, which also fails the description check by design.
- CI that *does* pass on the fork and is worth watching: `test-and-build
  (ubuntu)` and `(windows)`, `Build Android APK` (the only verifier of the
  Kotlin changes), `Check SDK version consistency`, `pr-title`. All green on
  `feat/discord-automations-and-bot-connect` @ `b2f6582`.

## Build / verify
- `npm ci` then `npm run lint` (0 errors), `npm test` (~850 files / ~9.2k tests,
  ~11 min — background it), `npm run build`, `npm run build:lib`.
- The pre-commit hook (lint-staged → tsc) needs generated files first:
  `npm run make-i18n` and `npx react-router typegen`. Do not run `make-i18n`
  concurrently (it deletes `public/locales` and races).
- Android: no Java/Gradle in the dev container — Kotlin changes are only
  verified by the CI `Build Android APK` job.

## Secrets
- Discord bot token lives only as the `DISCORD_BOT_TOKEN` secret (never in
  source). Public bot application id is `1568005572769480704`.

## GlowberryClient (ChimeraAnt-DEV/GlowberryClient) — Android launcher
- **Native code lives in a git submodule**: `app/src/main/cpp/preloader`
  (remote `ChimeraAnt-DEV/preloader-android`, working branch `lynxifyy/preloader-hardening`).
  A change under `app/src/main/cpp/preloader/**` must be committed **inside the submodule**,
  pushed, then the parent repo must commit the bumped submodule pointer. `git add` of a
  submodule path in the parent stages the gitlink, not the file.
  **Always check the pin is not behind the submodule's `main`** (`git -C <sub> merge-base
  --is-ancestor origin/main HEAD`). A `feat/*` submodule branch can silently skip a large
  `main` PR: the pin here was 13 commits behind main (frame-boundary clock, seqlock fixes,
  Optifine device/thread fixes, atomic sig-cache write, snapshot channel + TSAN). Merge
  `origin/main` into the working branch, resolve, run `tests/cosmetics/run_tests.sh`, push the
  submodule, then bump the parent pin. Merging main can also add JNI exports with no Java
  declaration → `JniBindingParityTest` fails with `stale-export`; add the Java `native` methods
  (`app/src/main/java/.../launcher/preloader/BerryApi.java`).
- **Build/test**: `JAVA_HOME=/workspace/tools/jdk-21.0.13+9 ANDROID_HOME=/workspace/android-sdk
  ./gradlew :app:testDebugUnitTest` (~3843 tests, ~25s, all green). Targeted:
  `--tests "org.chimeramc.client.<pkg>.*"`. Native host tests:
  `bash app/src/main/cpp/preloader/tests/cosmetics/run_tests.sh` (needs g++ + zlib + nlohmann).
- **BerryAPI ABI guard**: `python3 scripts/verify_berryapi_abi.py` (test:
  `scripts/tests/verify_berryapi_abi_test.py`). Pins the 14 dlsym'd `BERRY_*` symbols and the
  seam/replay/governor ABI numbers against the bundled `libBerryAPI.so`. **`BERRY_Inject` and
  `BERRY_IsAttached` return `int` (0/1), not C++ `bool`** — BerryAPI's header says a 1-byte bool
  return leaves the upper result register undefined. Official BerryAPI: `Lynxifyy/BerryAPI`
  (clone to /tmp to read headers; ABI 8, `BERRY_ABI_VERSION`).
- **Module catalogue**: `PvpModuleCatalog` = 200 entries (38 `link()` + 162 `add()`). Honest split:
  **50 reachable/toggleable**, **112 disabled/badged** (52 `ENGINE_HOOK` + 60 `SERVER_DECIDED`).
  Pinned by `PvpModuleScopesTest.theBadgeCountsMatchTheAuditNote`. The 52 ENGINE_HOOK need a
  **complete world seam** (the preloader binds only `get_local_player`; the stripped
  `libminecraftpe.so` gives no entity-list/level anchor) + a signature pack for the active build.
  The 60 SERVER_DECIDED cannot be enabled by any client code.
- **Setup wizard**: `SetupWizardActivity` is a self-sizing popup over `MainActivity`; each step
  shows only its own block; finishing calls `finishWizard()` → `MainActivity` (never a controller
  screen). Test `SetupWizardLastTabsTest` pins it.
- **Git author for this repo's commits**: `Lynxifyy <297587416+Lynxifyy@users.noreply.github.com>`,
  no co-authors, no AI mentions in PRs. Push with `$GithubLynxifyy` (has repo scope; `create_pr`
  tool's token is read-only here → use a `curl` POST to the API).
- **Two Android CI gates that fail locally-silent changes** (both run in `android.yml` before the
  Gradle build, so they fail fast): `python3 scripts/check_ui_hygiene.py` fails on *new*
  `findViewById` calls beyond a per-file baseline — read the view from the generated ViewBinding
  instead (or `--update` with a reason). `./gradlew :app:lintDebug` fails on a new
  `MissingSuperCall` — do not override `onBackPressed()` without calling `super`; register an
  `OnBackPressedCallback` on `getOnBackPressedDispatcher()` instead. Full unit suite is
  **3844 tests, 0 failures, 1 skipped** (~25s warm).
