# Working on this repo (Noah's CDDA fork)

Multiplayer / fluid mode work happens on the branch `claude/fluid-proto`. Before anything else read
`doc/MULTIPLAYER_HANDOFF.md` (section 0 is the newest) and the STATUS table at the top of
`doc/MULTIPLAYER_ROADMAP.md`. Noah's rules are in the handoff: no gameplay or balance change without
asking, say how every change was verified, keep the roadmap STATUS current, never push to
`intrvnus-mod`, no PR, no force push, no Windows build unless Noah asks.

## Quality rule from Noah (2026-10-15)
Quality over quantity: never take the easier option because it is less work. When a clean solution and a shortcut both exist, take the harder, cleaner one if it makes the product better (example of what not to do: hosting by copying the game into a second folder and running a second game window). If a shortcut is truly unavoidable, say so and ask first.

## Standing rule from Noah (2026-10-15)
Anything that should affect a client (its body, its items, its actions) but affects the host's
character instead is a bug: fix it right away without asking. Effects of an item use the item's
owner, never the host. Remember that `get_player_character()` is already the client's body during a
replay (`remote_ui::replay_scope`); the real bugs are `get_avatar()` / `as_avatar()` uses and code
that runs outside a replay (activity finish, item timers, per-turn effects).

## Delegating to Gemini (for the lead agent, Claude Opus)

A second Claude Code instance that runs on Gemini (through a local LiteLLM proxy) can take
well-scoped tasks. It works in its own git worktree, `/home/user/cdda-gemini`, on the branch
`gemini-work`. You stay on the normal Anthropic API; nothing about your own session changes.

- The tooling lives outside the repo, in `/root/gemini/` (written by the cloud environment's setup
  script): `start-proxy.sh` (LiteLLM on localhost:4000), `models.sh` (models the key may use),
  `delegate-gemini.sh "task"` (runs one task, commits it on `gemini-work`, prints the diff stat).
  The key comes from the environment variable `GEMINI_API_KEY`; it is never written anywhere.
- Delegate only well-scoped work: JSON content, sprites, test scripts, docs, small isolated files.
  **Never** the movement or network sync core (`src/fluid*.cpp`, `src/net_*.cpp`, `src/bubbles*`,
  `src/remote_ui*`), and nothing that changes gameplay or balance unless Noah asked for it.
- Write the task so it stands alone: the files to touch (by path), what to change, what not to
  change, how it will be checked.
- One task at a time. After each: `git -C /home/user/cataclysm-dda-extended diff
  claude/fluid-proto..gemini-work`, read every line, and merge into `claude/fluid-proto` only if it
  is correct (then build/test as usual and say so). Discard bad work with a reset of `gemini-work`
  to `claude/fluid-proto`.
- If the proxy does not answer, run `/root/gemini/start-proxy.sh`; if `GEMINI_API_KEY` is missing,
  tell Noah (he sets it in the cloud environment settings; never ask for the key in chat).
- Tell Noah what was delegated and what came back, in the regular progress updates.

## If you are the delegated agent (Gemini)

- Do exactly the task you were given. Edit only the files the task names; create new files only if
  the task asks for them.
- Never merge, rebase, switch branches, commit, push or run git commands that change history; the
  lead agent does all of that.
- No balance or gameplay changes unless the task says so explicitly.
- Do not touch the movement or network sync core (`src/fluid*.cpp`, `src/net_*.cpp`,
  `src/bubbles*`, `src/remote_ui*`).
- Match the style of the surrounding code and JSON (the repo uses astyle-like C++ formatting and
  the JSON formatting of `data/json`).
- If the task is unclear or would need more than the named files, stop and say so in your answer
  instead of guessing.
