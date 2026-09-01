/** @file

  HardwareInfoLib for QEMU sbsa-ref — fork of edk2-platforms'
  Silicon/Qemu/SbsaQemu/Library/SbsaQemuHardwareInfoLib, byte-for-byte
  identical except for GetCpuTopology() below.

  Why this fork exists (not a patch to the edk2-platforms submodule — see
  OrreryPkg.dec's header comment on why project-specific fixes live here
  instead): TF-A's SIP_SVC_GET_CPU_TOPOLOGY handler
  (trusted-firmware-a/plat/qemu/qemu_sbsa/sbsa_platform.c,
  read_cpu_topology_from_dt()) only has an answer if QEMU generated a
  "/cpus/topology" node in the guest device tree — which QEMU's sbsa-ref
  machine only does on a newer QEMU than the 8.2.2 available here (verified
  by dumping the DTB with `-machine dumpdtb=...` and inspecting it with
  `dtc`: no such node, with or without an explicit `-smp
  sockets=,clusters=,cores=,threads=`). Upstream's GetCpuTopology() treats
  that as a fatal error and calls ResetShutdown() — so on this QEMU/TF-A
  combination, stock edk2-platforms silently powers the machine back off
  moments after printing "SIP_SVC_GET_CPU_TOPOLOGY call failed", which
  reads exactly like a hang if you're not watching the serial log closely.

  Fix: fall back to a flat single-socket/single-cluster topology (Cores =
  GetCpuCount()) instead of shutting down. PPTT/SMBIOS type 4 end up
  reporting "N cores, 1 socket" rather than QEMU's actual -smp
  decomposition, which is cosmetically wrong but harmless for a dev/debug
  VM — nothing security- or boot-flow relevant depends on PPTT topology
  accuracy here. Delete this file (and the LibraryClass override in
  SbsaOrreryPkg.dsc) once running against a QEMU that populates
  /cpus/topology for sbsa-ref.

  Copyright (c) 2026, Orrery Project.
  Copyright (c) 2021, NUVIA Inc. All rights reserved.
  Copyright (c) 2024, Linaro Ltd. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/ArmMonitorLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/HardwareInfoLib.h>
#include <IndustryStandard/SbsaQemuSmc.h>

UINT32
GetCpuCount (
  VOID
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_GET_CPU_COUNT;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "%a: SIP_SVC_GET_CPU_COUNT call failed. We have no cpu information.\n", __func__));
    ResetShutdown ();
  }

  DEBUG ((DEBUG_INFO, "%a: We have %d cpus.\n", __func__, SmcArgs.Arg1));

  return SmcArgs.Arg1;
}

UINT64
GetMpidr (
  IN UINTN  CpuId
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_GET_CPU_NODE;
  SmcArgs.Arg1 = CpuId;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "%a: SIP_SVC_GET_CPU_NODE call failed. We have no MPIDR for CPU%d.\n", __func__, CpuId));
    ResetShutdown ();
  }

  DEBUG ((DEBUG_INFO, "%a: MPIDR for CPU%d: = %d\n", __func__, CpuId, SmcArgs.Arg2));

  return SmcArgs.Arg2;
}

UINT64
GetCpuNumaNode (
  IN UINTN  CpuId
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_GET_CPU_NODE;
  SmcArgs.Arg1 = CpuId;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "%a: SIP_SVC_GET_CPU_NODE call failed. Could not find information for CPU%d.\n", __func__, CpuId));
    return 0;
  }

  DEBUG ((DEBUG_INFO, "%a: NUMA node for CPU%d: = %d\n", __func__, CpuId, SmcArgs.Arg1));

  return SmcArgs.Arg1;
}

UINT32
GetMemNodeCount (
  VOID
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_GET_MEMORY_NODE_COUNT;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "%a: SIP_SVC_GET_MEMORY_NODE_COUNT call failed. We have no memory information.\n", __func__));
    ResetShutdown ();
  }

  DEBUG ((DEBUG_INFO, "%a: The number of the memory nodes is %ld\n", __func__, SmcArgs.Arg1));
  return (UINT32)SmcArgs.Arg1;
}

VOID
GetMemInfo (
  IN  UINTN       MemoryId,
  OUT MemoryInfo  *MemInfo
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_GET_MEMORY_NODE;
  SmcArgs.Arg1 = MemoryId;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "%a: SIP_SVC_GET_MEMORY_NODE call failed. We have no memory information.\n", __func__));
    ResetShutdown ();
  } else {
    MemInfo->NodeId      = SmcArgs.Arg1;
    MemInfo->AddressBase = SmcArgs.Arg2;
    MemInfo->AddressSize = SmcArgs.Arg3;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: NUMA node for System RAM:%d = 0x%lx - 0x%lx\n",
    __func__,
    MemInfo->NodeId,
    MemInfo->AddressBase,
    MemInfo->AddressBase + MemInfo->AddressSize -1
    ));
}

UINT64
GetNumaNodeCount (
  VOID
  )
{
  UINT64      Arg;
  UINT32      Index;
  UINT32      NumberNumaNodes;
  UINT32      NumberMemNodes;
  UINT32      NumCores = GetCpuCount ();
  MemoryInfo  MemInfo;

  NumberNumaNodes = 0;
  NumberMemNodes  = GetMemNodeCount ();

  if (NumCores > 0) {
    for (Index = 0; Index < NumCores; Index++) {
      Arg = GetCpuNumaNode (Index);
      if ((NumberNumaNodes == 0) || (NumberNumaNodes < (Arg + 1))) {
        NumberNumaNodes = Arg + 1;
      }
    }
  }

  if (NumberMemNodes > 0) {
    for (Index = 0; Index < NumberMemNodes; Index++) {
      GetMemInfo (Index, &MemInfo);
      if ((NumberNumaNodes == 0) || (NumberNumaNodes < (MemInfo.NodeId + 1))) {
        NumberNumaNodes = MemInfo.NodeId + 1;
      }
    }
  }

  return NumberNumaNodes;
}

/**
  Get CPU topology.

  Falls back to a flat 1-socket/1-cluster/N-core/1-thread topology instead
  of ResetShutdown() when TF-A reports the topology as unknown — see this
  file's header comment for why that happens on QEMU 8.2.2's sbsa-ref.
**/
VOID
GetCpuTopology (
  OUT CpuTopology  *CpuTopo
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_GET_CPU_TOPOLOGY;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    DEBUG ((
      DEBUG_WARN,
      "%a: SIP_SVC_GET_CPU_TOPOLOGY call failed (QEMU didn't publish "
      "/cpus/topology). Falling back to a flat 1-socket topology instead "
      "of ResetShutdown().\n",
      __func__
      ));
    CpuTopo->Sockets  = 1;
    CpuTopo->Clusters = 1;
    CpuTopo->Cores    = GetCpuCount ();
    CpuTopo->Threads  = 1;
  } else {
    CpuTopo->Sockets  = SmcArgs.Arg1;
    CpuTopo->Clusters = SmcArgs.Arg2;
    CpuTopo->Cores    = SmcArgs.Arg3;
    CpuTopo->Threads  = SmcArgs.Arg4;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: CPU Topology: sockets: %d, clusters: %d, cores: %d, threads: %d\n",
    __func__,
    CpuTopo->Sockets,
    CpuTopo->Clusters,
    CpuTopo->Cores,
    CpuTopo->Threads
    ));
}

VOID
GetGicInformation (
  OUT GicInfo  *GicInfo
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_GET_GIC;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    GicInfo->DistributorBase   = PcdGet64 (PcdGicDistributorBase);
    GicInfo->RedistributorBase = PcdGet64 (PcdGicRedistributorsBase);
  } else {
    GicInfo->DistributorBase   = SmcArgs.Arg1;
    GicInfo->RedistributorBase = SmcArgs.Arg2;
  }

  SmcArgs.Arg0 = SIP_SVC_GET_GIC_ITS;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    GicInfo->ItsBase = PcdGet64 (PcdGicItsBase);
  } else {
    GicInfo->ItsBase = SmcArgs.Arg1;
  }
}

VOID
GetPlatformVersion (
  OUT PlatformVersion  *PlatVer
  )
{
  ARM_MONITOR_ARGS  SmcArgs;

  SmcArgs.Arg0 = SIP_SVC_VERSION;
  ArmMonitorCall (&SmcArgs);

  if (SmcArgs.Arg0 != SMC_SIP_CALL_SUCCESS) {
    PlatVer->Major = 0;
    PlatVer->Minor = 0;
  } else {
    PlatVer->Major = SmcArgs.Arg1;
    PlatVer->Minor = SmcArgs.Arg2;
  }
}
