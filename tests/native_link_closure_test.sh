#!/bin/bash
# Copyright 2026 FogCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: native_link_closure_test.sh ROOT BUILD_DIR" >&2
	exit 2
fi

root=$1
build_dir=$2
native_makefile="$root/runtime/native/Makefile"
nm_tool=${NM:-nm}
cxxfilt_tool=${CXXFILT:-c++filt}

[[ -f "$native_makefile" ]] || {
	echo "native personality Makefile is missing" >&2
	exit 1
}

rm -rf -- "$build_dir"
mkdir -p -- "$build_dir"

make -f "$native_makefile" ROOT="$root" BUILD_DIR="$build_dir" all

archive="$build_dir/libfogcast-native-personality.a"
relocatable="$build_dir/fogcast-native-personality.o"
source_manifest="$build_dir/native-personality.sources"
object_manifest="$build_dir/native-personality.objects"
link_map="$build_dir/native-personality.map"
dependency_manifest="$build_dir/native-personality.dependencies"

for artifact in "$archive" "$relocatable" "$source_manifest" \
	"$object_manifest" "$link_map" "$dependency_manifest"; do
	[[ -s "$artifact" ]] || {
		echo "native closure artifact missing or empty: $artifact" >&2
		exit 1
	}
	done

expected_sources="$build_dir/expected.sources"
cat >"$expected_sources" <<'EOF'
runtime/mister_runtime.cpp
runtime/mister_runtime_v2.cpp
runtime/native/hardware_broker.cpp
runtime/native/linux/native_audio_adapter.cpp
runtime/native/linux/native_av_io_adapter.cpp
runtime/native/linux/native_content_adapter.cpp
runtime/native/linux/native_core_artifact_adapter.cpp
runtime/native/linux/native_core_protocol_io_adapter.cpp
runtime/native/linux/native_fpga_programmer.cpp
runtime/native/linux/native_input_adapter.cpp
runtime/native/linux/native_linux_v2_context.cpp
runtime/native/linux/native_mmio_adapter.cpp
runtime/native/linux/native_offload_adapter.cpp
runtime/native/linux/native_save_adapter.cpp
runtime/native/linux/native_scheduler_adapter.cpp
runtime/native/linux/native_video_adapter.cpp
runtime/native/native_containment.cpp
runtime/native/native_core_profile.cpp
runtime/native/native_core_protocol.cpp
runtime/native/native_input.cpp
runtime/native/native_lifecycle.cpp
runtime/native/native_peripheral_session.cpp
runtime/native/native_recovery.cpp
runtime/native/native_save_key.cpp
runtime/native/native_snes_content.cpp
runtime/native/native_spi_bus.cpp
EOF

LC_ALL=C sort -c "$source_manifest"
cmp "$expected_sources" "$source_manifest"

expected_objects="$build_dir/expected.objects"
sed -e 's|^|objects/|' -e 's|\.cpp$|.o|' "$expected_sources" >"$expected_objects"
LC_ALL=C sort -c "$object_manifest"
cmp "$expected_objects" "$object_manifest"

for forbidden in \
	main.cpp runtime/mister_runtime_legacy.cpp runtime/mister_runtime_linux_v2.cpp \
	scheduler.cpp offload.cpp input.cpp user_io.cpp fpga_io.cpp; do
	if grep -Fx "$forbidden" "$source_manifest" >/dev/null; then
		echo "native closure contains forbidden compatibility source: $forbidden" >&2
		exit 1
	fi
	done

for required in \
	runtime/native/linux/native_core_artifact_adapter.cpp \
	runtime/native/linux/native_fpga_programmer.cpp \
	runtime/native/linux/native_mmio_adapter.cpp \
	runtime/native/linux/native_core_protocol_io_adapter.cpp; do
	grep -Fx "$required" "$source_manifest" >/dev/null || {
		echo "native closure omits required source: $required" >&2
		exit 1
	}
	done

if "$nm_tool" -u "$build_dir/objects/runtime/native/linux/native_fpga_programmer.o" | \
	grep -E '(^|_)(f?open|openat|stat|lstat|fstatat)(64)?($|@|_)' >/dev/null; then
	echo "native FPGA programmer can reopen a core by path" >&2
	exit 1
fi

undefined_symbols="$build_dir/native-personality.undefined"
"$nm_tool" -u "$relocatable" | "$cxxfilt_tool" >"$undefined_symbols"
if grep -E '(^|[^[:alnum:]_])(fpga_load_rbf|user_io_|scheduler_|offload_|reboot|reexec|execl|system)($|[^[:alnum:]_])' \
	"$undefined_symbols" >/dev/null; then
	echo "native closure references compatibility mutation authority" >&2
	exit 1
fi

global_symbols="$build_dir/native-personality.globals"
"$nm_tool" -g "$archive" | "$cxxfilt_tool" >"$global_symbols"
if grep -E 'Native(Mmio|CoreProtocolIo)(Test|LinuxOperations)|PosixOperations' \
	"$global_symbols" >/dev/null; then
	echo "native closure exports raw MMIO/SPI operations" >&2
	exit 1
fi

raw_owners="$build_dir/native-personality.raw-owners"
while IFS= read -r object; do
	if "$nm_tool" -u "$build_dir/$object" | \
		grep -E '(^|[[:space:]])_?(close|ioctl|mmap|munmap|open|open64|openat|pread|pread64|pwrite|pwrite64)(@.*)?$' \
		>/dev/null; then
		printf '%s\n' "$object" >>"$raw_owners"
	fi
done <"$object_manifest"
cat >"$raw_owners.expected" <<'EOF'
objects/runtime/native/linux/native_core_artifact_adapter.o
objects/runtime/native/linux/native_core_protocol_io_adapter.o
objects/runtime/native/linux/native_input_adapter.o
objects/runtime/native/linux/native_mmio_adapter.o
EOF
cmp "$raw_owners.expected" "$raw_owners"

grep -F 'runtime/native/linux/native_core_artifact_adapter.hpp' \
	"$dependency_manifest" >/dev/null || {
	echo "native dependency closure omits the core artifact adapter" >&2
	exit 1
}

for object in $(cat "$object_manifest"); do
	grep -F "$object" "$link_map" >/dev/null || {
		echo "native link map omits object: $object" >&2
		exit 1
	}
	done
