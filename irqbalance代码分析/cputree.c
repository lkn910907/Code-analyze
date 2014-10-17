#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <glib.h>

#include "irqbalance.h"


GList *cpus;
GList *cache_domains;
GList *packages;

int package_count;
int cache_domain_count;
int core_count;

/* Users want to be able to keep interrupts away from some cpus; store these in a cpumask_t */
cpumask_t banned_cpus;

cpumask_t cpu_possible_map;

/* 
   it's convenient to have the complement of banned_cpus available so that 
   the AND operator can be used to mask out unwanted cpus
*/
cpumask_t unbanned_cpus;

/*将cache_domain域加入到指定的package域中，如果不存在packageid的package域，则在package队尾增加一个
packageid的package域，并将cache_domain域插入*/
static struct topo_obj* add_cache_domain_to_package(struct topo_obj *cache, 
						    int packageid, cpumask_t package_mask)
{
	GList *entry;
	struct topo_obj *package;
	struct topo_obj *lcache; 

	/*检查packageid的package域是否已经存在*/
	entry = g_list_first(packages);
	while (entry) {
		package = entry->data;
		if (cpus_equal(package_mask, package->mask)) {
			if (packageid != package->number)
				log(TO_ALL, LOG_WARNING, "package_mask with different physical_package_id found!\n");
			break;
		}
		entry = g_list_next(entry);
	}

	/*遍历完package链表都没找到指定的package域后，新建一个number为packageid的package域*/
	if (!entry) {
		package = calloc(sizeof(struct topo_obj), 1);
		if (!package)
			return NULL;
		package->mask = package_mask;
		package->obj_type = OBJ_TYPE_PACKAGE;
		package->obj_type_list = &packages;
		package->number = packageid;
		packages = g_list_append(packages, package);
		package_count++;
	}

	/*检查该cache域是否已经存在package域子链表上*/
	entry = g_list_first(package->children);
	while (entry) {
		lcache = entry->data;
		if (lcache == cache)
			break;
		entry = g_list_next(entry);
	}

	/*cache域不在子链表上时，插入指定package域的子链表中*/
	if (!entry) {
		package->children = g_list_append(package->children, cache);
		cache->parent = package;
	}

	return package;
}

/*将cpu域加入到指定的cache域中，如果指定cache域不存在，则在cache域链表队尾增加一个
指定的cache域，并将cpu域插入*/
static struct topo_obj* add_cpu_to_cache_domain(struct topo_obj *cpu,
						    cpumask_t cache_mask)
{
	GList *entry;
	struct topo_obj *cache;
	struct topo_obj *lcpu;

	entry = g_list_first(cache_domains);

	while (entry) {
		cache = entry->data;
		if (cpus_equal(cache_mask, cache->mask))
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		cache = calloc(sizeof(struct topo_obj), 1);
		if (!cache)
			return NULL;
		cache->obj_type = OBJ_TYPE_CACHE;
		cache->mask = cache_mask;
		cache->number = cache_domain_count;
		cache->obj_type_list = &cache_domains;
		cache_domains = g_list_append(cache_domains, cache);
		cache_domain_count++;
	}

	entry = g_list_first(cache->children);
	while (entry) {
		lcpu = entry->data;
		if (lcpu == cpu)
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		cache->children = g_list_append(cache->children, cpu);
		cpu->parent = (struct topo_obj *)cache;
	}

	return cache;
}

/*创建一个CPU域，并将其加入到对应的cache域和package域以及CPU域链表中*/ 
static void do_one_cpu(char *path)
{
	struct topo_obj *cpu;
	FILE *file;
	char new_path[PATH_MAX];
	cpumask_t cache_mask, package_mask;
	struct topo_obj *cache;
	struct topo_obj *package;
	DIR *dir;
	struct dirent *entry;
	int nodeid;
	int packageid = 0;
	unsigned int max_cache_index, cache_index, cache_stat;

	/*无视那些离线的CPU */
	snprintf(new_path, PATH_MAX, "%s/online", path);
	file = fopen(new_path, "r");
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file)==0)
			return;
		fclose(file);
		if (line && line[0]=='0') {
			free(line);
			return;
		}
		free(line);
	}

	cpu = calloc(sizeof(struct topo_obj), 1);
	if (!cpu)
		return;

	cpu->obj_type = OBJ_TYPE_CPU;
	/*将获得的CPU编号转换成十进制无符号长整型*/
	cpu->number = strtoul(&path[27], NULL, 10);

	/*根据CPU编号设置各个位图掩码*/
	cpu_set(cpu->number, cpu_possible_map);
	cpu_set(cpu->number, cpu->mask);
	cpus_clear(cache_mask);
	cpu_set(cpu->number, cache_mask);

	/*如果该CPU是banned，不将其加入CPU拓扑链表中 */
	if (cpus_intersects(cpu->mask, banned_cpus)) {
		free(cpu);
		/* even though we don't use the cpu we do need to count it */
		core_count++;
		return;
	}

	/* try to read the package mask; if it doesn't exist assume solitary */
	snprintf(new_path, PATH_MAX, "%s/topology/core_siblings", path);
	file = fopen(new_path, "r");
	cpu_set(cpu->number, package_mask);
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file)) 
			cpumask_parse_user(line, strlen(line), package_mask);
		fclose(file);
		free(line);
	}
	/*获取所在package域的id */
	snprintf(new_path, PATH_MAX, "%s/topology/physical_package_id", path);
	file = fopen(new_path, "r");
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file))
			packageid = strtoul(line, NULL, 10);
		fclose(file);
		free(line);
	}

	/* try to read the cache mask; if it doesn't exist assume solitary */
	/* We want the deepest cache level available */
	cpu_set(cpu->number, cache_mask);
	max_cache_index = 0;
	cache_index = 1;
	cache_stat = 0;
	do {
		struct stat sb;
		snprintf(new_path, PATH_MAX, "%s/cache/index%d/shared_cpu_map", path, cache_index);
		cache_stat = stat(new_path, &sb);
		if (!cache_stat) {
			max_cache_index = cache_index;
			if (max_cache_index == deepest_cache)
				break;
			cache_index ++;
		}
	} while(!cache_stat);

	if (max_cache_index > 0) {
		snprintf(new_path, PATH_MAX, "%s/cache/index%d/shared_cpu_map", path, max_cache_index);
		file = fopen(new_path, "r");
		if (file) {
			char *line = NULL;
			size_t size = 0;
			if (getline(&line, &size, file))
				cpumask_parse_user(line, strlen(line), cache_mask);
			fclose(file);
			free(line);
		}
	}

	nodeid=-1;
	if (numa_avail) {
		dir = opendir(path);
		do {
			entry = readdir(dir);
			if (!entry)
				break;
			if (strstr(entry->d_name, "node")) {
				nodeid = strtoul(&entry->d_name[4], NULL, 10);
				break;
			}
		} while (entry);
		closedir(dir);
	}

	/*保证位图掩码所代表的CPU都是可用的，未被ban*/
	cpus_and(cache_mask, cache_mask, unbanned_cpus);
	cpus_and(package_mask, package_mask, unbanned_cpus);

	/*将CPU域加入到cache域和package域中*/
	cache = add_cpu_to_cache_domain(cpu, cache_mask);
	package = add_cache_domain_to_package(cache, packageid, package_mask);
	add_package_to_node(package, nodeid);

	cpu->obj_type_list = &cpus;
	/*将该CPU域加入到CPU拓扑链表结构中*/
	cpus = g_list_append(cpus, cpu);
	core_count++;
}

static void dump_irq(struct irq_info *info, void *data)
{
	int spaces = (long int)data;
	int i;
	for (i=0; i<spaces; i++) log(TO_CONSOLE, LOG_INFO, " ");
	log(TO_CONSOLE, LOG_INFO, "Interrupt %i node_num is %d (%s/%u) \n",
	    info->irq, irq_numa_node(info)->number, classes[info->class], (unsigned int)info->load);
}

static void dump_topo_obj(struct topo_obj *d, void *data __attribute__((unused)))
{
	struct topo_obj *c = (struct topo_obj *)d;
	log(TO_CONSOLE, LOG_INFO, "                CPU number %i  numa_node is %d (load %lu)\n",
	    c->number, cpu_numa_node(c)->number , (unsigned long)c->load);
	if (c->interrupts)
		for_each_irq(c->interrupts, dump_irq, (void *)18);
}

static void dump_cache_domain(struct topo_obj *d, void *data)
{
	char *buffer = data;
	cpumask_scnprintf(buffer, 4095, d->mask);
	log(TO_CONSOLE, LOG_INFO, "        Cache domain %i:  numa_node is %d cpu mask is %s  (load %lu) \n",
	    d->number, cache_domain_numa_node(d)->number, buffer, (unsigned long)d->load);
	if (d->children)
		for_each_object(d->children, dump_topo_obj, NULL);
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, dump_irq, (void *)10);
}

static void dump_package(struct topo_obj *d, void *data)
{
	char *buffer = data;
	cpumask_scnprintf(buffer, 4096, d->mask);
	log(TO_CONSOLE, LOG_INFO, "Package %i:  numa_node is %d cpu mask is %s (load %lu)\n",
	    d->number, package_numa_node(d)->number, buffer, (unsigned long)d->load);
	if (d->children)
		for_each_object(d->children, dump_cache_domain, buffer);
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, dump_irq, (void *)2);
}

void dump_tree(void)
{
	char buffer[4096];
	for_each_object(packages, dump_package, buffer);
}

/*清除一个中断的负载记录*/
static void clear_irq_stats(struct irq_info *info, void *data __attribute__((unused)))
{
	info->load = 0;
}

/*清除一个域及其子域的中断负载记录*/
static void clear_obj_stats(struct topo_obj *d, void *data __attribute__((unused)))
{
	for_each_object(d->children, clear_obj_stats, NULL);
	for_each_irq(d->interrupts, clear_irq_stats, NULL);
}

/*清除非一致性内存节点的中断负载记录 */
void clear_work_stats(void)
{
	for_each_object(numa_nodes, clear_obj_stats, NULL);
}

/*遍历系统的CPU，将其全部加入CPU拓扑链表中*/
void parse_cpu_tree(void)
{
	DIR *dir;
	struct dirent *entry;

	cpus_complement(unbanned_cpus, banned_cpus);

	dir = opendir("/sys/devices/system/cpu");
	if (!dir)
		return;
	do {
		int num;
		char pad;
		entry = readdir(dir);
		/*
 		 * We only want to count real cpus, not cpufreq and
 		 * cpuidle
 		 */
		if (entry &&
		    sscanf(entry->d_name, "cpu%d%c", &num, &pad) == 1 &&
		    !strchr(entry->d_name, ' ')) {
			char new_path[PATH_MAX];
			sprintf(new_path, "/sys/devices/system/cpu/%s", entry->d_name);
			do_one_cpu(new_path);
		}
	} while (entry);
	closedir(dir);  

	if (debug_mode)
		dump_tree();

}


/*清除CPU拓扑链表，包括package链表，cache_domain链表以及CPU链表中保存的信息 */
void clear_cpu_tree(void)
{
	GList *item;
	struct topo_obj *cpu;
	struct topo_obj *cache_domain;
	struct topo_obj *package;

	while (packages) {
		item = g_list_first(packages);
		package = item->data;
		g_list_free(package->children);
		g_list_free(package->interrupts);
		free(package);
		packages = g_list_delete_link(packages, item);
	}
	package_count = 0;

	while (cache_domains) {
		item = g_list_first(cache_domains);
		cache_domain = item->data;
		g_list_free(cache_domain->children);
		g_list_free(cache_domain->interrupts);
		free(cache_domain);
		cache_domains = g_list_delete_link(cache_domains, item);
	}
	cache_domain_count = 0;


	while (cpus) {
		item = g_list_first(cpus);
		cpu = item->data;
		g_list_free(cpu->interrupts);
		free(cpu);
		cpus = g_list_delete_link(cpus, item);
	}
	core_count = 0;

}

/*比较两个CPU，一般用于遍历比较，寻找特定的CPU*/
static gint compare_cpus(gconstpointer a, gconstpointer b)
{
	const struct topo_obj *ai = a;
	const struct topo_obj *bi = b;

	return ai->number - bi->number;	
}

/*若找到指定CPU，则返回其结构信息，否则返回空*/
struct topo_obj *find_cpu_core(int cpunr)
{
	GList *entry;
	struct topo_obj find;

	find.number = cpunr;
	entry = g_list_find_custom(cpus, &find, compare_cpus);

	return entry ? entry->data : NULL;
}	

/*获取CPU数目*/
int get_cpu_count(void)
{
	return g_list_length(cpus);
}

