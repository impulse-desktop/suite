{# the suite's macOS cross build from Linux, for a check without a Mac:
   host tools around, the target's libraries carrying their target, so the
   realm stays a host one. The compiler is the toolchain's own (the one that
   built these libraries): a second clang in the realm brings a second copy
   of the resource headers, and include_next stops at it short of the SDK #}

{% extends '//die/hub.sh' %}

{% block run_deps %}
bin/wabt
bin/glslang
bin/pkg/config
bin/python
lib/molten/vk(target=darwin-aarch64,kind=lib)
lib/png(target=darwin-aarch64,kind=lib)
lib/jxl(target=darwin-aarch64,kind=lib)
lib/vulkan/headers(target=darwin-aarch64,kind=lib)
lib/c++(target=darwin-aarch64,kind=lib)
{% endblock %}
