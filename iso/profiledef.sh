#!/usr/bin/env bash
# shellcheck disable=SC2034

iso_name="win11"
iso_label="WIN11_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="win11wm <https://github.com/ggg610523-lab/Supernova>"
iso_application="win11wm Live System"
iso_version="$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y.%m.%d)"
install_dir="arch"
bootmodes=('bios.syslinux'
           'uefi.systemd-boot')
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'xz' '-Xbcj' 'x86' '-b' '1M' '-Xdict-size' '1M')
bootstrap_tarball_compression=('zstd' '-c' '-T0' '--auto-threads=logical' '--long' '-19')
file_permissions=(
  ["/etc/shadow"]="0:0:400"
  ["/etc/gshadow"]="0:0:400"
  ["/root"]="0:0:750"
  ["/root/.automated_script.sh"]="0:0:755"
  ["/root/.gnupg"]="0:0:700"
  ["/etc/sudoers.d/liveuser"]="0:0:440"
  ["/opt/win11wm/win11wm"]="0:0:755"
  ["/usr/local/bin/win11-session"]="0:0:755"
  ["/usr/local/bin/win11-files"]="0:0:755"
  ["/usr/local/bin/win11-terminal"]="0:0:755"
)
