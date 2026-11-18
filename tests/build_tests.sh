#!/bin/bash
# Builds fieldprep and every test from scratch on Linux (g++ 13 or later; the shader tests need Mesa's EGL and GL:
# libegl1-mesa-dev libgl1-mesa-dev, software rendering is fine), into the folder given (default: tests/out).
#   tests/build_tests.sh [output folder]
# Everything is rebuilt every time on purpose: an object left over from before a header changed links without complaint
# and then reads the wrong fields. tests/run_tests.sh runs what this builds.
set -e
cd "$(dirname "$0")/.."
OUT="${1:-tests/out}"
mkdir -p "$OUT/obj"
rm -f "$OUT"/obj/*.o
CXX="${CXX:-g++}"
FLAGS="-std=c++20 -O2 -pthread -w"
compile() { # source, object name
	$CXX $FLAGS -Iff8_demaster -c "$1" -o "$OUT/obj/$2.o"
}
pids=()
for f in field_pbr field_moving_lights field_layers field_chars world_vehicles field_tween field_mirror analog_move scene_light; do compile ff8_demaster/$f.cpp $f & pids+=($!); done
for f in prep image_io archive models main; do compile fieldprep/$f.cpp $f & pids+=($!); done
for f in lodepng bc7enc bc7decomp; do compile fieldprep/third_party/$f.cpp $f & pids+=($!); done
for f in field_pbr_test field_layers_test field_chars_test fieldprep_model_test world_vehicles_test pbr_shader_test chara_shader_test field_depth_test \
	field_tween_test mirror_test scene_shader_test; do
	compile tests/$f.cpp $f & pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done
O="$OUT/obj"
THIRD="$O/lodepng.o $O/bc7enc.o $O/bc7decomp.o"
link() { out="$1"; shift; $CXX -pthread "$@" -o "$OUT/$out"; }
link fieldprep $O/main.o $O/prep.o $O/models.o $O/image_io.o $O/archive.o $O/field_pbr.o $O/field_layers.o $THIRD
link field_pbr_test $O/field_pbr_test.o $O/scene_light.o $O/field_pbr.o $O/field_moving_lights.o $O/field_layers.o $O/prep.o $O/image_io.o $O/archive.o $THIRD
link field_layers_test $O/field_layers_test.o $O/field_layers.o
link field_chars_test $O/field_chars_test.o $O/field_chars.o $O/field_pbr.o
link fieldprep_model_test $O/fieldprep_model_test.o $O/models.o $O/image_io.o $O/field_pbr.o $O/field_layers.o $THIRD
link world_vehicles_test $O/world_vehicles_test.o $O/world_vehicles.o
link pbr_shader_test $O/pbr_shader_test.o $O/field_layers.o $O/field_pbr.o $O/field_moving_lights.o $O/image_io.o $THIRD -lEGL -lGL
link chara_shader_test $O/chara_shader_test.o -lEGL -lGL
link field_depth_test $O/field_depth_test.o -lEGL -lGL
link field_tween_test $O/field_tween_test.o $O/field_tween.o $O/analog_move.o
link mirror_test $O/mirror_test.o $O/field_mirror.o -lEGL -lGL
link scene_shader_test $O/scene_shader_test.o $O/scene_light.o $O/field_pbr.o $O/field_moving_lights.o -lEGL -lGL
echo "built into $OUT"
