#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

ISO=""
MODE="auto"
GL="${WIN11_QEMU_GL:-auto}"
EXTRA=()

usage() {
    cat <<'EOF'
usage: ./run-iso-qemu.sh [--uefi|--bios] [--gl|--no-gl] [image.iso] [-- qemu-args...]

  --uefi / --bios   force UEFI (OVMF, GRUB) or legacy BIOS (syslinux)
  --gl / --no-gl    force / disable GPU acceleration (virtio-vga-gL + virgl).
                    default: auto (on when a /dev/dri/renderD* node exists)
  image.iso         path to a win11-*.iso (default: newest out/win11-*.iso)
  --                everything after is passed straight to qemu-system-x86_64

env: WIN11_QEMU_RAM (default 3G)  WIN11_QEMU_SMP (default 4)
     WIN11_QEMU_LOG (serial log, default /tmp/win11-qemu-serial.log)
     WIN11_QEMU_GL   auto|1|0   WIN11_QEMU_DISPLAY (default gtk)
     WIN11_QEMU_SERIAL  override serial backend (default file:$WIN11_QEMU_LOG)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uefi) MODE="uefi" ;;
        --bios) MODE="bios" ;;
        --gl) GL=1 ;;
        --no-gl) GL=0 ;;
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
SMP="${WIN11_QEMU_SMP:-4}"
SERIAL_LOG="${WIN11_QEMU_LOG:-/tmp/win11-qemu-serial.log}"
KVM=()
if [[ -w /dev/kvm ]]; then
    KVM=(-enable-kvm)
else
    echo "note: /dev/kvm not usable, falling back to TCG (slow)" >&2
fi

echo "booting $ISO ($MODE, ${RAM} RAM, ${SMP} cpu cores)..."

if [[ "$GL" == "auto" ]]; then
    GL=0
    compgen -G "/dev/dri/renderD*" >/dev/null 2>&1 && GL=1
fi

GPU=(-vga virtio)
DISPLAY_ARGS=()
if [[ "$GL" == "1" ]]; then
    GL_BACKEND="${WIN11_QEMU_DISPLAY:-gtk}"
    GPU=(-device virtio-vga-gl)
    DISPLAY_ARGS=(-display "${GL_BACKEND},gl=on")
    echo "GPU acceleration: ON (virtio-vga-gl + virgl, display ${GL_BACKEND},gl=on)"
else
    echo "GPU acceleration: off (software llvmpipe; pass --gl to enable)"
fi

ARGS=(-name win11-iso -m "$RAM" -smp "$SMP" "${KVM[@]}" -no-reboot \
      -snapshot \
      -cpu max "${GPU[@]}" "${DISPLAY_ARGS[@]}" \
      -nic user,model=virtio-net-pci \
      -usb -device usb-tablet \
      -serial "${WIN11_QEMU_SERIAL:-file:$SERIAL_LOG}")

case "$MODE" in
    uefi)
        command -v xorriso >/dev/null 2>&1 || {
            echo "error: xorriso required for --uefi (bundle archiso to install it)" >&2
            exit 1
        }
        KDIR="$(mktemp -d /tmp/win11qemu-kern.XXXXXX)"
        trap 'rm -rf "$KDIR"' EXIT
        if ! xorriso -osirrox on -indev "$ISO" -extract /arch/boot/x86_64 "$KDIR" >/dev/null 2>&1; then
            echo "error: could not extract kernel/initramfs from $ISO" >&2
            exit 1
        fi
        # GRUB-based ISO: pull the archisosearchuuid from the GRUB config
        xorriso -osirrox on -indev "$ISO" -extract /boot/grub/grub.cfg "$KDIR/grub.cfg" >/dev/null 2>&1 || true
        VMLINUZ="$(ls "$KDIR"/vmlinuz-* 2>/dev/null | head -1 || true)"
        INITRD="$(ls "$KDIR"/initramfs-*.img 2>/dev/null | head -1 || true)"
        if [[ -z "$VMLINUZ" || -z "$INITRD" ]]; then
            echo "error: kernel/initramfs not found on ISO" >&2
            exit 1
        fi
        UUID="$(sed -n 's/.*archisosearchuuid=\([^ ]*\).*/\1/p' "$KDIR/grub.cfg" 2>/dev/null | head -1 || true)"
        if [[ -z "$UUID" ]]; then
            echo "warning: no archisosearchuuid in GRUB config; ISO may not be found at runtime" >&2
        fi
        VARS_COPY="/tmp/win11qemu-ovmf-vars.fd"
        cp -f "$VARS" "$VARS_COPY"
        ARGS+=(-machine "type=q35,smm=on,usb=on" \
               -global ICH9-LPC.disable_s3=1 \
               -global "driver=cfi.pflash01,property=secure,value=off" \
               -kernel "$VMLINUZ" \
               -initrd "$INITRD" \
               -append "archisobasedir=arch archisosearchuuid=$UUID console=ttyS0" \
               -drive "file=$ISO,format=raw" \
               -drive "if=pflash,format=raw,unit=0,file=$CODE,read-only=on" \
               -drive "if=pflash,format=raw,unit=1,file=$VARS_COPY")
        ;;
    bios)
        ARGS+=(-machine "type=pc,usb=on" \
               -drive "file=$ISO,format=raw,media=cdrom,read-only=on" \
               -boot "order=d,menu=on,reboot-timeout=5000")
        ;;
esac

exec qemu-system-x86_64 "${ARGS[@]}" "${EXTRA[@]}"