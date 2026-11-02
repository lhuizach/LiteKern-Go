# LiteKern Go - creates the VirtualBox VM, sized like the Galaxy A5 (SM-A500FU).
#
#   powershell -ExecutionPolicy Bypass -File vm\create-vm.ps1
#
# Phone -> VM:
#   Snapdragon 410, 4x Cortex-A53  -> 4 x86_64 CPUs (not the same speed, see below)
#   2 GB RAM                       -> 2048 MB
#   16 GB storage                  -> 16 GB disk (dynamic, grows as used)
#   720x1280 portrait screen       -> VMSVGA (Linux vmwgfx DRM driver), 720x1280 hint
#   touchscreen                    -> USB tablet (absolute pointer, a click is a tap)
#   Wi-Fi                          -> NAT network card
#
# What a VM can't match: the real CPU speed, the Adreno GPU, the touch
# controller, Wi-Fi chip, battery and the phone's boot process (lk2nd).
# An x86_64 core is much faster than an A53, so speed checks belong on the phone.
# -CpuCap (1-100) limits the VM's CPU time if you want it to feel slower.

param(
    [string]$Name = "LiteKern Go",
    [int]$CpuCap = 100
)

$ErrorActionPreference = "Stop"
$vbox = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"

if ((& $vbox list vms) -match "^`"$([regex]::Escape($Name))`" ") { throw "A VM called '$Name' already exists." }

$machineFolder = ((& $vbox list systemproperties | Select-String "Default machine folder:").ToString() -split ":\s+", 2)[1].Trim()
$disk = Join-Path $machineFolder "$Name\$Name.vdi"

function Run { & $vbox @args; if ($LASTEXITCODE -ne 0) { throw "VBoxManage $args failed" } }

Run createvm --name $Name --ostype Linux_64 --register

# CPU, memory, firmware
Run modifyvm $Name --cpus 4 --cpuexecutioncap $CpuCap --memory 2048 `
    --firmware bios --ioapic on --pae on --longmode on --rtcuseutc on `
    --boot1 disk --boot2 dvd --boot3 none --boot4 none

# Screen: 720x1280 portrait
Run modifyvm $Name --graphicscontroller vmsvga --vram 32 --accelerate3d off
Run setextradata $Name "CustomVideoMode1" "720x1280x32"
Run setextradata $Name "GUI/LastGuestSizeHint" "720,1280"

# Input: the mouse acts as the touchscreen
Run modifyvm $Name --usbohci on --mouse usbtablet --keyboard ps2

# Network (stands in for Wi-Fi), no audio for now
Run modifyvm $Name --nic1 nat --nictype1 82540EM --audio-enabled off

# Storage: port 0 is the system image (linux/vm.sh vbox start puts it there),
# port 1 an empty optical drive, port 2 a 16 GB disk like the phone's storage.
Run createmedium disk --filename $disk --size 16384 --format VDI --variant Standard
Run storagectl $Name --name SATA --add sata --controller IntelAhci --portcount 3 --bootable on
Run storageattach $Name --storagectl SATA --port 1 --device 0 --type dvddrive --medium emptydrive
Run storageattach $Name --storagectl SATA --port 2 --device 0 --type hdd --medium $disk

# The boot log: COM1 to build-linux\vbox\serial.log (linux/vm.sh vbox test reads it)
$serial = Join-Path (Split-Path $PSScriptRoot -Parent) "build-linux\vbox\serial.log"
New-Item -ItemType Directory -Force (Split-Path $serial) | Out-Null
Run modifyvm $Name --uart1 0x3F8 4 --uart-mode1 file $serial

Write-Host "Created '$Name'. Boot it with: wsl bash linux/vm.sh vbox start"
