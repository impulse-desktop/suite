{% extends '//lib/ffmpeg/7/ix.sh' %}

{% block lib_deps %}
lib/c
lib/z
{% endblock %}

{% block configure_all_flags %}
--enable-cross-compile
--target-os=darwin
--arch=aarch64
--cc=clang
--cxx=clang++
--prefix=${out}
--enable-static
--disable-shared
--disable-programs
--disable-doc
--disable-avdevice
--disable-avfilter
--disable-postproc
--disable-encoders
--disable-muxers
--disable-autodetect
--disable-stripping
--enable-zlib
{% endblock %}
