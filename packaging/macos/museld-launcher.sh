#!/bin/sh
#
# museld launcher.
#
# The Vulkan loader finds its driver through a manifest in a handful of fixed
# system locations, none of which a downloaded folder can write to. Point it at
# the MoltenVK manifest shipped alongside this script instead, unless the caller
# has already chosen a driver, and then run the real binary.

dir=$(cd "$(dirname "$0")" && pwd)
icd="$dir/vulkan/icd.d/MoltenVK_icd.json"

if [ -f "$icd" ]; then
    VK_DRIVER_FILES="${VK_DRIVER_FILES:-$icd}"
    export VK_DRIVER_FILES
    # Loaders older than 1.3.207 only know this spelling.
    VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$icd}"
    export VK_ICD_FILENAMES
fi

exec "$dir/museld-bin" "$@"
