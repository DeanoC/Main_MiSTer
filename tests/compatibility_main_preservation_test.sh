#!/bin/bash
# Copyright 2026 FogCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

if [[ $# -ne 3 ]]; then
	echo "usage: compatibility_main_preservation_test.sh ROOT MANIFEST MANIFEST_SHA256" >&2
	exit 2
fi

root=$1
manifest=$2
expected_manifest_sha=$3
baseline=984980ccffc4079cff364345cb7c62a544854452

[[ -f "$manifest" ]] || {
	echo "compatibility source manifest is missing" >&2
	exit 1
}

actual_manifest_sha=$(shasum -a 256 "$manifest" | awk '{print $1}')
[[ "$actual_manifest_sha" == "$expected_manifest_sha" ]] || {
	echo "compatibility manifest identity changed" >&2
	exit 1
}

paths=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-paths.XXXXXX")
sorted_paths=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-sorted.XXXXXX")
baseline_candidates=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-baseline-paths.XXXXXX")
current_candidates=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-current-paths.XXXXXX")
negative_graph=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-negative-graph.XXXXXX")
negative_command_graph=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-negative-command-graph.XXXXXX")
negative_commands=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-negative-commands.XXXXXX")
baseline_root=$(mktemp -d "${TMPDIR:-/tmp}/compatibility-main-baseline.XXXXXX")
current_root=$(mktemp -d "${TMPDIR:-/tmp}/compatibility-main-current.XXXXXX")
trap 'rm -f -- "$paths" "$sorted_paths" "$baseline_candidates" "$current_candidates" "$negative_graph" "$negative_command_graph" "$negative_commands"; rm -rf -- "$baseline_root" "$current_root"' EXIT

while IFS= read -r line; do
	[[ "$line" =~ ^([0-9a-f]{64})\ \ (.+)$ ]] || {
		echo "malformed compatibility manifest line" >&2
		exit 1
	}
	expected=${BASH_REMATCH[1]}
	path=${BASH_REMATCH[2]}
	[[ "$path" != /* && "$path" != *..* && "$path" != *//* ]] || {
		echo "unsafe compatibility manifest path: $path" >&2
		exit 1
	}
	printf '%s\n' "$path" >>"$paths"
	baseline_hash=$(git -C "$root" show "$baseline:$path" | shasum -a 256 | awk '{print $1}')
	[[ "$baseline_hash" == "$expected" ]] || {
		echo "manifest does not match baseline: $path" >&2
		exit 1
	}
	[[ -f "$root/$path" ]] || {
		echo "compatibility production input is missing: $path" >&2
		exit 1
	}
	current_hash=$(shasum -a 256 "$root/$path" | awk '{print $1}')
	[[ "$current_hash" == "$expected" ]] || {
		echo "compatibility production input changed: $path" >&2
		exit 1
	}
done <"$manifest"

LC_ALL=C sort -u "$paths" >"$sorted_paths"
cmp "$paths" "$sorted_paths"

git -C "$root" ls-tree -r --name-only "$baseline" | awk '
	/\.(c|cpp|h|hpp|png)$/ &&
	$0 !~ /^(tests|fogcast|runtime\/native|\.github|\.devcontainer)\// &&
	$0 !~ /^lib\/miniz\/examples\// &&
	$0 !~ /^lib\/libco\/(amd64|fiber|libco|ppc|sjlj|ucontext|x86)\.c$/
' | LC_ALL=C sort -u >"$baseline_candidates"
(
	git -C "$root" ls-files
	git -C "$root" ls-files --others --exclude-standard
) | awk '
	/\.(c|cpp|h|hpp|png)$/ &&
	$0 !~ /^(tests|fogcast|runtime\/native|\.github|\.devcontainer)\// &&
	$0 !~ /^lib\/miniz\/examples\// &&
	$0 !~ /^lib\/libco\/(amd64|fiber|libco|ppc|sjlj|ucontext|x86)\.c$/
' | LC_ALL=C sort -u >"$current_candidates"
cmp "$paths" "$baseline_candidates"
cmp "$paths" "$current_candidates"

for required in scheduler.cpp scheduler.h offload.cpp offload.h main.cpp \
	input.cpp input.h user_io.cpp user_io.h fpga_io.cpp fpga_io.h; do
	grep -Fx "$required" "$paths" >/dev/null || {
		echo "compatibility manifest omits required production input: $required" >&2
		exit 1
	}
	done

git -C "$root" archive "$baseline" | tar -x -C "$baseline_root"
git -C "$root" archive HEAD | tar -x -C "$current_root"

# The compatibility build definition is deliberately evaluated from the live
# candidate. The archived tree supplies unchanged inputs without copying build
# artifacts, while this overlay makes staged and unstaged Makefile changes
# visible to the pre-commit gate.
cp "$root/Makefile" "$current_root/Makefile"

extract_graph() {
	local directory=$1
	(
		cd "$directory"
		make -f Makefile -pn VDATE=260812 2>/dev/null
	) | awk '
		$1 == "C_SRC" || $1 == "CPP_SRC" || $1 == "IMG" || $1 == "RUNTIME_SRC" {
			if ($2 == "=" || $2 == ":=") {
				key=$1
				for (i=3; i<=NF; ++i) print key " " $i
			}
		}
	' | LC_ALL=C sort -u
}

extract_commands() {
	local directory=$1
	(
		cd "$directory"
		make -B -n -f Makefile V=1 VDATE=260812 bin/MiSTer
	) | LC_ALL=C sort -u
}

extract_graph "$baseline_root" >"$baseline_root/compatibility.graph"
extract_graph "$current_root" >"$current_root/compatibility.graph"
cmp "$baseline_root/compatibility.graph" "$current_root/compatibility.graph"

extract_commands "$baseline_root" >"$baseline_root/compatibility.commands"
extract_commands "$current_root" >"$current_root/compatibility.commands"
cmp "$baseline_root/compatibility.commands" "$current_root/compatibility.commands"

# Prove that both protected dimensions are live. These mutation controls would
# have passed when the candidate was evaluated from `git archive HEAD`.
cp "$current_root/Makefile" "$current_root/Makefile.saved"
printf '\nCPP_SRC += task8-preservation-negative.cpp\n' >>"$current_root/Makefile"
extract_graph "$current_root" >"$negative_graph"
if cmp -s "$baseline_root/compatibility.graph" "$negative_graph"; then
	echo "compatibility graph mutation was not detected" >&2
	exit 1
fi
mv "$current_root/Makefile.saved" "$current_root/Makefile"

cp "$current_root/Makefile" "$current_root/Makefile.saved"
printf '\nCFLAGS += -DTASK8_PRESERVATION_NEGATIVE\n' >>"$current_root/Makefile"
extract_graph "$current_root" >"$negative_command_graph"
cmp "$baseline_root/compatibility.graph" "$negative_command_graph"
extract_commands "$current_root" >"$negative_commands"
if cmp -s "$baseline_root/compatibility.commands" "$negative_commands"; then
	echo "compatibility command mutation was not detected" >&2
	exit 1
fi
mv "$current_root/Makefile.saved" "$current_root/Makefile"
