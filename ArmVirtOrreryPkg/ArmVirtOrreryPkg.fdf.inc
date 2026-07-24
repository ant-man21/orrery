## =============================================================================
## ArmVirtOrreryPkg.fdf — ArmVirt (QEMU virt, AArch64) flash description
##
## Mirrors Q35Pkg.fdf.inc's shape. NOTE: unlike Q35Pkg.fdf.inc — which is
## pulled into edk2/OvmfPkg/OvmfPkgX64.fdf via an `!include` already patched
## into that vendored file — edk2/ArmVirtPkg/ArmVirtQemuFvMain.fdf.inc's
## [FV.FvMain] has no equivalent hook yet. Until edk2 (the ant-man21/edk2
## fork, not this repo) gets a matching
##   !include ArmVirtOrreryPkg/ArmVirtOrreryPkg.fdf.inc
## added inside its [FV.FvMain] section, this file is not actually included
## by the build — HelloDxe/HelloDxe2 still compile (via ArmVirtOrreryPkg.dsc's
## [Components]) but are not baked into the arm-virt flash image.
## =============================================================================

INF OrreryPkg/Drivers/HelloDxe/HelloDxe.inf
INF OrreryPkg/Drivers/HelloDxe2/HelloDxe2.inf
