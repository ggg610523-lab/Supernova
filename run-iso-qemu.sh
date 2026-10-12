#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

ISO=""
MODE="auto"
EXTRA=()

usage() {
    cat <<'EOF'
usage: ./run-iso-qemu.sh [--uefi|--bios] [image.iso] [-- qemu-args...]

  --uefi / --bios   force UEFI (systemd-boot) or legacy BIOS (syslinux)
  image.iso         path to a win11-*.iso (default: newest out/win11-*.iso)
  --                everything after is passed straight to qemu-system-x86_64

env: WIN11_QEMU_RAM (default 3G)  WIN11_QEMU_SMP (default 2)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uefi) MODE="uefi" ;;
        --bios) MODE="bios" ;;
        --help | -h) usage; exit 0 ;;
        --) shift; EXTRA+=("$@"); break ;;
        *.iso) ISO="$1" ;;
        *) usage >&2; exit 1 ;;
    esac
    shift
done

if [[ -z "$ISO" ]]; then
    ISO="$(ls -1t "$ROOT"/out/win11-*.iso 2>/dev/null | head -1 || true)"
fi
if [[ -z "$ISO" || ! -f "$ISO" ]]; then
    echo "error: no ISO found; pass a path or build one with ./build-iso.sh" >&2
    exit 1
fi

command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "error: qemu-system-x86_64 not installed" >&2
    exit 1
}

KVM=()
if [[ -w /dev/kvm ]]; then
    KVM=(-enable-kvm -cpu host)
else
    echo "note: /dev/kvm not usable, falling back to TCG (slow)" >&2
fi

CODE=""
VARS=""
for c in /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2/ovmf/OVMF_CODE.fd; do
    [[ -f "$c" ]] && { CODE="$c"; break; }
done
for v in /usr/share/edk2/x64/OVMF_VARS.4m.fd /usr/share/edk2/ovmf/OVMF_VARS.fd; do
    [[ -f "$v" ]] && { VARS="$v"; break; }
done

if [[ "$MODE" == "auto" ]]; then
    MODE="bios"
    [[ -n "$CODE" && -n "$VARS" ]] && MODE="uefi"
fi

RAM="${WIN11_QEMU_RAM:-3G}"
SMP="${WIN11_QEMU_SMP:-2}"

echo "booting $ISO ($MODE, ${RAM} RAM, ${SMP} cpu cores)..."

ARGS=(-name win11-iso -m "$RAM" -smp "$SMP" "${KVM[@]}" -no-reboot \
      -snapshot \
      -nic user,model=virtio-net-pci -usb -device usb-tablet)

case "$MODE" in
    uefi)
        VARS_COPY="/tmp/win11qemu-ovmf-vars.fd"
        cp -f "$VARS" "$VARS_COPY"
        ARGS+=(-machine q35 \
               -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
               -drive "if=pflash,format=raw,file=$VARS_COPY" \
               -drive "file=$ISO,format=raw")
        ;;
    bios)
        ARGS+=(-cdrom "$ISO" -boot order=d)
        ;;
esac

exec qemu-system-x86_64 "${ARGS[@]}" "${EXTRA[@]}"