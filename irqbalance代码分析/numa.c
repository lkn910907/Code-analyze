#include "config.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>

#include "irqbalance.h"

#define SYSFS_NODE_PATH "/sys/devices/system/node"

GList *numa_nodes = NULL;

static struct topo_obj unspecified_node_template = {
	.load = 0,
	.number = -1,
	.obj_type = OBJ_TYPE_NODE,
	.mask = CPU_MASK_ALL,
	.interrupts = NULL,
	.children = NULL,
	.parent = NULL,
	.obj_type_list = &numa_nodes,
};

static struct topo_obj unspecified_node;

/*增加一个非一致性内存访问节点域结构*/
static void add_one_node(const char *nodename)
{
	char path[PATH_MAX];
	struct topo_obj *new;
	char *cpustr = NULL;
	FILE *f;
	ssize_t ret;
	size_t blen;

	new = calloc(1, sizeof(struct topo_obj));
	if (!new)
		return;
	/*节点应该有可运行的CPU*/
	sprintf(path, "%s/%s/cpumap", SYSFS_NODE_PATH, nodename);
	f = fopen(path, "r");
	if (!f) {
		free(new);
		return;
	}
	if (ferror(f)) {
		cpus_clear(new->mask);
	} else {
		/*根据节点的可运行CPU位图来设置节点域的位图掩码*/
		ret = getline(&cpustr, &blen, f);
		if (ret <= 0) {
			cpus_clear(new->mask);
		} else {
			cpumask_parse_user(cpustr, ret, new->mask);
			free(cpustr);
		}
	}
	fclose(f);
	/*填充节点域的其他信息*/
	new->obj_type = OBJ_TYPE_NODE;	
	new->number = strtoul(&nodename[4], NULL, 10);
	new->obj_type_list = &numa_nodes;
	/*将新建的节点域加入节点域链表中*/
	numa_nodes = g_list_append(numa_nodes, new);
}

/*建立一个NUMA域链表，如果系统支持NUMA，将所有的NUMA节点加入到链表中*/
void build_numa_node_list(void)
{
	DIR *dir;
	struct dirent *entry;

	/*利用模版结构创建一个NUMA节点域结构*/
	memcpy(&unspecified_node, &unspecified_node_template, sizeof (struct topo_obj));

	/*将该结构加入到NUMA域链表中，作为一个无实际意义的头结点 */
	numa_nodes = g_list_append(numa_nodes, &unspecified_node);

	/*查看是否支持NUMA架构，如果支持，将所有的节点加入到NUMA域链表中*/
	if (!numa_avail)
		return;

	dir = opendir(SYSFS_NODE_PATH);
	if (!dir)
		return;

	do {
		entry = readdir(dir);
		if (!entry)
			break;
		if ((entry->d_type == DT_DIR) && (strstr(entry->d_name, "node"))) {
			add_one_node(entry->d_name);
		}
	} while (entry);
	closedir(dir);
}

/*释放NUMA节点域空间，包括其子链表空间以及中断链表空间*/
static void free_numa_node(gpointer data)
{
	struct topo_obj *obj = data;
	g_list_free(obj->children);
	g_list_free(obj->interrupts);

	if (data != &unspecified_node)
		free(data);
}

/*释放整个节点域链表以及链表上的节点域空间*/
void free_numa_node_list(void)
{
	g_list_free_full(numa_nodes, free_numa_node);
	numa_nodes = NULL;
}

/*比较两个节点域是否一致，是则返回0*/
static gint compare_node(gconstpointer a, gconstpointer b)
{
	const struct topo_obj *ai = a;
	const struct topo_obj *bi = b;

	return (ai->number == bi->number) ? 0 : 1;
}

/*将package域插入指定节点域的子链表中*/
void add_package_to_node(struct topo_obj *p, int nodeid)
{
	struct topo_obj *node;
	/*根据节点编号找到指定NUMA节点*/
	node = get_numa_node(nodeid);

	if (!node) {
		log(TO_CONSOLE, LOG_INFO, "Could not find numa node for node id %d\n", nodeid);
		return;
	}
	/*如果该package域还没加入到任何NUMA节点域子链表中，将其插入到指定节点域的子链表中*/
	if (!p->parent) {
		node->children = g_list_append(node->children, p);
		p->parent = node;
	}
}

void dump_numa_node_info(struct topo_obj *d, void *unused __attribute__((unused)))
{
	char buffer[4096];

	log(TO_CONSOLE, LOG_INFO, "NUMA NODE NUMBER: %d\n", d->number);
	cpumask_scnprintf(buffer, 4096, d->mask); 
	log(TO_CONSOLE, LOG_INFO, "LOCAL CPU MASK: %s\n", buffer);
	log(TO_CONSOLE, LOG_INFO, "\n");
}

/*根据节点ID找到对应的NMUA节点域*/
struct topo_obj *get_numa_node(int nodeid)
{
	struct topo_obj find;
	GList *entry;

	if (!numa_avail)
		return &unspecified_node;

	if (nodeid == -1)
		return &unspecified_node;

	find.number = nodeid;

	entry = g_list_find_custom(numa_nodes, &find, compare_node);
	return entry ? entry->data : NULL;
}

