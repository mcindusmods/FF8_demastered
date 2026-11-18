#!/bin/bash
# Runs the tests tests/build_tests.sh built (default folder: tests/out).
#   tests/run_tests.sh [folder they were built into]
# Always: the shared code (field_pbr_test, field_layers_test, field_chars_test, field_tween_test), fieldprep's model
# textures (fieldprep_model_test) and the in-between picture's framebuffer copies on the GPU (mirror_test: Mesa is enough). The shader tests draw with the real shaders through Mesa and need things that aren't in the
# repository, so they run only when you say where those are:
#   FIELD=<a field's folder, prepared by fieldprep, PNG maps>   pbr_shader_test (the look, the moving lights, night, glows)
#   GAME_SHADERS=<folder with the game's main.vert, main_char.frag>   chara_shader_test
#   BENCH=<frames> with FIELD: also times the field shader per setup
# With glslangValidator installed (glslang-tools) every shader in DEMASTER_EXP/shaders is also checked strictly.
cd "$(dirname "$0")/.."
OUT="${1:-tests/out}"
fail=0
run() { echo "--- $*"; "$@" 2>&1 | tail -${LINES_SHOWN:-3}; [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1; }
run "$OUT/field_pbr_test"
run "$OUT/field_layers_test"
run "$OUT/field_chars_test"
run "$OUT/field_tween_test"
run "$OUT/mirror_test"
run "$OUT/scene_shader_test" DEMASTER_EXP/shaders
scratch=$(mktemp -d)
run "$OUT/fieldprep_model_test" "$scratch"
rm -rf "$scratch"
if [ -n "$FIELD" ]; then
	pictures=$(mktemp -d)
	LINES_SHOWN=40 run "$OUT/pbr_shader_test" "$FIELD" DEMASTER_EXP/shaders/demaster_field.frag "$pictures"
	echo "    its pictures: $pictures"
fi
# The shaders, read strictly as the GLSL version they declare. Mesa (which the shader tests draw with) lets things through
# that NVIDIA's compiler stops at: v15.8 to v15.10 shipped a field shader that never compiled on NVIDIA because of one.
if command -v glslangValidator >/dev/null; then
	echo "--- glslangValidator DEMASTER_EXP/shaders/*"
	for f in DEMASTER_EXP/shaders/*.frag DEMASTER_EXP/shaders/*.vert; do
		errors=$(glslangValidator "$f" 2>&1 | grep "ERROR" | grep -v "compilation errors")
		if [ -n "$errors" ]; then echo "$f"; echo "$errors" | head -5; fail=1; fi
	done
else
	echo "--- shaders not checked strictly: glslangValidator is not installed (Debian/Ubuntu: glslang-tools)"
fi
if [ -n "$GAME_SHADERS" ]; then run "$OUT/chara_shader_test" DEMASTER_EXP/shaders "$GAME_SHADERS"; fi
[ $fail -eq 0 ] && echo "all passed" || echo "FAILURES"
exit $fail
