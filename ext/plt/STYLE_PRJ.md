# Project style settings

Per-project settings that the shared [STYLE.md](STYLE.md) delegates here.

- **Macro prefix.** Project-owned macros use a `PLATFORM_` prefix.
- **Namespace.** Public declarations live in the `plt` namespace.
  Implementation files use `using namespace plt;` instead of wrapping
  definitions in `namespace plt`.
- **Formatter.** `./dev/style.py` formats every tracked C++ source.

## Deviations

- `STD_VERIFY` throws, and the only catch is around the whole event loop: a
  failure there ends the session. Use it for our own invariants only —
  startup, resources sized by the platform and state we created.
- An allocation or resource sized by client input never goes through a
  throwing macro. Check the result in place and degrade: cap the size before
  allocating, skip the content with a log line, or reject the offending
  request. Client input must not be able to reach the top-level catch.
