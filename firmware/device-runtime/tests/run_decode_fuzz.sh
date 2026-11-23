#!/usr/bin/env bash
# 文件说明：复用正式C++解码器进行有限模糊测试；种子只读，变异语料和崩溃产物留在本worktree的.temp。
set -euo pipefail
umask 077

# source_root/repo_root仅定位本任务源码；binary必须是本工作区已有的独立测试产物。
source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd -- "$source_root/../.." && pwd)"
if [[ $# != 1 || "$1" != /* ]]; then
  echo '用法: bash firmware/device-runtime/tests/run_decode_fuzz.sh <本worktree中模糊测试产物的绝对路径>' >&2
  exit 2
fi
binary="$(realpath -- "$1")"
if [[ ! -x "$binary" || "$binary" != "$repo_root/.temp/"* ]]; then
  echo '只允许当前worktree .temp内已构建的可执行测试产物' >&2
  exit 2
fi
# run_dir独立保留每次变异；不能把libFuzzer的可写第一语料目录指向Git源码。
mkdir -p "$repo_root/.temp/device-runtime"
run_dir="$(mktemp -d "$repo_root/.temp/device-runtime/decode-fuzz.XXXXXXXX")"
mkdir "$run_dir/corpus"
cp -a "$source_root/tests/corpus/." "$run_dir/corpus/"
printf '模糊测试产物目录: %s\n' "$run_dir"
exec "$binary" -runs=20000 -max_len=65537 -artifact_prefix="$run_dir/" "$run_dir/corpus"
