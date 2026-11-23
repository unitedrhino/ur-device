#!/usr/bin/env bash
# 在隔离项目中编译全部共享核心；只读挂载源码，不烧录、不访问硬件，也不修改原Watcher。
set -euo pipefail

# script_dir/core_root/repo_root定位源码；所有IDF生成物限制在当前worktree的.temp。
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
core_root="$(cd -- "$script_dir/.." && pwd)"
repo_root="$(cd -- "$core_root/../.." && pwd)"
# image固定已核实的release-v6.0 amd64清单，不随可变标签自动升级。
image='espressif/idf@sha256:0451470225f5f44bd0dbdb40861050b517f7473a1b2acf031fad848c853322ab'
if [[ "${1:-}" != '--guarded' && -n "${UR_RESOURCE_GUARD:-}" ]]; then
  exec bash "$UR_RESOURCE_GUARD" run --profile heavy -- bash "$script_dir/check_espidf.sh" --guarded
fi
if [[ $# -gt 1 || ( $# == 1 && "$1" != '--guarded' ) ]]; then
  echo '用法: bash firmware/device-runtime/tests/check_espidf.sh' >&2
  exit 2
fi
command -v docker >/dev/null
mkdir -p "$repo_root/.temp/device-runtime"
# 每次运行使用新目录，失败日志和编译中间产物也保留，避免删除或覆盖其他运行。
build_root="$(mktemp -d "$repo_root/.temp/device-runtime/espidf.XXXXXXXX")"
cp -a "$script_dir/espidf_smoke/." "$build_root/"
printf 'ESP-IDF输出目录: %s\n' "$build_root"
# cleanup只读取本次全新目录中Docker生成的精确容器ID，超时或中断也不能遗留编译容器。
cleanup() {
  local container_id=''
  if [[ -s "$build_root/container.cid" ]]; then
    read -r container_id < "$build_root/container.cid" || true
    if [[ "$container_id" =~ ^[0-9a-f]{64}$ ]]; then
      docker container rm --force "$container_id" >/dev/null 2>&1 || true
    fi
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# 不授予设备/特权；源码挂载只读。IDF组件管理器仅在临时项目内下载依赖。
timeout --signal=TERM --kill-after=15s 600 docker run --rm --platform linux/amd64 --cpus=2 \
  --cidfile "$build_root/container.cid" \
  --mount "type=bind,src=$repo_root,dst=/source,readonly" \
  --mount "type=bind,src=$build_root,dst=/project" \
  --workdir /project --env IDF_PY_BUILD_JOBS=2 \
  "$image" idf.py -DUR_DEVICE_ROOT=/source/firmware/device-runtime \
  -DIDF_TARGET=esp32s3 build
