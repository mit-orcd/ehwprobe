/*****************************************************************************\
 * xcpuinfo.c - standalone CPU topology probe ported from Slurm
 *
 * Independent reimplementation of the topology detection logic in Slurm's
 * src/slurmd/common/xcpuinfo.c (verified identical between master and the
 * slurm-26.05 branch), with all Slurm dependencies removed. It reproduces
 * what "slurmd -C" reports for a node, including the effect of:
 *
 *   --parameters=l3cache_as_socket    (SlurmdParameters=l3cache_as_socket)
 *   --parameters=numa_node_as_socket  (SlurmdParameters=numa_node_as_socket)
 *   --parameters=ignore_numa          (SchedulerParameters=Ignore_NUMA,
 *                                      hwloc v1 builds only)
 *
 * Build:
 *   make                # uses hwloc if pkg-config/headers find it
 *   make nohwloc        # force the /proc/cpuinfo fallback parser
 *
 * Run:
 *   ./xcpuinfo                              # same output as: slurmd -C,
 *                                           # plus GPU/IB/NUMA locality report
 *   ./xcpuinfo --parameters=l3cache_as_socket
 *   ./xcpuinfo --map --verbose
 *   ./xcpuinfo --quiet                      # only the slurmd -C lines
 *
 * Beyond slurmd -C, the tool reports (when present):
 *   - NUMA nodes with their CPU sets and local memory (memory locality)
 *   - GPUs with PCI address, NUMA node, "socket" (per the active socket
 *     definition, i.e. L3 domain under l3cache_as_socket) and CPU affinity
 *   - InfiniBand adapters (OpenFabrics devices, e.g. mlx5_0) with the same
 *     locality/affinity data
 * Device locality comes from hwloc in hwloc builds and from /sys in the
 * nohwloc fallback build.
 *
 * Ported function by function from Slurm:
 *   xcpuinfo_hwloc_topo_load()  -> hwloc_topo_load()
 *   _check_full_access()        -> check_full_access()
 *   _remove_ecores()            -> remove_ecores()
 *   _core_child_count()         -> core_child_count()
 *   xcpuinfo_hwloc_topo_get()   -> topo_get()            [hwloc build]
 *   /proc/cpuinfo fallback      -> topo_get()            [nohwloc build]
 *   _compute_block_map() etc.   -> compute_block_map()   [nohwloc build]
 *   get_procs()                 -> get_procs()
 *   get_memory()/get_up_time()  -> get_memory()/get_up_time()
 *     (from src/slurmd/slurmd/get_mach_stat.c)
 *   _print_config() output      -> main()                [slurmd -C format]
 *
 * Intentional deviations from Slurm are marked with "NOTE:".
 * Not ported (not needed for topology reporting): CpuSpecList handling
 * (xcpuinfo_abs_to_mac / xcpuinfo_mac_to_abs / xcpuinfo_get_cpuspec) and
 * GRES GPU autodetection.
 *
 * Slurm is free software (GPLv2+); this port keeps that license.
\*****************************************************************************/

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#ifdef HAVE_HWLOC
#include <hwloc.h>
#include <hwloc/helper.h>
#endif

#define MAX_CPUSET_STR 2048
#define MAX_SOCKET_INX 1024	/* Slurm's _MAX_SOCKET_INX */

/* Command line / "configuration" state (replaces slurm_conf lookups) */
static bool opt_l3cache_as_socket  = false;	/* CONF_FLAG_L3CSOCK  */
static bool opt_numa_node_as_socket = false;	/* CONF_FLAG_NNSOCK   */
static bool opt_ignore_numa        = false;	/* Ignore_NUMA        */
static bool opt_map                = false;
static bool opt_verbose            = false;
static bool opt_quiet              = false;	/* only slurmd -C lines */
static char *opt_parameters        = NULL;	/* echoed like slurmd -C */

/*
 * Device/locality report (GPUs, InfiniBand, NUMA memory).
 * Filled from hwloc when available, from /sys otherwise.
 */
#define MAX_REPORT_DEVS 64
#define MAX_REPORT_NUMA 256

typedef struct {
	char name[64];		/* GPU: model or vendor:device; IB: mlx5_0 */
	char bdf[16];		/* PCI bus id 0000:31:00.0 ("" if unknown) */
	int  numa;		/* NUMA node os_index, -1 if none/unknown */
	char socket[64];	/* range list of "socket" logical indexes per
				 * the active socket definition (L3 domains
				 * when l3cache_as_socket) contained in the
				 * device's locality cpuset; "" = unresolved */
	char link[16];		/* IB/RDMA link layer: InfiniBand/Ethernet */
	char cpus[512];		/* CPU affinity list, e.g. 0-47,96-143 */
} dev_report_t;

typedef struct {
	int      os_index;
	char     cpus[512];
	uint64_t mem_mb;
} numa_report_t;

static dev_report_t  gpu_report[MAX_REPORT_DEVS];
static int           gpu_report_cnt = 0;
static dev_report_t  ib_report[MAX_REPORT_DEVS];
static int           ib_report_cnt = 0;
static numa_report_t numa_report[MAX_REPORT_NUMA];
static int           numa_report_cnt = 0;

/* defined in the shared /sys section below; called from both builds */
static void collect_devices_sys(void);
static int read_first_line(const char *path, char *buf, size_t len);

#define info(...)    fprintf(stderr, __VA_ARGS__)
#define warning(...) fprintf(stderr, "warning: " __VA_ARGS__)
#define error(...)   fprintf(stderr, "error: "   __VA_ARGS__)
#define fatal(...)  do { fprintf(stderr, "fatal: " __VA_ARGS__); \
			 fputc('\n', stderr); exit(1); } while (0)
#define debug(...)  do { if (opt_verbose) \
			 fprintf(stderr, "debug: " __VA_ARGS__); \
			 } while (0)

#ifndef HAVE_HWLOC
/*
 * get_procs - Return the count of procs on this system
 * (verbatim logic from Slurm's get_procs(); only used by the
 * /proc/cpuinfo fallback path)
 */
static int get_procs(uint16_t *procs)
{
#ifdef _SC_NPROCESSORS_ONLN
	int my_proc_tally;

	*procs = 1;
	my_proc_tally = (int) sysconf(_SC_NPROCESSORS_ONLN);
	if (my_proc_tally < 1) {
		error("get_procs: error running sysconf(_SC_NPROCESSORS_ONLN)\n");
		return EINVAL;
	}

	*procs = (uint16_t) my_proc_tally;
#else
	*procs = 1;
#endif

	return 0;
}
#endif /* !HAVE_HWLOC */

/*
 * get_memory - Return the physical memory size in MiB
 * (verbatim logic from get_mach_stat.c:get_memory())
 */
static int get_memory(uint64_t *real_memory)
{
#ifdef _SC_PHYS_PAGES
	long pages;

	*real_memory = 1;
	pages = sysconf(_SC_PHYS_PAGES);
	if (pages < 1) {
		error("get_memory: error running sysconf(_SC_PHYS_PAGES)\n");
		return EINVAL;
	}
	*real_memory = (uint64_t)((float)pages * (sysconf(_SC_PAGE_SIZE) /
			1048576.0)); /* Megabytes of memory */
#else
	*real_memory = 1;
#endif
	return 0;
}

/*
 * get_up_time - Return system uptime in seconds
 * (verbatim logic from get_mach_stat.c:get_up_time(), Linux path)
 */
static int get_up_time(uint32_t *up_time)
{
	struct sysinfo info;

	if (sysinfo(&info) < 0) {
		*up_time = 0;
		return errno;
	}
	*up_time = info.uptime;
	return 0;
}

#ifdef HAVE_HWLOC

/* Return the number of cores which are descendants of the given object */
static int core_child_count(hwloc_topology_t topology, hwloc_obj_t obj)
{
	int count = 0;
	unsigned i;

	if (obj->type == HWLOC_OBJ_CORE)
		return 1;

	for (i = 0; i < obj->arity; i++)
		count += core_child_count(topology, obj->children[i]);
	return count;
}

/*
 * Slurm runs this before remove_ecores() as hwloc_topology_restrict()
 * changes the view. Reports CPUs outside the allowed cpuset (what Slurm
 * would turn into CpuSpecList).
 */
static void check_full_access(hwloc_topology_t *topology)
{
	hwloc_const_bitmap_t complete, allowed;
	hwloc_bitmap_t restricted_cpus_mask;
	char restricted_cpus_as_mac[MAX_CPUSET_STR];

	complete = hwloc_topology_get_complete_cpuset(*topology);
	allowed = hwloc_topology_get_allowed_cpuset(*topology);

	if (!hwloc_bitmap_isequal(complete, allowed)) {
		restricted_cpus_mask = hwloc_bitmap_alloc();
		hwloc_bitmap_andnot(restricted_cpus_mask, complete, allowed);
		hwloc_bitmap_list_snprintf(restricted_cpus_as_mac,
					   MAX_CPUSET_STR,
					   restricted_cpus_mask);
		hwloc_bitmap_free(restricted_cpus_mask);
		warning("subset of restricted cpus (not available for jobs): %s\n",
			restricted_cpus_as_mac);
	} else {
		debug("got full access to the cpuset topology\n");
	}
}

#if HWLOC_API_VERSION > 0x00020401
/* Simple string set for P-core frequencies (replaces Slurm's list_t) */
#define MAX_PCORE_FREQS 64
static char pcore_freqs[MAX_PCORE_FREQS][32];
static int  n_pcore_freqs = 0;

static bool freq_seen(const char *freq)
{
	for (int i = 0; i < n_pcore_freqs; i++)
		if (!strcmp(pcore_freqs[i], freq))
			return true;
	return false;
}
#endif

/*
 * Restrict the topology to Intel P-cores on hybrid (Alder Lake+) CPUs.
 * No-op on any other processor (including all AMD EPYC).
 * Two-pass approach ported verbatim from Slurm's _remove_ecores().
 */
static void remove_ecores(hwloc_topology_t *topology)
{
#if HWLOC_API_VERSION > 0x00020401
	int type_cnt;
	hwloc_bitmap_t cpuset, cpuset_tot;
	bool found = false;

	/* NOTE: body requires hwloc >= 2.5 (cpukinds); empty on hwloc v1,
	 * same as Slurm's own build */

	if (!(type_cnt = hwloc_cpukinds_get_nr(*topology, 0)))
		return;

	cpuset = hwloc_bitmap_alloc();
	cpuset_tot = hwloc_bitmap_alloc();

	/*
	 * First pass: Find all CPU Kinds with CoreType=IntelCore and collect
	 * their frequencies. This identifies all properly-labeled P-cores,
	 * which may have varying frequencies on newer processors.
	 */
	for (int i = 0; i < type_cnt; i++) {
		unsigned nr_infos = 0;
		struct hwloc_info_s *infos;
		bool is_pcore = false;
		char *freq = NULL;

		if (hwloc_cpukinds_get_info(
			    *topology, i, cpuset, NULL, &nr_infos, &infos, 0))
			fatal("Error getting info from hwloc_cpukinds_get_info()");

		/* Look for the CPU Kinds entry with CoreType=IntelCore. */
		for (unsigned j = 0; j < nr_infos; j++) {
			if (!strcasecmp(infos[j].name, "CoreType") &&
			    !strcasecmp(infos[j].value, "IntelCore")) {
				is_pcore = true;
			} else if (!strcasecmp(infos[j].name,
					       "FrequencyMaxMHz")) {
				freq = infos[j].value;
			}
		}

		if (is_pcore) {
			hwloc_bitmap_or(cpuset_tot, cpuset_tot, cpuset);
			found = true;

			/* Collect all distinct P-core frequencies. */
			if (freq && !freq_seen(freq) &&
			    n_pcore_freqs < MAX_PCORE_FREQS) {
				snprintf(pcore_freqs[n_pcore_freqs], 32,
					 "%s", freq);
				n_pcore_freqs++;
			}
		}
	}

	if (!found)
		goto cleanup;

	/*
	 * Second pass: Include any CPU Kinds that match collected P-core
	 * frequencies, even without CoreType=IntelCore. Recovers cpuset-
	 * restricted P-cores that lack CoreType on hwloc < 2.10.
	 */
	for (int i = 0; i < type_cnt; i++) {
		unsigned nr_infos = 0;
		struct hwloc_info_s *infos;

		if (hwloc_cpukinds_get_info(
			    *topology, i, cpuset, NULL, &nr_infos, &infos, 0))
			fatal("Error getting info from hwloc_cpukinds_get_info()");

		for (unsigned j = 0; j < nr_infos; j++) {
			if (!strcasecmp(infos[j].name, "FrequencyMaxMHz")) {
				if (freq_seen(infos[j].value))
					hwloc_bitmap_or(cpuset_tot, cpuset_tot,
							cpuset);
				break;
			}
		}
	}

	hwloc_topology_restrict(*topology, cpuset_tot, 0);
	debug("restricted topology to Intel P-cores\n");

cleanup:
	hwloc_bitmap_free(cpuset);
	hwloc_bitmap_free(cpuset_tot);
#else
	(void) topology;	/* no E-core handling on hwloc < 2.5 */
#endif
}

/* read or load topology; init and destroy topology must be outside */
static int hwloc_topo_load(hwloc_topology_t *topology)
{
	/* parse all system */
	hwloc_topology_set_flags(*topology, HWLOC_TOPOLOGY_FLAG_WHOLE_SYSTEM);

	/* ignores cache, misc */
#if HWLOC_API_VERSION < 0x00020000
	hwloc_topology_ignore_type(*topology, HWLOC_OBJ_CACHE);
	hwloc_topology_ignore_type(*topology, HWLOC_OBJ_MISC);
#else
	hwloc_topology_set_type_filter(*topology, HWLOC_OBJ_L1CACHE,
				       HWLOC_TYPE_FILTER_KEEP_NONE);
	hwloc_topology_set_type_filter(*topology, HWLOC_OBJ_L2CACHE,
				       HWLOC_TYPE_FILTER_KEEP_NONE);
	/* need to preserve HWLOC_OBJ_L3CACHE for l3cache_as_socket */
	hwloc_topology_set_type_filter(*topology, HWLOC_OBJ_L4CACHE,
				       HWLOC_TYPE_FILTER_KEEP_NONE);
	hwloc_topology_set_type_filter(*topology, HWLOC_OBJ_L5CACHE,
				       HWLOC_TYPE_FILTER_KEEP_NONE);
	hwloc_topology_set_type_filter(*topology, HWLOC_OBJ_MISC,
				       HWLOC_TYPE_FILTER_KEEP_NONE);
#endif

	/* load topology */
	if (hwloc_topology_load(*topology)) {
		debug("hwloc_topology_load() failed.\n");
		return -1;
	}

	check_full_access(topology);
	remove_ecores(topology);

	return 0;
}

/* compress a sorted index list into cpulist form, e.g. "0-3,8-9" */
static void idx_list_snprintf(char *buf, size_t len, int *idx, int n)
{
	size_t off = 0;
	int start, prev, i;

	buf[0] = '\0';
	if (n <= 0)
		return;
	start = prev = idx[0];
	for (i = 1; i <= n; i++) {
		if ((i < n) && (idx[i] == prev + 1)) {
			prev = idx[i];
			continue;
		}
		if (off < len)
			off += snprintf(buf + off, len - off,
					(start == prev) ? "%s%d" : "%s%d-%d",
					off ? "," : "", start, prev);
		if (i < n)
			start = prev = idx[i];
	}
}

/*
 * Range-compressed list of socket_type objects whose cpuset is fully
 * contained in a device's locality cpuset. With the default socket view
 * this yields a single index for socket-local devices; with
 * l3cache_as_socket it yields the L3 domain(s) the device is local to
 * (PCI devices attach above the L3 level, so a plain ancestor walk
 * would never find one).
 */
static void socket_set_snprintf(hwloc_topology_t topology,
				hwloc_obj_type_t socket_type,
				hwloc_const_bitmap_t cset,
				char *buf, size_t len)
{
	int idx[MAX_SOCKET_INX];
	int n = 0, count, i;

	buf[0] = '\0';
	if (!cset)
		return;
	count = hwloc_get_nbobjs_by_type(topology, socket_type);
	for (i = 0; (i < count) && (n < MAX_SOCKET_INX); i++) {
		hwloc_obj_t s = hwloc_get_obj_by_type(topology, socket_type, i);

		if (s && s->cpuset && hwloc_bitmap_isincluded(s->cpuset, cset))
			idx[n++] = s->logical_index;
	}
	idx_list_snprintf(buf, len, idx, n);
}

/*
 * Fill numa/socket/cpus affinity for a PCI or OS device by walking its
 * ancestors. "socket" is computed against the active socket definition
 * (HWLOC_OBJ_L3CACHE when l3cache_as_socket), so the report uses the same
 * topology view as the CPU detection above.
 */
static void device_affinity(hwloc_topology_t topology, hwloc_obj_t obj,
			    hwloc_obj_type_t socket_type,
			    int *numa, char *socket, size_t sock_len,
			    char *cpus, size_t len)
{
	hwloc_obj_t anc;

	*numa = -1;
	socket[0] = '\0';
	cpus[0] = '\0';

	for (anc = obj->parent; anc; anc = anc->parent) {
		if ((*numa < 0) && (anc->type == HWLOC_OBJ_NODE))
			*numa = anc->os_index;
	}

	/* NOTE: hwloc_get_non_io_ancestor_cpuset() is hwloc v2 only;
	 * hwloc_get_non_io_ancestor_obj() exists in both v1 and v2 */
	anc = hwloc_get_non_io_ancestor_obj(topology, obj);
	if (anc && anc->cpuset) {
		hwloc_bitmap_list_snprintf(cpus, len, anc->cpuset);
		socket_set_snprintf(topology, socket_type, anc->cpuset,
				    socket, sock_len);
	}
}

/* Collect NUMA memory locality, GPUs and InfiniBand adapters via hwloc */
static void collect_devices_hwloc(hwloc_topology_t topology,
				  hwloc_obj_type_t socket_type)
{
	hwloc_obj_t obj = NULL;

	/* NUMA nodes: cpuset + local memory */
	while ((obj = hwloc_get_next_obj_by_type(topology, HWLOC_OBJ_NODE,
						 obj))) {
		numa_report_t *n;
		if (numa_report_cnt >= MAX_REPORT_NUMA)
			break;
		n = &numa_report[numa_report_cnt++];
		n->os_index = obj->os_index;
		if (obj->cpuset)
			hwloc_bitmap_list_snprintf(n->cpus, sizeof(n->cpus),
						   obj->cpuset);
#if HWLOC_API_VERSION >= 0x00020000
		n->mem_mb = obj->attr->numanode.local_memory / 1048576;
#else
		n->mem_mb = obj->attr->node.memory_kB / 1024;
#endif
	}

	/* GPUs: PCI devices with display class (0x03xx) from known vendors */
	obj = NULL;
	while ((obj = hwloc_get_next_pcidev(topology, obj))) {
		dev_report_t *g;
		unsigned class_id, vendor;

		if (gpu_report_cnt >= MAX_REPORT_DEVS)
			break;
		class_id = obj->attr->pcidev.class_id;
		vendor   = obj->attr->pcidev.vendor_id;
		if ((class_id >> 8) != 0x03)	/* VGA/3D display class */
			continue;
		if ((vendor != 0x10de) &&	/* NVIDIA */
		    (vendor != 0x1002) &&	/* AMD    */
		    (vendor != 0x8086))		/* Intel  */
			continue;

		g = &gpu_report[gpu_report_cnt++];
		if (obj->name && obj->name[0])
			snprintf(g->name, sizeof(g->name), "%s", obj->name);
		else
			snprintf(g->name, sizeof(g->name), "%04x:%04x",
				 vendor, obj->attr->pcidev.device_id);
		snprintf(g->bdf, sizeof(g->bdf), "%04x:%02x:%02x.%01x",
			 (unsigned) obj->attr->pcidev.domain,
			 (unsigned) obj->attr->pcidev.bus,
			 (unsigned) obj->attr->pcidev.dev,
			 (unsigned) obj->attr->pcidev.func);
		device_affinity(topology, obj, socket_type,
				&g->numa, g->socket, sizeof(g->socket),
				g->cpus, sizeof(g->cpus));
	}

	/* InfiniBand adapters: OpenFabrics OS devices (mlx5_0, ...) */
	obj = NULL;
	while ((obj = hwloc_get_next_osdev(topology, obj))) {
		dev_report_t *ib;
		hwloc_obj_t anc;

		if (ib_report_cnt >= MAX_REPORT_DEVS)
			break;
		if (obj->attr->osdev.type != HWLOC_OBJ_OSDEV_OPENFABRICS)
			continue;

		ib = &ib_report[ib_report_cnt++];
		snprintf(ib->name, sizeof(ib->name), "%s",
			 obj->name ? obj->name : "?");
		ib->bdf[0] = '\0';
		/* link layer (IB vs Ethernet/RoCE) from sysfs */
		{
			char lpath[512];

			snprintf(lpath, sizeof(lpath),
				 "/sys/class/infiniband/%s/ports/1/link_layer",
				 ib->name);
			if (read_first_line(lpath, ib->link,
					    sizeof(ib->link)) != 0)
				ib->link[0] = '\0';
		}
		for (anc = obj->parent; anc; anc = anc->parent) {
			if (anc->type == HWLOC_OBJ_PCI_DEVICE) {
				snprintf(ib->bdf, sizeof(ib->bdf),
					 "%04x:%02x:%02x.%01x",
					 (unsigned) anc->attr->pcidev.domain,
					 (unsigned) anc->attr->pcidev.bus,
					 (unsigned) anc->attr->pcidev.dev,
					 (unsigned) anc->attr->pcidev.func);
				break;
			}
		}
		device_affinity(topology, obj, socket_type,
				&ib->numa, ib->socket, sizeof(ib->socket),
				ib->cpus, sizeof(ib->cpus));
	}
}

/*
 * A device found via raw sysfs has an empty socket string (unresolved).
 * Map its NUMA node onto the hwloc topology to fill in the active socket
 * view (L3 cache domains when l3cache_as_socket) and, if sysfs provided
 * no cpulist, the canonical NUMA cpuset.
 */
static void resolve_sys_device(hwloc_topology_t topology,
			       hwloc_obj_type_t socket_type,
			       dev_report_t *dev)
{
	hwloc_obj_t node = NULL;

	if (dev->socket[0])
		return;		/* found via hwloc, already resolved */
	if (dev->numa < 0)
		return;

	while ((node = hwloc_get_next_obj_by_type(topology, HWLOC_OBJ_NODE,
						  node))) {
		if ((int) node->os_index != dev->numa)
			continue;
		if (node->cpuset) {
			socket_set_snprintf(topology, socket_type,
					    node->cpuset, dev->socket,
					    sizeof(dev->socket));
			if (!dev->cpus[0])
				hwloc_bitmap_list_snprintf(dev->cpus,
							   sizeof(dev->cpus),
							   node->cpuset);
		}
		break;
	}
}

/*
 * topo_get - Return detailed cpuinfo on the whole system
 * (verbatim port of xcpuinfo_hwloc_topo_get())
 * Output: p_cpus - number of processors on the system
 *         p_boards - number of baseboards (containing sockets)
 *         p_sockets - number of physical processor sockets
 *         p_cores - number of physical CPU cores per socket
 *         p_threads - number of hardware execution threads per core
 *         block_map - abstract->physical block distribution map
 *         block_map_inv - physical->abstract block distribution map (inverse)
 *         return code - 0 if no error
 */
static int topo_get(uint16_t *p_cpus, uint16_t *p_boards,
		    uint16_t *p_sockets, uint16_t *p_cores, uint16_t *p_threads,
		    uint16_t *p_block_map_size,
		    uint16_t **p_block_map, uint16_t **p_block_map_inv)
{
	enum { SOCKET = 0, CORE = 1, PU = 2, LAST_OBJ = 3 };
	hwloc_topology_t topology;
	hwloc_obj_t obj;
	hwloc_obj_type_t objtype[LAST_OBJ];
	unsigned idx[LAST_OBJ];
	int nobj[LAST_OBJ];
	/* NOTE: char array replaces Slurm's bitstr_t; semantically identical */
	char used_socket[MAX_SOCKET_INX] = { 0 };
	int *cores_per_socket;
	int actual_cpus;
	int macid;
	int absid;
	int actual_boards = 1, depth, sock_cnt, tot_socks = 0;
	int i, used_core_idx, used_sock_idx;

	if (hwloc_topology_init(&topology)) {
		debug("hwloc_topology_init() failed.\n");
		return 1;
	}

	if (hwloc_topo_load(&topology) != 0) {
		hwloc_topology_destroy(topology);
		return 2;
	}

	/*
	 * Some processors (e.g. AMD Opteron 6000 series) contain multiple
	 * NUMA nodes per socket. This is a configuration which does not map
	 * into the hardware entities that Slurm optimizes resource allocation
	 * for (PU/thread, core, socket, baseboard, node and network switch).
	 * In order to optimize resource allocations on such hardware, Slurm
	 * will consider each NUMA node within the socket as a separate socket.
	 * You can disable this configuring "SchedulerParameters=Ignore_NUMA",
	 * in which case Slurm will report the correct socket count on the node,
	 * but not be able to optimize resource allocations on the NUMA nodes.
	 */
	objtype[SOCKET] = HWLOC_OBJ_SOCKET;
	objtype[CORE]   = HWLOC_OBJ_CORE;
	objtype[PU]     = HWLOC_OBJ_PU;
#if HWLOC_API_VERSION >= 0x00020000
	if (opt_ignore_numa)
		info("SchedulerParameters=Ignore_NUMA not supported by hwloc v2\n");
#else
	if (hwloc_get_type_depth(topology, HWLOC_OBJ_NODE) >
	    hwloc_get_type_depth(topology, HWLOC_OBJ_SOCKET)) {
		if (opt_ignore_numa) {
			info("Ignoring NUMA nodes within a socket\n");
		} else {
			info("Considering each NUMA node as a socket\n");
			objtype[SOCKET] = HWLOC_OBJ_NODE;
		}
	}
#endif

	if (opt_l3cache_as_socket) {
#if HWLOC_API_VERSION >= 0x00020000
		objtype[SOCKET] = HWLOC_OBJ_L3CACHE;
#else
		error("SlurmdParameters=l3cache_as_socket requires hwloc v2\n");
#endif
	} else if (opt_numa_node_as_socket) {
#if HWLOC_API_VERSION >= 0x00020000
		hwloc_obj_t numa_obj = hwloc_get_next_obj_by_type(
			topology, HWLOC_OBJ_NODE, NULL);

		if (numa_obj && numa_obj->parent) {
			char tmp[128];

			objtype[SOCKET] = numa_obj->parent->type;
			hwloc_obj_type_snprintf(tmp, sizeof(tmp),
						numa_obj->parent, 0);
			debug("numa_node_as_socket mapped to '%s'\n", tmp);
		}
#else
		error("SlurmdParameters=numa_node_as_socket requires hwloc v2\n");
#endif
	}

	/* Groups below root obj are interpreted as boards */
	obj = hwloc_get_root_obj(topology);
	obj = hwloc_get_next_child(topology, obj, NULL);
	/* NOTE: NULL guard added (Slurm dereferences obj unchecked) */
	if (obj && !hwloc_compare_types(HWLOC_OBJ_GROUP, obj->type)) {
		actual_boards = hwloc_get_nbobjs_by_depth(topology, obj->depth);
		if (actual_boards < 1)
			actual_boards = 1;
	}

	/*
	 * Count sockets/NUMA containing any cores.
	 * KNL NUMA with no cores are NOT counted.
	 */
	nobj[SOCKET] = 0;
	depth = hwloc_get_type_depth(topology, objtype[SOCKET]);
	cores_per_socket = calloc(MAX_SOCKET_INX, sizeof(int));
	sock_cnt = hwloc_get_nbobjs_by_depth(topology, depth);
	for (i = 0; i < sock_cnt; i++) {
		obj = hwloc_get_obj_by_depth(topology, depth, i);
		if (obj->type == objtype[SOCKET]) {
			cores_per_socket[i] = core_child_count(topology, obj);
			if (cores_per_socket[i] > 0) {
				nobj[SOCKET]++;
				used_socket[tot_socks] = 1;
			}
			if (++tot_socks >= MAX_SOCKET_INX) {
				fatal("Socket count exceeds %d, expand data structure size",
				      MAX_SOCKET_INX);
			}
		}
	}

	nobj[CORE] = hwloc_get_nbobjs_by_type(topology, objtype[CORE]);

	/*
	 * Workaround for hwloc bug, in some cases the topology "children" array
	 * does not get populated, so core_child_count() always returns 0
	 */
	if (nobj[SOCKET] == 0) {
		nobj[SOCKET] = hwloc_get_nbobjs_by_type(topology,
							objtype[SOCKET]);
		if (nobj[SOCKET] == 0) {
			debug("fudging nobj[SOCKET] from 0 to 1\n");
			nobj[SOCKET] = 1;
		}
		if (nobj[SOCKET] >= MAX_SOCKET_INX) {
			fatal("Socket count exceeds %d, expand data structure size",
			      MAX_SOCKET_INX);
		}
		memset(used_socket, 1, nobj[SOCKET]);
	}

	/*
	 * Workaround for hwloc
	 * hwloc_get_nbobjs_by_type() returns 0 on some architectures.
	 */
	if (nobj[CORE] == 0) {
		debug("fudging nobj[CORE] from 0 to 1\n");
		nobj[CORE] = 1;
	}
	if (nobj[SOCKET] == -1)
		fatal("can not handle nobj[SOCKET] = -1");
	if (nobj[CORE] == -1)
		fatal("can not handle nobj[CORE] = -1");
	actual_cpus = hwloc_get_nbobjs_by_type(topology, objtype[PU]);

	if ((actual_cpus % nobj[CORE]) != 0) {
		error("Thread count (%d) not multiple of core count (%d)\n",
		      actual_cpus, nobj[CORE]);
	}
	nobj[PU] = actual_cpus / nobj[CORE];	/* threads per core */

	if ((nobj[CORE] % nobj[SOCKET]) != 0) {
		error("Core count (%d) not multiple of socket count (%d)\n",
		      nobj[CORE], nobj[SOCKET]);
	}
	nobj[CORE] /= nobj[SOCKET];		/* cores per socket */

	debug("CPUs:%d Boards:%d Sockets:%d CoresPerSocket:%d ThreadsPerCore:%d\n",
	      actual_cpus, actual_boards, nobj[SOCKET], nobj[CORE], nobj[PU]);

	/* allocate block_map */
	if (p_block_map_size)
		*p_block_map_size = (uint16_t) actual_cpus;
	if (p_block_map && p_block_map_inv) {
		*p_block_map     = calloc(actual_cpus, sizeof(uint16_t));
		*p_block_map_inv = calloc(actual_cpus, sizeof(uint16_t));

		/* initialize default as linear mapping */
		for (i = 0; i < actual_cpus; i++) {
			(*p_block_map)[i]     = i;
			(*p_block_map_inv)[i] = i;
		}
		/* create map with hwloc */
		used_sock_idx = -1;
		used_core_idx = -1;
		for (idx[SOCKET] = 0; (used_sock_idx + 1) < nobj[SOCKET];
		     idx[SOCKET]++) {
			if (!used_socket[idx[SOCKET]])
				continue;
			used_sock_idx++;
			for (idx[CORE] = 0;
			     idx[CORE] < (unsigned) cores_per_socket[idx[SOCKET]];
			     idx[CORE]++) {
				used_core_idx++;
				for (idx[PU] = 0; idx[PU] < (unsigned) nobj[PU];
				     ++idx[PU]) {
					/* get hwloc_obj by indexes */
					obj = hwloc_get_obj_below_array_by_type(
						topology, 3, objtype, idx);
					if (!obj)
						continue;
					macid = obj->os_index;
					absid = used_core_idx * nobj[PU] +
						idx[PU];

					if ((macid >= actual_cpus) ||
					    (absid >= actual_cpus)) {
						/* physical or logical ID are
						 * out of range */
						continue;
					}
					(*p_block_map)[absid]     = macid;
					(*p_block_map_inv)[macid] = absid;
				}
			}
		}
	}
	/* collect GPU/IB/NUMA locality while the topology is still loaded */
	collect_devices_hwloc(topology, objtype[SOCKET]);

	/*
	 * Fill any gaps from raw sysfs: some hwloc installations (or
	 * HWLOC_XMLFILE/HWLOC_FSROOT environments) yield topologies
	 * without any I/O devices. Entries found this way are marked
	 * socket=-2 and resolved against the hwloc topology here.
	 */
	collect_devices_sys();
	for (i = 0; i < gpu_report_cnt; i++)
		resolve_sys_device(topology, objtype[SOCKET], &gpu_report[i]);
	for (i = 0; i < ib_report_cnt; i++)
		resolve_sys_device(topology, objtype[SOCKET], &ib_report[i]);

	free(cores_per_socket);
	hwloc_topology_destroy(topology);

	/* update output parameters */
	*p_cpus    = actual_cpus;
	*p_boards  = actual_boards;
	*p_sockets = nobj[SOCKET];
	*p_cores   = nobj[CORE];
	*p_threads = nobj[PU];

	return 0;
}

#else /* !HAVE_HWLOC */

/*****************************************************************************\
 * Fallback: /proc/cpuinfo parser (Slurm's non-hwloc path, verbatim port)
\*****************************************************************************/

typedef struct cpuinfo {
	uint16_t seen;
	uint32_t cpuid;
	uint32_t physid;
	uint16_t physcnt;
	uint32_t coreid;
	uint16_t corecnt;
	uint16_t siblings;
	uint16_t cores;
} cpuinfo_t;
static cpuinfo_t *cpuinfo = NULL; /* array of CPU information for get_cpuinfo */
				  /* Note: file static for qsort/compare_cpus */

static const char *cpuinfo_path = "/proc/cpuinfo";

static int compute_block_map(uint16_t numproc,
			     uint16_t **block_map, uint16_t **block_map_inv);
static int chk_cpuinfo_str(char *buffer, const char *keyword, char **valptr);
static int chk_cpuinfo_uint32(char *buffer, const char *keyword,
			      uint32_t *val);

/*
 * topo_get - /proc/cpuinfo based fallback
 * (verbatim port of Slurm's non-hwloc xcpuinfo_hwloc_topo_get())
 */
static int topo_get(uint16_t *p_cpus, uint16_t *p_boards,
		    uint16_t *p_sockets, uint16_t *p_cores, uint16_t *p_threads,
		    uint16_t *p_block_map_size,
		    uint16_t **p_block_map, uint16_t **p_block_map_inv)
{
	int retval;

	uint16_t numproc;
	uint16_t numcpu    = 0;		/* number of cpus seen */
	uint16_t numphys   = 0;		/* number of unique "physical id"s */
	uint16_t numcores  = 0;		/* number of unique "cores id"s */

	uint16_t maxsibs   = 0;		/* maximum value of "siblings" */
	uint16_t maxcores  = 0;		/* maximum value of "cores" */
	uint16_t minsibs   = 0xffff;	/* minimum value of "siblings" */
	uint16_t mincores  = 0xffff;	/* minimum value of "cores" */

	uint32_t maxcpuid  = 0;		/* maximum CPU ID ("processor") */
	uint32_t maxphysid = 0;		/* maximum "physical id" */
	uint32_t maxcoreid = 0;		/* maximum "core id" */
	uint32_t mincpuid  = 0xffffffff;/* minimum CPU ID ("processor") */
	uint32_t minphysid = 0xffffffff;/* minimum "physical id" */
	uint32_t mincoreid = 0xffffffff;/* minimum "core id" */
	int i;
	FILE *cpu_info_file;
	char buffer[128];
	uint16_t curcpu = 0, sockets, cores, threads;

	if (opt_l3cache_as_socket || opt_numa_node_as_socket)
		error("l3cache_as_socket/numa_node_as_socket require a hwloc build\n");
	if (opt_ignore_numa)
		debug("ignore_numa has no effect without hwloc\n");

	get_procs(&numproc);
	*p_cpus = numproc;
	*p_boards = 1;		/* Boards not identified from /proc/cpuinfo */
	*p_sockets = numproc;	/* initially all single core/thread */
	*p_cores   = 1;
	*p_threads = 1;
	*p_block_map_size = 0;
	*p_block_map      = NULL;
	*p_block_map_inv  = NULL;

	cpu_info_file = fopen(cpuinfo_path, "r");
	if (cpu_info_file == NULL) {
		error("error %d opening %s\n", errno, cpuinfo_path);
		return errno;
	}

	/* Note: assumes all processor IDs are within [0:numproc-1] */
	/*       treats physical/core IDs as tokens, not indices */
	if (cpuinfo)
		memset(cpuinfo, 0, numproc * sizeof(cpuinfo_t));
	else
		cpuinfo = calloc(numproc, sizeof(cpuinfo_t));

	curcpu = 0;
	while (fgets(buffer, sizeof(buffer), cpu_info_file) != NULL) {
		uint32_t val;
		if (chk_cpuinfo_uint32(buffer, "processor", &val)) {
			curcpu = numcpu;
			numcpu++;
			if (curcpu >= numproc) {
				info("processor limit reached (%u >= %d)\n",
				     curcpu, numproc);
				continue;
			}
			cpuinfo[curcpu].seen = 1;
			cpuinfo[curcpu].cpuid = val;
			maxcpuid = val > maxcpuid ? val : maxcpuid;
			mincpuid = val < mincpuid ? val : mincpuid;
		} else if (chk_cpuinfo_uint32(buffer, "physical id", &val)) {
			/* see if the ID has already been seen */
			for (i = 0; i < numproc; i++) {
				if ((cpuinfo[i].physid == val)
				    &&  (cpuinfo[i].physcnt))
					break;
			}

			if (i == numproc) {		/* new ID... */
				numphys++;		/* ...increment total */
			} else {			/* existing ID... */
				cpuinfo[i].physcnt++;	/* ...update ID cnt */
			}

			if (curcpu < numproc) {
				cpuinfo[curcpu].physcnt++;
				cpuinfo[curcpu].physid = val;
			}

			maxphysid = val > maxphysid ? val : maxphysid;
			minphysid = val < minphysid ? val : minphysid;
		} else if (chk_cpuinfo_uint32(buffer, "core id", &val)) {
			/* see if the ID has already been seen */
			for (i = 0; i < numproc; i++) {
				if ((cpuinfo[i].coreid == val)
				    &&  (cpuinfo[i].corecnt))
					break;
			}

			if (i == numproc) {		/* new ID... */
				numcores++;		/* ...increment total */
			} else {			/* existing ID... */
				cpuinfo[i].corecnt++;	/* ...update ID cnt */
			}

			if (curcpu < numproc) {
				cpuinfo[curcpu].corecnt++;
				cpuinfo[curcpu].coreid = val;
			}

			maxcoreid = val > maxcoreid ? val : maxcoreid;
			mincoreid = val < mincoreid ? val : mincoreid;
		} else if (chk_cpuinfo_uint32(buffer, "siblings", &val)) {
			/* Note: this value is a count, not an index */
			if (val > numproc) {	/* out of bounds, ignore */
				debug("siblings is %u (> %d), ignored\n",
				      val, numproc);
				continue;
			}
			if (curcpu < numproc)
				cpuinfo[curcpu].siblings = val;
			maxsibs = val > maxsibs ? val : maxsibs;
			minsibs = val < minsibs ? val : minsibs;
		} else if (chk_cpuinfo_uint32(buffer, "cpu cores", &val)) {
			/* Note: this value is a count, not an index */
			if (val > numproc) {	/* out of bounds, ignore */
				debug("cores is %u (> %d), ignored\n",
				      val, numproc);
				continue;
			}
			if (curcpu < numproc)
				cpuinfo[curcpu].cores = val;
			maxcores = val > maxcores ? val : maxcores;
			mincores = val < mincores ? val : mincores;
		}
	}

	fclose(cpu_info_file);

	/*** Sanity check ***/
	if (minsibs == 0) minsibs = 1;		/* guarantee non-zero */
	if (maxsibs == 0) {
		minsibs = 1;
		maxsibs = 1;
	}
	if (maxcores == 0) {			/* no core data */
		mincores = 0;
		maxcores = 0;
	}

	/*** Compute Sockets/Cores/Threads ***/
	if ((minsibs == maxsibs) &&		/* homogeneous system */
	    (mincores == maxcores)) {
		sockets = numphys;		/* unique "physical id" */
		if (sockets <= 1) {		/* verify single socket */
			sockets = numcpu / maxsibs; /* maximum "siblings" */
		}
		if (sockets == 0)
			sockets = 1;		/* guarantee non-zero */

		cores = numcores / sockets;	/* unique "core id" */
		if (maxcores > cores)
			cores = maxcores;	/* maximum "cpu cores" */

		if (cores == 0) {
			cores = numcpu / sockets;	/* assume multi-core */
			if (cores > 1)
				debug("cpuinfo missing 'core id' or 'cpu cores' but assuming multi-core\n");
		}
		if (cores == 0)
			cores = 1;	/* guarantee non-zero */

		threads = numcpu / (sockets * cores); /* solve for threads */
		if (threads == 0)
			threads = 1;	/* guarantee non-zero */
	} else {				/* heterogeneous system */
		sockets = numcpu;
		cores   = 1;			/* one core per socket */
		threads = 1;			/* one thread per core */
	}

	*p_sockets = sockets;		/* update output parameters */
	*p_cores   = cores;
	*p_threads = threads;

	*p_block_map_size = numcpu;
	retval = compute_block_map(*p_block_map_size, p_block_map,
				   p_block_map_inv);

	free(cpuinfo);			/* done with raw cpuinfo data */
	cpuinfo = NULL;

	return retval;
}

/* chk_cpuinfo_str
 *	check a line of cpuinfo data (buffer) for a keyword.  If it
 *	exists, return the string value for that keyword in *valptr.
 */
static int chk_cpuinfo_str(char *buffer, const char *keyword, char **valptr)
{
	char *ptr;
	if (strncmp(buffer, keyword, strlen(keyword)))
		return false;

	ptr = strstr(buffer, ":");
	if (ptr != NULL)
		ptr++;
	*valptr = ptr;
	return true;
}

/* chk_cpuinfo_uint32
 *	check a line of cpuinfo data (buffer) for a keyword.  If it
 *	exists, return the uint32 value for that keyword in *val.
 */
static int chk_cpuinfo_uint32(char *buffer, const char *keyword, uint32_t *val)
{
	char *valptr;
	if (chk_cpuinfo_str(buffer, keyword, &valptr)) {
		*val = strtoul(valptr, (char **) NULL, 10);
		return true;
	} else {
		return false;
	}
}

/*
 * compute_block_map - Compute abstract->machine block mapping (and inverse)
 *   allows computation of CPU ID masks for an abstract block distribution
 *   of logical processors which can then be mapped the IDs used in the
 *   actual machine processor ID ordering (which can be BIOS/OS dependent)
 */
static int icmp16(uint16_t a, uint16_t b)
{
	if (a < b) {
		return -1;
	} else if (a == b) {
		return 0;
	} else {
		return 1;
	}
}
static int icmp32(uint32_t a, uint32_t b)
{
	if (a < b) {
		return -1;
	} else if (a == b) {
		return 0;
	} else {
		return 1;
	}
}

static int compare_cpus(const void *a1, const void *b1)
{
	uint16_t *a = (uint16_t *) a1;
	uint16_t *b = (uint16_t *) b1;
	int cmp;

	cmp = -1 * icmp16(cpuinfo[*a].seen, cpuinfo[*b].seen); /* seen first */
	if (cmp != 0)
		return cmp;

	cmp = icmp32(cpuinfo[*a].physid, cpuinfo[*b].physid); /* key 1: physid */
	if (cmp != 0)
		return cmp;

	cmp = icmp32(cpuinfo[*a].coreid, cpuinfo[*b].coreid); /* key 2: coreid */
	if (cmp != 0)
		return cmp;

	cmp = icmp32(cpuinfo[*a].cpuid, cpuinfo[*b].cpuid);   /* key 3: cpu id */
	return cmp;
}

static int compute_block_map(uint16_t numproc,
			     uint16_t **block_map, uint16_t **block_map_inv)
{
	uint16_t i;
	/* Compute abstract->machine block mapping (and inverse) */
	if (block_map) {
		*block_map = calloc(numproc, sizeof(uint16_t));
		for (i = 0; i < numproc; i++)
			(*block_map)[i] = i;
		qsort(*block_map, numproc, sizeof(uint16_t), &compare_cpus);
	}
	if (block_map && block_map_inv) {
		*block_map_inv = calloc(numproc, sizeof(uint16_t));
		for (i = 0; i < numproc; i++) {
			uint16_t idx = (*block_map)[i];
			(*block_map_inv)[idx] = i;
		}
	}
	return 0;
}

#endif /* HAVE_HWLOC */

/*****************************************************************************\
 * Device/locality report from raw /sys
 * Shared by both builds: the sole source in nohwloc builds, and a
 * gap-filler in hwloc builds whose loaded topology contains no I/O
 * devices (e.g. HWLOC_XMLFILE/HWLOC_FSROOT environments, or minimal
 * hwloc installations without PCI/OS-device discovery).
\*****************************************************************************/

static int read_first_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

/* read numa_node and local_cpulist for a PCI device (sysfs BDF directory) */
static void sys_pci_locality(const char *bdf, int *numa, char *cpus,
			     size_t len)
{
	char path[512], buf[64];

	*numa = -1;
	cpus[0] = '\0';

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/numa_node", bdf);
	if (read_first_line(path, buf, sizeof(buf)) == 0)
		*numa = atoi(buf);

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/local_cpulist",
		 bdf);
	read_first_line(path, cpus, len);
}

static void collect_devices_sys(void)
{
	DIR *d;
	struct dirent *de;
	char path[512], buf[512];

	/* NUMA nodes: cpulist + meminfo (skip if hwloc already reported) */
	if ((numa_report_cnt == 0) &&
	    (d = opendir("/sys/devices/system/node"))) {
		while ((de = readdir(d))) {
			numa_report_t *n;
			FILE *f;
			if (strncmp(de->d_name, "node", 4) ||
			    !isdigit(de->d_name[4]))
				continue;
			if (numa_report_cnt >= MAX_REPORT_NUMA)
				break;
			n = &numa_report[numa_report_cnt];
			n->os_index = atoi(de->d_name + 4);
			n->mem_mb = 0;
			snprintf(path, sizeof(path),
				 "/sys/devices/system/node/%s/cpulist",
				 de->d_name);
			if (read_first_line(path, n->cpus,
					    sizeof(n->cpus)) != 0)
				continue;
			snprintf(path, sizeof(path),
				 "/sys/devices/system/node/%s/meminfo",
				 de->d_name);
			if ((f = fopen(path, "r"))) {
				while (fgets(buf, sizeof(buf), f)) {
					char *p = strstr(buf, "MemTotal:");
					if (p) {
						n->mem_mb =
							strtoul(p + 9, NULL,
								10) / 1024;
						break;
					}
				}
				fclose(f);
			}
			numa_report_cnt++;
		}
		closedir(d);
	}

	/* GPUs: PCI devices with display class from known vendors
	 * (skip if hwloc already reported some) */
	if ((gpu_report_cnt == 0) && (d = opendir("/sys/bus/pci/devices"))) {
		while ((de = readdir(d))) {
			dev_report_t *g;
			unsigned long class_code, vendor, device;
			if (de->d_name[0] == '.')
				continue;
			if (gpu_report_cnt >= MAX_REPORT_DEVS)
				break;
			snprintf(path, sizeof(path),
				 "/sys/bus/pci/devices/%s/class", de->d_name);
			if (read_first_line(path, buf, sizeof(buf)) != 0)
				continue;
			class_code = strtoul(buf, NULL, 0);
			if (((class_code >> 16) & 0xff) != 0x03)
				continue;
			snprintf(path, sizeof(path),
				 "/sys/bus/pci/devices/%s/vendor",
				 de->d_name);
			if (read_first_line(path, buf, sizeof(buf)) != 0)
				continue;
			vendor = strtoul(buf, NULL, 0);
			if ((vendor != 0x10de) && (vendor != 0x1002) &&
			    (vendor != 0x8086))
				continue;
			snprintf(path, sizeof(path),
				 "/sys/bus/pci/devices/%s/device",
				 de->d_name);
			device = 0;
			if (read_first_line(path, buf, sizeof(buf)) == 0)
				device = strtoul(buf, NULL, 0);

			g = &gpu_report[gpu_report_cnt++];
			snprintf(g->name, sizeof(g->name), "%04lx:%04lx",
				 vendor, device);
			snprintf(g->bdf, sizeof(g->bdf), "%.15s", de->d_name);
			g->socket[0] = '\0'; /* unresolved; hwloc maps it */
			sys_pci_locality(de->d_name, &g->numa, g->cpus,
					 sizeof(g->cpus));
		}
		closedir(d);
	}

	/* InfiniBand adapters: /sys/class/infiniband/<name>/device -> BDF
	 * (skip if hwloc already reported some) */
	if ((ib_report_cnt == 0) && (d = opendir("/sys/class/infiniband"))) {
		while ((de = readdir(d))) {
			dev_report_t *ib;
			char link[256], *base;
			ssize_t len;
			if (de->d_name[0] == '.')
				continue;
			if (ib_report_cnt >= MAX_REPORT_DEVS)
				break;

			ib = &ib_report[ib_report_cnt++];
			snprintf(ib->name, sizeof(ib->name), "%.63s",
				 de->d_name);
			ib->bdf[0] = '\0';
			ib->socket[0] = '\0'; /* unresolved; hwloc maps it */
			ib->numa = -1;
			ib->cpus[0] = '\0';

			/* link layer (IB vs Ethernet/RoCE) */
			snprintf(path, sizeof(path),
				 "/sys/class/infiniband/%s/ports/1/link_layer",
				 de->d_name);
			if (read_first_line(path, ib->link,
					    sizeof(ib->link)) != 0)
				ib->link[0] = '\0';

			snprintf(path, sizeof(path),
				 "/sys/class/infiniband/%s/device",
				 de->d_name);
			len = readlink(path, link, sizeof(link) - 1);
			if (len < 0)
				continue;
			link[len] = '\0';
			base = strrchr(link, '/');
			if (!base)
				continue;
			snprintf(ib->bdf, sizeof(ib->bdf), "%.15s", base + 1);
			sys_pci_locality(ib->bdf, &ib->numa, ib->cpus,
					 sizeof(ib->cpus));
		}
		closedir(d);
	}
}

static const char *int_or_dash(int v, char *buf)
{
	if (v < 0)
		return "-";
	snprintf(buf, 16, "%d", v);
	return buf;
}

static int cmp_numa_report(const void *a, const void *b)
{
	return ((const numa_report_t *) a)->os_index -
	       ((const numa_report_t *) b)->os_index;
}

static int cmp_dev_bdf(const void *a, const void *b)
{
	return strcmp(((const dev_report_t *) a)->bdf,
		      ((const dev_report_t *) b)->bdf);
}

static int cmp_dev_name(const void *a, const void *b)
{
	return strcmp(((const dev_report_t *) a)->name,
		      ((const dev_report_t *) b)->name);
}

static void print_device_report(void)
{
	char sbuf[16];

	/* deterministic order regardless of enumeration source */
	if (numa_report_cnt > 1)
		qsort(numa_report, numa_report_cnt, sizeof(numa_report_t),
		      cmp_numa_report);
	if (gpu_report_cnt > 1)
		qsort(gpu_report, gpu_report_cnt, sizeof(dev_report_t),
		      cmp_dev_bdf);
	if (ib_report_cnt > 1)
		qsort(ib_report, ib_report_cnt, sizeof(dev_report_t),
		      cmp_dev_name);

	printf("------\n");
	if (opt_l3cache_as_socket)
		printf("(socket column = L3 cache domain(s) local to the device; '-' = unknown)\n");

	if (numa_report_cnt) {
		printf("NUMA nodes (memory locality):\n");
		for (int i = 0; i < numa_report_cnt; i++)
			printf("  NUMA[%d] cpus=%s mem=%" PRIu64 "MB\n",
			       numa_report[i].os_index, numa_report[i].cpus,
			       numa_report[i].mem_mb);
	} else {
		printf("NUMA nodes: none detected\n");
	}

	if (gpu_report_cnt) {
		printf("GPUs:\n");
		for (int i = 0; i < gpu_report_cnt; i++)
			printf("  GPU[%d] %s pci=%s numa=%s socket=%s cpus=%s\n",
			       i, gpu_report[i].name, gpu_report[i].bdf,
			       int_or_dash(gpu_report[i].numa, sbuf),
			       gpu_report[i].socket[0] ?
			       gpu_report[i].socket : "-",
			       gpu_report[i].cpus);
	} else {
		printf("GPUs: none detected\n");
	}

	if (ib_report_cnt) {
		printf("InfiniBand/RDMA adapters:\n");
		for (int i = 0; i < ib_report_cnt; i++)
			printf("  IB[%d] %s pci=%s link=%s numa=%s socket=%s cpus=%s\n",
			       i, ib_report[i].name, ib_report[i].bdf,
			       ib_report[i].link[0] ? ib_report[i].link : "-",
			       int_or_dash(ib_report[i].numa, sbuf),
			       ib_report[i].socket[0] ?
			       ib_report[i].socket : "-",
			       ib_report[i].cpus);
	} else {
		printf("InfiniBand adapters: none detected\n");
	}
}

static void print_block_map(uint16_t cpus, uint16_t *block_map,
			    uint16_t *block_map_inv)
{
	printf("------\n");
	printf("Abstract -> Machine logical CPU ID block mapping:\n");
	printf("%8s %8s %8s\n", "Abstract", "Physical", "Inverse");
	for (int i = 0; i < cpus; i++)
		printf("%8d %8u %8u\n", i, block_map[i], block_map_inv[i]);
	printf("------\n");
}

static void usage(char *prog)
{
	printf("Usage: %s [-C] [-p <list>] [-m] [-v] [-h]\n"
	       "\n"
	       "Print the node CPU topology exactly as Slurm's slurmd would\n"
	       "detect it (port of src/slurmd/common/xcpuinfo.c). Output\n"
	       "format matches \"slurmd -C\".\n"
	       "\n"
	       "Options:\n"
	       "  -C, --check             accepted for slurmd habit; this is the\n"
	       "                          default (and only) action\n"
	       "  -p, --parameters=<list> comma-separated topology modifiers:\n"
	       "                            l3cache_as_socket  - count each hwloc L3\n"
	       "                              cache domain as a socket (hwloc v2 only)\n"
	       "                            numa_node_as_socket - use NUMA parent as\n"
	       "                              socket (hwloc v2 only)\n"
	       "                            ignore_numa - Ignore_NUMA (hwloc v1 only)\n"
	       "  -m, --map               also print the abstract<->physical\n"
	       "                          block distribution map\n"
	       "  -q, --quiet             only print the slurmd -C lines (suppress\n"
	       "                          the GPU/IB/NUMA locality report)\n"
	       "  -v, --verbose           debug output on stderr\n"
	       "  -h, --help              this help\n"
	       "\n"
	       "After the slurmd -C lines, the tool reports NUMA memory locality\n"
	       "plus GPU and InfiniBand adapters with their PCI address, NUMA\n"
	       "node, socket (per the active socket definition) and CPU affinity.\n"
	       "\n"
	       "Build: with hwloc (default, like Slurm) or \"make nohwloc\" for\n"
	       "the /proc/cpuinfo + /sys fallback.\n",
	       prog);
}

int main(int argc, char **argv)
{
	static struct option long_opts[] = {
		{ "check",      no_argument,       NULL, 'C' },
		{ "parameters", required_argument, NULL, 'p' },
		{ "map",        no_argument,       NULL, 'm' },
		{ "quiet",      no_argument,       NULL, 'q' },
		{ "verbose",    no_argument,       NULL, 'v' },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL,         0,                 NULL,  0  },
	};
	uint16_t cpus = 0, boards = 0, sockets = 0, cores = 0, threads = 0;
	uint16_t block_map_size = 0;
	uint16_t *block_map = NULL, *block_map_inv = NULL;
	uint64_t real_memory = 0;
	uint32_t up_time = 0;
	char name[128];
	int c;

	while ((c = getopt_long(argc, argv, "Cp:mqvh", long_opts, NULL)) != -1) {
		switch (c) {
		case 'C':
			break;	/* default action, like slurmd -C */
		case 'p': {
			free(opt_parameters);
			opt_parameters = strdup(optarg);
			char *saveptr = NULL, *tok;
			for (tok = strtok_r(optarg, ",", &saveptr); tok;
			     tok = strtok_r(NULL, ",", &saveptr)) {
				if (!strcasecmp(tok, "l3cache_as_socket"))
					opt_l3cache_as_socket = true;
				else if (!strcasecmp(tok,
						     "numa_node_as_socket"))
					opt_numa_node_as_socket = true;
				else if (!strcasecmp(tok, "ignore_numa"))
					opt_ignore_numa = true;
				else
					fatal("unknown parameter: %s", tok);
			}
			break;
		}
		case 'm':
			opt_map = true;
			break;
		case 'q':
			opt_quiet = true;
			break;
		case 'v':
			opt_verbose = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (opt_l3cache_as_socket && opt_numa_node_as_socket)
		fatal("l3cache_as_socket and numa_node_as_socket are mutually exclusive");

	if (topo_get(&cpus, &boards, &sockets, &cores, &threads,
		     &block_map_size, &block_map, &block_map_inv))
		fatal("Failed to detect hardware correctly");

#ifndef HAVE_HWLOC
	/* hwloc builds collect devices inside topo_get() before the
	 * topology is destroyed; the fallback reads /sys instead */
	collect_devices_sys();
#endif

	get_memory(&real_memory);

	/* short hostname, like Slurm's gethostname_short() */
	if (gethostname(name, sizeof(name))) {
		error("gethostname: %s\n", strerror(errno));
		snprintf(name, sizeof(name), "localhost");
	}
	name[strcspn(name, ".")] = '\0';

	/* output format matches slurmd -C (_print_config in slurmd.c) */
	printf("NodeName=%s CPUs=%u Boards=%u SocketsPerBoard=%u "
	       "CoresPerSocket=%u ThreadsPerCore=%u RealMemory=%"
	       PRIu64 "%s%s\n",
	       name, cpus, boards, (sockets / boards), cores, threads,
	       real_memory,
	       opt_parameters ? " Parameters=" : "",
	       opt_parameters ? opt_parameters : "");

	get_up_time(&up_time);
	printf("UpTime=%u-%2.2u:%2.2u:%2.2u\n",
	       up_time / 86400, (up_time / 3600) % 24,
	       (up_time / 60) % 60, up_time % 60);

	if (opt_map && block_map && block_map_inv)
		print_block_map(cpus, block_map, block_map_inv);

	if (!opt_quiet)
		print_device_report();

	free(block_map);
	free(block_map_inv);
	free(opt_parameters);
	return 0;
}
