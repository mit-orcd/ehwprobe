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

## License

GPLv2+ (same as Slurm, from which this was ported). See `COPYING`.
Original copyright notices are retained in the `ehwprobe.c` header.
