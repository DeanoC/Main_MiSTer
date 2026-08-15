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
negative_dependency_recipe=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-negative-dependency-recipe.XXXXXX")
negative_dependency_commands=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-negative-dependency-commands.XXXXXX")
negative_dependency_targets=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-negative-dependency-targets.XXXXXX")
negative_dependency_order=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-negative-dependency-order.XXXXXX")
graph_probe=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-graph-probe.XXXXXX")
production_baseline=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-production-baseline.XXXXXX")
production_current=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-production-current.XXXXXX")
production_mutation=$(mktemp "${TMPDIR:-/tmp}/compatibility-main-production-mutation.XXXXXX")
baseline_root=$(mktemp -d "${TMPDIR:-/tmp}/compatibility-main-baseline.XXXXXX")
current_root=$(mktemp -d "${TMPDIR:-/tmp}/compatibility-main-current.XXXXXX")
trap 'rm -f -- "$paths" "$sorted_paths" "$baseline_candidates" "$current_candidates" "$negative_graph" "$negative_command_graph" "$negative_commands" "$negative_dependency_recipe" "$negative_dependency_commands" "$negative_dependency_targets" "$negative_dependency_order" "$graph_probe" "$production_baseline" "$production_current" "$production_mutation"; rm -rf -- "$baseline_root" "$current_root"' EXIT

allow_runtime_internal=runtime/mister_runtime_internal.hpp
allow_runtime_v2=runtime/mister_runtime_v2.cpp
allow_runtime_internal_sha=7ee1ca6244a478faeeffe07435823aea4d919a91cf9879936ce7e1f4fb3393a2
allow_runtime_v2_sha=59c012f18d8b5d075f79daf91d28e1502c69f215ff6ebefe41d9017628348711

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
	if [[ "$path" == "$allow_runtime_internal" ]]; then
		expected=$allow_runtime_internal_sha
	elif [[ "$path" == "$allow_runtime_v2" ]]; then
		expected=$allow_runtime_v2_sha
	fi
	[[ "$current_hash" == "$expected" ]] || {
		echo "compatibility production input changed: $path" >&2
		exit 1
	}
done <"$manifest"

for allowed in "$allow_runtime_internal" "$allow_runtime_v2"; do
	case "$allowed" in
		"$allow_runtime_internal") expected_allowed=$allow_runtime_internal_sha ;;
		"$allow_runtime_v2") expected_allowed=$allow_runtime_v2_sha ;;
		*) exit 1 ;;
	esac
	[[ -f "$root/$allowed" ]] || { echo "allowed runtime input is missing: $allowed" >&2; exit 1; }
	actual_allowed=$(shasum -a 256 "$root/$allowed" | awk '{print $1}')
	[[ "$actual_allowed" == "$expected_allowed" ]] || {
		echo "allowed runtime input changed: $allowed" >&2
		exit 1
	}
done

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
{ cat "$paths"; printf '%s\n' "$allow_runtime_internal" "$allow_runtime_v2"; } |
	LC_ALL=C sort -u >"$sorted_paths"
cmp "$sorted_paths" "$current_candidates"

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
cp "$root/$allow_runtime_internal" "$current_root/$allow_runtime_internal"
cp "$root/$allow_runtime_v2" "$current_root/$allow_runtime_v2"

(
	cd "$baseline_root"
	c++ -std=c++14 -U MISTER_RUNTIME_TESTING -U MISTER_NATIVE_PROFILE_TESTING \
		-I. -E -P runtime/mister_runtime_v2.cpp
) >"$production_baseline"
(
	cd "$current_root"
	c++ -std=c++14 -U MISTER_RUNTIME_TESTING -U MISTER_NATIVE_PROFILE_TESTING \
		-I. -E -P runtime/mister_runtime_v2.cpp
) >"$production_current"
cmp "$production_baseline" "$production_current"
cp "$current_root/$allow_runtime_v2" "$current_root/$allow_runtime_v2.saved"
sed 's/MISTER_RUNTIME_GENERATION_V2/MISTER_RUNTIME_GENERATION_V2_MUTATED/g' \
	"$current_root/$allow_runtime_v2.saved" >"$current_root/$allow_runtime_v2"
(
	cd "$current_root"
	c++ -std=c++14 -U MISTER_RUNTIME_TESTING -U MISTER_NATIVE_PROFILE_TESTING \
		-I. -E -P runtime/mister_runtime_v2.cpp
) >"$production_mutation"
if cmp -s "$production_baseline" "$production_mutation"; then
	echo "production-active mutation was not detected" >&2
	exit 1
fi
mv "$current_root/$allow_runtime_v2.saved" "$current_root/$allow_runtime_v2"

extract_graph() {
	local directory=$1
	printf '%s\n' '.PHONY: __compat_graph_probe' '__compat_graph_probe:' \
		$'\t@printf "%s\\n" "C_SRC $(C_SRC)" "CPP_SRC $(CPP_SRC)" "IMG $(IMG)" "RUNTIME_SRC $(RUNTIME_SRC)"' >"$graph_probe"
	(
		cd "$directory"
		make --no-print-directory -s -f Makefile -f "$graph_probe" VDATE=260812 DEP= __compat_graph_probe
	) | awk '
		$1 == "C_SRC" || $1 == "CPP_SRC" || $1 == "IMG" || $1 == "RUNTIME_SRC" {
			key=$1
			for (i=2; i<=NF; ++i) print key " " $i
		}
	' | LC_ALL=C sort -u
}

extract_commands() {
	local directory=$1
	(
		cd "$directory"
		make --no-print-directory -B -n -f Makefile V=1 VDATE=260812 DEP= bin/MiSTer 2>/dev/null
	) | LC_ALL=C sort -u
}

extract_dependency_targets() {
	local graph=$1
	awk '
		$1 == "C_SRC" || $1 == "CPP_SRC" || $1 == "RUNTIME_SRC" {
			path=$2
			sub(/^\.\//, "", path)
			if (path ~ /^\// || path ~ /(^|\/)\.\.($|\/)/) exit 2
			if ($1 == "C_SRC") sub(/\.c$/, ".c.d", path)
			else sub(/\.cpp$/, ".cpp.d", path)
			print "bin/" path
		}
	' "$graph" | LC_ALL=C sort -u
}

extract_dependency_contract() {
	local directory=$1
	awk '
		function anchor(name, pattern, line) {
			count[name]++
			if (count[name] > 1) exit 2
			print name " " line
		}
		$0 ~ /^BUILDDIR[[:space:]]*=/ { anchor("BUILDDIR", "", $0) }
		$0 ~ /^RUNTIME_DEP[[:space:]]*=/ { anchor("RUNTIME_DEP", "", $0) }
		$0 ~ /^DEP[[:space:]]*=/ {
			count["DEP"]++
			if (count["DEP"] > 1) exit 2
			in_dep=1
		}
		in_dep { print "DEP " $0; if ($0 !~ /\\[[:space:]]*$/) in_dep=0 }
		$0 == "ifneq ($(MAKECMDGOALS), clean)" {
			count["INCLUDE"]++
			if (count["INCLUDE"] > 1) exit 2
			in_include=1
		}
		in_include { print "INCLUDE " $0; if ($0 ~ /^endif/) in_include=0 }
		$0 ~ /^\$\(BUILDDIR\)\/%.c\.d:/ {
			count["C_RULE"]++
			if (count["C_RULE"] > 1) exit 2
			in_rule="C_RULE"
			print in_rule " " $0
			next
		}
		$0 ~ /^\$\(BUILDDIR\)\/%.cpp\.d:/ {
			count["CPP_RULE"]++
			if (count["CPP_RULE"] > 1) exit 2
			in_rule="CPP_RULE"
			print in_rule " " $0
			next
		}
		$0 ~ /^\$\(BUILDDIR\)\/runtime\/%.cpp\.d:/ {
			count["RUNTIME_RULE"]++
			if (count["RUNTIME_RULE"] > 1) exit 2
			in_rule="RUNTIME_RULE"
			print in_rule " " $0
			next
		}
		in_rule && $0 ~ /^[[:space:]]/ { print in_rule " " $0; next }
		in_rule { in_rule="" }
		END {
			for (name in count) if (count[name] != 1) exit 2
		}
	' "$directory/Makefile"
}

extract_graph "$baseline_root" >"$baseline_root/compatibility.graph"
extract_graph "$current_root" >"$current_root/compatibility.graph"
cmp "$baseline_root/compatibility.graph" "$current_root/compatibility.graph"

extract_dependency_targets "$baseline_root/compatibility.graph" >"$baseline_root/compatibility.dependencies"
extract_dependency_targets "$current_root/compatibility.graph" >"$current_root/compatibility.dependencies"
cmp "$baseline_root/compatibility.dependencies" "$current_root/compatibility.dependencies"

extract_dependency_contract "$baseline_root" >"$baseline_root/compatibility.dependency-contract"
extract_dependency_contract "$current_root" >"$current_root/compatibility.dependency-contract"
[[ -s "$baseline_root/compatibility.dependency-contract" && -s "$current_root/compatibility.dependency-contract" ]]
for anchor in \
	'BUILDDIR BUILDDIR = bin' \
	'RUNTIME_DEP RUNTIME_DEP = $(RUNTIME_SRC:%.cpp=$(BUILDDIR)/%.cpp.d)' \
	'INCLUDE ifneq ($(MAKECMDGOALS), clean)' \
	'INCLUDE -include $(DEP)' \
	'C_RULE $(BUILDDIR)/%.c.d: %.c' \
	'CPP_RULE $(BUILDDIR)/%.cpp.d: %.cpp' \
	'RUNTIME_RULE $(BUILDDIR)/runtime/%.cpp.d: runtime/%.cpp'; do
	for contract in "$baseline_root/compatibility.dependency-contract" "$current_root/compatibility.dependency-contract"; do
		count=$(grep -Fxc -- "$anchor" "$contract")
		[[ "$count" -eq 1 ]] || { echo "missing or duplicate dependency anchor: $anchor" >&2; exit 1; }
	done
done
for contract in "$baseline_root/compatibility.dependency-contract" "$current_root/compatibility.dependency-contract"; do
	count=$(grep -Ec '^DEP DEP[[:space:]]*=' "$contract")
	[[ "$count" -eq 1 ]] || { echo "missing or duplicate DEP anchor" >&2; exit 1; }
done
grep -F -- '-MM ' "$baseline_root/compatibility.dependency-contract" >/dev/null
grep -F -- '$(OUTPUT_FILTER)' "$baseline_root/compatibility.dependency-contract" >/dev/null
cmp "$baseline_root/compatibility.dependency-contract" "$current_root/compatibility.dependency-contract"

extract_commands "$baseline_root" >"$baseline_root/compatibility.commands"
extract_commands "$current_root" >"$current_root/compatibility.commands"
cmp "$baseline_root/compatibility.commands" "$current_root/compatibility.commands"

extract_commands() {
	local directory=$1
	shift
	(
		cd "$directory"
		make --no-print-directory -B -n -f Makefile V=1 VDATE=260812 DEP= "$@" 2>/dev/null
	) | LC_ALL=C sort -u
}

extract_dependency_commands() {
	local directory=$1
	local dependency_file=$2
	local target
	local targets=()
	while IFS= read -r target; do
		targets+=("$target")
	done <"$dependency_file"
	extract_commands "$directory" "${targets[@]}"
}

extract_dependency_commands "$baseline_root" "$baseline_root/compatibility.dependencies" >"$baseline_root/compatibility.dependency-commands"
extract_dependency_commands "$current_root" "$current_root/compatibility.dependencies" >"$current_root/compatibility.dependency-commands"
cmp "$baseline_root/compatibility.dependency-commands" "$current_root/compatibility.dependency-commands"

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

cp "$current_root/Makefile" "$current_root/Makefile.saved"
sed 's/-MM/-MMD/g' "$current_root/Makefile.saved" >"$current_root/Makefile"
extract_dependency_contract "$current_root" >"$negative_dependency_recipe"
extract_dependency_commands "$current_root" "$current_root/compatibility.dependencies" >"$negative_dependency_commands"
if cmp -s "$baseline_root/compatibility.dependency-contract" "$negative_dependency_recipe" ||
	cmp -s "$baseline_root/compatibility.dependency-commands" "$negative_dependency_commands"; then
	echo "compatibility dependency recipe mutation was not detected" >&2
	exit 1
fi
mv "$current_root/Makefile.saved" "$current_root/Makefile"

cp "$current_root/compatibility.dependencies" "$current_root/compatibility.dependencies.saved"
sed '$d' "$current_root/compatibility.dependencies.saved" >"$negative_dependency_targets"
if cmp -s "$baseline_root/compatibility.dependencies" "$negative_dependency_targets"; then
	echo "compatibility dependency target mutation was not detected" >&2
	exit 1
fi
mv "$current_root/compatibility.dependencies.saved" "$current_root/compatibility.dependencies"

cp "$current_root/Makefile" "$current_root/Makefile.saved"
awk '
	!swapped && index($0, "@mkdir -p $(dir $(BUILDDIR)/$*)") != 0 {
		saved=$0
		next
	}
	!swapped && saved != "" && index($0, "$(Q)$(info $< >> $@)") != 0 {
		print $0
		print saved
		swapped=1
		next
	}
	{ print }
	END { if (!swapped) exit 2 }
' "$current_root/Makefile.saved" >"$current_root/Makefile"
extract_dependency_contract "$current_root" >"$negative_dependency_order"
extract_dependency_commands "$current_root" "$current_root/compatibility.dependencies" >"$negative_dependency_commands"
if cmp -s "$baseline_root/compatibility.dependency-contract" "$negative_dependency_order" &&
	cmp -s "$baseline_root/compatibility.dependency-commands" "$negative_dependency_commands"; then
	echo "compatibility dependency contract order mutation was not detected" >&2
	exit 1
fi
mv "$current_root/Makefile.saved" "$current_root/Makefile"
