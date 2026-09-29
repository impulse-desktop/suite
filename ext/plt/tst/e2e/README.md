# Real compositor scenarios

Every scenario is a C++ application using plt's public API and a paired Python
script. The driver configures the application through environment variables,
starts an isolated headless Sway compositor, injects real keyboard and pointer
input, and checks pixels captured by grim. Missing tools and timeouts fail the
run. Each scenario preserves client/compositor logs, PNG screenshots and Sway
window geometry; the runner writes `results.json` and continues after failures.

Build and run with libstd configured as for the ordinary library build:

```sh
./build e2e-binaries
./build e2e
python3 tst/e2e/run.py --binary-dir .build/e2e \
    --artifacts .build/e2e-lavapipe --renderer lavapipe
```

`e2e` runs the shm path. The lavapipe path creates a real Wayland Vulkan surface
and swapchain and requires a CPU Vulkan device. Set `VK_DRIVER_FILES` to Mesa's
`lvp_icd*.json` and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` to reproduce
CI. Vulkan validation errors fail the scenario. Use `--filter gallery` to run
one pair.

Build dependencies additionally include Cairo and Vulkan development packages.
Runtime tools: Sway, swaymsg, swaybg, grim, wtype, wl-copy and wl-paste, plus
DejaVu fonts. Lavapipe needs Mesa Vulkan drivers and Vulkan validation layers.
Run as a regular user with writable temporary storage and shared memory.
`dev/ci_linux.sh` installs the CI dependencies.

| Program | Scenario |
| --- | --- |
| gallery | Presentation, resizing, fractional and integer output scales |
| editor | Text entry and deletion |
| pointer | Clicking, dragging and pointer icons |
| scroller | Wheel input and repainting |
| clipboard | Streaming copy/paste with independent clipboard clients |
| primary | Primary selection exchange through mouse input |
| windows | Multiple windows and focus changes |
| window_state | Fullscreen, resize, title and compositor close |
| animation | Timers and repeated frame requests |
| pipe_dashboard | Fiber waiting for external FIFO data |
| worker | Background thread progress and event loop wakeups |
| render_retry | Retrying deferred rendering |

Every Linux test job (GCC/glibc, Clang/musl, Clang/ASan, Clang/UBSan and
coverage) runs both renderers, using the same compiler and instrumentation as
the Wayland integration tests. All jobs are gated by `build`. Linux coverage includes their production code coverage
in the report merged with Darwin. Screenshots and logs are uploaded on success
and failure.
