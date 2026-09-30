# Project style settings

Per-project settings that the shared [STYLE.md](STYLE.md) delegates here.

- **Macro prefix.** None reserved. Vulkan and protocol macros keep their
  external spelling.
- **Namespace.** The suite is a program: no project namespace. The vendored
  `plt` platform library keeps its `plt` namespace, `libstd` its `stl`.
- **Formatter.** `./dev/style.py` formats every tracked C++ source.

## Deviations

- Every tool is one entry point behind `main.cpp`'s table, and runs as its
  own process: a failure the tool cannot recover from reaches its
  top-level handler and ends that process, never the caller.
- GPU allocation with a real fallback (a swapchain rebuilt, a device without
  an extension taking the slower path) is a meaningful local recovery path.
  `vkc` may be caught at such a boundary; without one its failure reaches the
  top-level handler.
