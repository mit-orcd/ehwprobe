# ehwprobe

Standalone CPU topology probe ported from Slurm's `xcpuinfo.c`. Reproduces
what `slurmd -C` reports for a node — including the effects of
`l3cache_as_socket`, `numa_node_as_socket`, and `ignore_numa` — plus a
GPU/InfiniBand/NUMA locality report. No Slurm installation required.

## Build

Install the compiler and (optionally) hwloc headers:

```sh
# RHEL / Rocky / Alma
sudo dnf install gcc make hwloc-devel

# Debian / Ubuntu
sudo apt install gcc make libhwloc-dev
```

Then:

```sh
make            # build with hwloc support if available (like Slurm)
make nohwloc    # force the /proc/cpuinfo fallback parser
make clean
```

To build against a non-system hwloc (e.g. the one your Slurm was built with):

```sh
make HWLOC_CFLAGS=-I/opt/hwloc/include "HWLOC_LIBS=-L/opt/hwloc/lib -lhwloc"
# or
make PKG_CONFIG_PATH=/opt/hwloc/lib/pkgconfig
```

To statically embed hwloc (portable binary for nodes without hwloc):

```sh
make static HWLOC_CFLAGS=-I/opt/hwloc/include HWLOC_STATIC=/opt/hwloc/lib/libhwloc.a
```

## Run

```sh
./ehwprobe                              # same output as slurmd -C, plus locality report
./ehwprobe --parameters=l3cache_as_socket
./ehwprobe --map --verbose
./ehwprobe --quiet                      # only the slurmd -C lines
```

## `--parameters` / `-p`

Comma-separated, case-insensitive list of topology modifiers, mirroring the
corresponding Slurm configuration options. Unknown parameters are rejected;
the list is echoed in the output as `Parameters=` exactly like `slurmd -C`.

| Parameter | Slurm equivalent | Effect |
|---|---|---|
| `l3cache_as_socket` | `SlurmdParameters=l3cache_as_socket` | Count each hwloc L3 cache domain as a socket. Requires a hwloc **v2** build. |
| `numa_node_as_socket` | `SlurmdParameters=numa_node_as_socket` | Use the NUMA nodes' parent object as the socket. Requires a hwloc **v2** build. Mutually exclusive with `l3cache_as_socket`. |
| `ignore_numa` | `SchedulerParameters=Ignore_NUMA` | Report real sockets instead of treating each NUMA node as a socket. Effective only in hwloc **v1** builds (hwloc v2 ignores it; nohwloc builds are unaffected). |

Example:

```sh
./ehwprobe --parameters=l3cache_as_socket
./ehwprobe -p l3cache_as_socket,ignore_numa
```

## Example output

Collected via Slurm batch jobs on MIT Engaging (see `cpu_example.slurm` and
`gpu_example.slurm`). Long CPU lists are compacted to range notation here
(`0-126:2` = every 2nd CPU from 0 to 126).

### CPU node (`mit_quicktest`, node1600)

```
NodeName=node1600 CPUs=192 Boards=1 SocketsPerBoard=2 CoresPerSocket=48 ThreadsPerCore=2 RealMemory=385957
UpTime=25-10:44:40
```

### GPU node (`mit_normal_gpu`, node3402, 1× L40S allocated)

```
CUDA_VISIBLE_DEVICES=0
GPU 0: NVIDIA L40S (UUID: GPU-397589de-2032-ca80-1e35-d28f1c427f88)

NodeName=node3402 CPUs=128 Boards=1 SocketsPerBoard=2 CoresPerSocket=32 ThreadsPerCore=2 RealMemory=1031051
UpTime=29-15:50:14
------
NUMA nodes (memory locality):
  NUMA[0] cpus=0-126:2  mem=515019MB
  NUMA[1] cpus=1-127:2  mem=516032MB
GPUs:
  GPU[0] 10de:26b9 pci=0000:4a:00.0 numa=0 socket=- cpus=0-126:2
  GPU[1] 10de:26b9 pci=0000:61:00.0 numa=0 socket=- cpus=0-126:2
  GPU[2] 10de:26b9 pci=0000:ca:00.0 numa=1 socket=- cpus=1-127:2
  GPU[3] 10de:26b9 pci=0000:e1:00.0 numa=1 socket=- cpus=1-127:2
InfiniBand/RDMA adapters:
  IB[0] mlx5_0 pci=0000:a0:00.0 link=InfiniBand numa=1 socket=- cpus=1-127:2
```

Notes on this output:

- `10de:26b9` is the PCI vendor/device ID of the NVIDIA L40S.
- `socket=-` means the socket is unknown — this binary was a `nohwloc`
  (fallback) build; hwloc builds resolve the socket per the active socket
  definition.
- ehwprobe reports *all* hardware present on the node (4 GPUs above), while
  Slurm only granted the job one (`CUDA_VISIBLE_DEVICES=0`).

### Preemptable GPU nodes (`mit_preemptable`)

Same script, with the partition and GPU type overridden on the command line:

```sh
sbatch -p mit_preemptable --gres=gpu:a100:1 -J gpu_a100 -o gpu_a100_%j.out gpu_example.slurm
sbatch -p mit_preemptable --gres=gpu:h100:1 -J gpu_h100 -o gpu_h100_%j.out gpu_example.slurm
sbatch -p mit_preemptable --gres=gpu:h200:1 -J gpu_h200 -o gpu_h200_%j.out gpu_example.slurm
```

**A100 node (node1463)** — 4× A100-SXM4-80GB (`10de:20b2`); note the 8 NUMA
domains (AMD, NPS4-style) and that the GPUs sit on the odd domains 1/3/5/7:

```
NodeName=node1463 CPUs=128 Boards=1 SocketsPerBoard=2 CoresPerSocket=64 ThreadsPerCore=1 RealMemory=515047
------
NUMA nodes (memory locality):
  NUMA[0] cpus=0-15     mem=63557MB
  NUMA[1] cpus=16-31    mem=64506MB
  NUMA[2] cpus=32-47    mem=64506MB
  NUMA[3] cpus=48-63    mem=64494MB
  NUMA[4] cpus=64-79    mem=64506MB
  NUMA[5] cpus=80-95    mem=64506MB
  NUMA[6] cpus=96-111   mem=64506MB
  NUMA[7] cpus=112-127  mem=64461MB
GPUs:
  GPU[0] 10de:20b2 pci=0000:01:00.0 numa=3 socket=- cpus=48-63
  GPU[1] 10de:20b2 pci=0000:41:00.0 numa=1 socket=- cpus=16-31
  GPU[2] 10de:20b2 pci=0000:81:00.0 numa=7 socket=- cpus=112-127
  GPU[3] 10de:20b2 pci=0000:c1:00.0 numa=5 socket=- cpus=80-95
InfiniBand/RDMA adapters:
  IB[0] mlx5_0 pci=0000:63:00.0 link=Ethernet   numa=0 socket=- cpus=0-15
  IB[1] mlx5_1 pci=0000:63:00.1 link=Ethernet   numa=0 socket=- cpus=0-15
  IB[2] mlx5_2 pci=0000:a1:00.0 link=InfiniBand numa=6 socket=- cpus=96-111
```

**H100 node (node2901)** — 8× H100 80GB HBM3 (`10de:2330`), 4 GPUs per NUMA
node, and 9 InfiniBand adapters (rail-per-GPU HGX layout):

```
NodeName=node2901 CPUs=224 Boards=1 SocketsPerBoard=2 CoresPerSocket=56 ThreadsPerCore=2 RealMemory=2063205
------
NUMA nodes (memory locality):
  NUMA[0] cpus=0-222:2  mem=1031099MB
  NUMA[1] cpus=1-223:2  mem=1032106MB
GPUs:
  GPU[0] 10de:2330 pci=0000:19:00.0 numa=0 socket=- cpus=0-222:2
  GPU[1] 10de:2330 pci=0000:3b:00.0 numa=0 socket=- cpus=0-222:2
  GPU[2] 10de:2330 pci=0000:4c:00.0 numa=0 socket=- cpus=0-222:2
  GPU[3] 10de:2330 pci=0000:5d:00.0 numa=0 socket=- cpus=0-222:2
  GPU[4] 10de:2330 pci=0000:9b:00.0 numa=1 socket=- cpus=1-223:2
  GPU[5] 10de:2330 pci=0000:bb:00.0 numa=1 socket=- cpus=1-223:2
  GPU[6] 10de:2330 pci=0000:cb:00.0 numa=1 socket=- cpus=1-223:2
  GPU[7] 10de:2330 pci=0000:db:00.0 numa=1 socket=- cpus=1-223:2
InfiniBand/RDMA adapters:
  IB[0] mlx5_0 pci=0000:1a:00.0 link=InfiniBand numa=0 socket=- cpus=0-222:2
  IB[1] mlx5_1 pci=0000:3c:00.0 link=InfiniBand numa=0 socket=- cpus=0-222:2
  IB[2] mlx5_2 pci=0000:4d:00.0 link=InfiniBand numa=0 socket=- cpus=0-222:2
  IB[3] mlx5_3 pci=0000:5e:00.0 link=InfiniBand numa=0 socket=- cpus=0-222:2
  IB[4] mlx5_4 pci=0000:9c:00.0 link=InfiniBand numa=1 socket=- cpus=1-223:2
  IB[5] mlx5_5 pci=0000:9d:00.0 link=InfiniBand numa=1 socket=- cpus=1-223:2
  IB[6] mlx5_6 pci=0000:bc:00.0 link=InfiniBand numa=1 socket=- cpus=1-223:2
  IB[7] mlx5_7 pci=0000:cc:00.0 link=InfiniBand numa=1 socket=- cpus=1-223:2
  IB[8] mlx5_8 pci=0000:dc:00.0 link=InfiniBand numa=1 socket=- cpus=1-223:2
```

**H200 node (node5105)** — 2× H200 NVL (`10de:233b`), one GPU per NUMA node:

```
NodeName=node5105 CPUs=64 Boards=1 SocketsPerBoard=2 CoresPerSocket=16 ThreadsPerCore=2 RealMemory=772552
------
NUMA nodes (memory locality):
  NUMA[0] cpus=0-15,32-47   mem=386078MB
  NUMA[1] cpus=16-31,48-63  mem=386473MB
GPUs:
  GPU[0] 10de:233b pci=0000:89:00.0 numa=0 socket=- cpus=0-15,32-47
  GPU[1] 10de:233b pci=0001:c5:00.0 numa=1 socket=- cpus=16-31,48-63
InfiniBand/RDMA adapters:
  IB[0] mlx5_0 pci=0001:3f:00.0 link=InfiniBand numa=1 socket=- cpus=16-31,48-63
```

## License

GPLv2+ (same as Slurm, from which this was ported). See `COPYING`.
Original copyright notices are retained in the `ehwprobe.c` header.
