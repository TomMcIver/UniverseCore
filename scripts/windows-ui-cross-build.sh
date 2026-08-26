#!/usr/bin/env bash
set -euo pipefail

workspace_root="${1:-/workspace}"
build_root="${2:-/build-ui}"
output_dir="${workspace_root}/build/windows-ui/UniverseCore"

cmake -S "${workspace_root}" -B "${build_root}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIVERSECORE_BUILD_UI=ON \
  -DUNIVERSECORE_BUILD_TESTS=OFF \
  -DCMAKE_TOOLCHAIN_FILE="${workspace_root}/cmake/toolchains/mingw-w64-x86_64.cmake"

cmake --build "${build_root}" --target universe_ui

rm -rf "${output_dir}"
mkdir -p "${output_dir}"
cp "${build_root}/universe_ui.exe" "${output_dir}/universe_ui.exe"

sdl_dll="$(find "${build_root}" -type f -name SDL3.dll | head -n 1 || true)"
if [[ -z "${sdl_dll}" ]]; then
  echo "SDL3.dll not found in cross-build output" >&2
  exit 2
fi
cp "${sdl_dll}" "${output_dir}/SDL3.dll"

x86_64-w64-mingw32-objdump -p "${build_root}/universe_ui.exe" | sed -n 's/^\tDLL Name: //p' | while read -r dll; do
  [[ -z "${dll}" ]] && continue

  case "${dll}" in
    KERNEL32.dll|USER32.dll|GDI32.dll|ADVAPI32.dll|SHELL32.dll|OLE32.dll|OLEAUT32.dll|COMDLG32.dll|WINMM.dll|IMM32.dll|SETUPAPI.dll|VERSION.dll|WS2_32.dll|IPHLPAPI.dll|UCRTBASE.dll|MSVCRT.dll|SHLWAPI.dll|RPCRT4.dll|CRYPT32.dll|WINTRUST.dll|WTSAPI32.dll|DINPUT8.dll|DXGI.dll|D3D11.dll|D3D12.dll|DXGUID.dll|SDL3.dll)
      continue
      ;;
  esac

  dll_path="$(find "${build_root}" /usr -type f -name "${dll}" 2>/dev/null | head -n 1 || true)"
  if [[ -n "${dll_path}" ]]; then
    cp "${dll_path}" "${output_dir}/${dll}"
  fi
done

echo "WINDOWS_UI_OUTPUT|${output_dir}/universe_ui.exe"
