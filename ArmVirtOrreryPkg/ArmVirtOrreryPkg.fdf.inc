## =============================================================================
## ArmVirtOrreryPkg.fdf — ArmVirt (QEMU virt, AArch64) flash description
##
## Mirrors Q35Pkg.fdf.inc's shape and role. Pulled into this platform's FV
## the same way Q35Pkg.fdf.inc is pulled into edk2/OvmfPkg/OvmfPkgX64.fdf —
## via a matching include line patched into
## edk2/ArmVirtPkg/ArmVirtQemuFvMain.fdf.inc's [FV.FvMain] section, in the
## ant-man21/edk2 fork (not this repo).
##
## NOTE for whoever edits this comment block: don't spell out that include
## directive literally in here (not even inside a comment) — GenFds's FDF
## preprocessor scans for the include-directive text everywhere, comments
## included, and will report a false "include loop" against this very file.
## =============================================================================

INF OrreryPkg/Drivers/HelloDxe/HelloDxe.inf
INF OrreryPkg/Drivers/HelloDxe2/HelloDxe2.inf
