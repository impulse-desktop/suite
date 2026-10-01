{% extends '//lib/openal/ix.sh' %}

{% block lib_deps %}
lib/c
lib/c++
lib/darwin/framework/CoreAudio
lib/darwin/framework/AudioToolbox
lib/darwin/framework/CoreFoundation
{% endblock %}

{% block bld_libs %}
{% endblock %}

{% block cmake_flags %}
{{super().replace('ALSOFT_BACKEND_SNDIO=ON', 'ALSOFT_BACKEND_SNDIO=OFF')}}
ALSOFT_BACKEND_COREAUDIO=ON
{% endblock %}
