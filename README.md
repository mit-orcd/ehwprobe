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
./xcpuinfo                              # same output as slurmd -C, plus locality report
./xcpuinfo --parameters=l3cache_as_socket
./xcpuinfo --map --verbose
./xcpuinfo --quiet                      # only the slurmd -C lines
```

## License

GPLv2+ (same as Slurm, from which this was ported).
