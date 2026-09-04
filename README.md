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

## License

GPLv2+ (same as Slurm, from which this was ported). See `COPYING`.
Original copyright notices are retained in the `ehwprobe.c` header.
