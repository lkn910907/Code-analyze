#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>

#include "types.h"
#include "irqbalance.h"

struct load_balance_info {
	unsigned long long int total_load;  //系统总中断负载
	unsigned long long avg_load;	//系统平均中断负载
	unsigned long long min_load;	//系统中的中断负载最小值
	unsigned long long adjustment_load; //记录迁移中断的域的总负载
	int load_sources;	//负载域计数器
	unsigned long long int deviations;	//差值
	long double std_deviation;	
	unsigned int num_over;	//超过平均负载的域计数器
	unsigned int num_under;	//低于平均负载的域计数器
	unsigned int num_powersave;	//系统中节能模式的域计数器
	struct topo_obj *powersave;
};

/*更新负载均衡结构中的最小负载，总负载以及负载源数目*/
static void gather_load_stats(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;

	if (info->min_load == 0 || obj->load < info->min_load)
		info->min_load = obj->load;
	info->total_load += obj->load;
	info->load_sources += 1;
}

/*更新负载均衡结构中的负载差值信息*/
static void compute_deviations(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;
	unsigned long long int deviation;

	deviation = (obj->load > info->avg_load) ?
		obj->load - info->avg_load :
		info->avg_load - obj->load;

	info->deviations += (deviation * deviation);
}

/*判断这个中断是否符合迁移要求，如果满足则将其从原来的中断链表中删除*/
static void move_candidate_irqs(struct irq_info *info, void *data)
{
	struct load_balance_info *lb_info = data;

	/* never move an irq that has an afinity hint when 
 	 * hint_policy is HINT_POLICY_EXACT 
 	 */
	if (info->hint_policy == HINT_POLICY_EXACT)
		if (!cpus_empty(info->affinity_hint))
			return;

	/* Don't rebalance irqs that don't want it */
	if (info->level == BALANCE_NONE)
		return;

	/*如果这个中断所绑定的CPU只有这一个中断，不管这个中断带来的负载多重，都不迁移该中断 */
	if (g_list_length(info->assigned_obj->interrupts) <= 1)
		return;

	/* IRQs with a load of 1 have most likely not had any interrupts and
	 * aren't worth migrating
	 */
	if (info->load <= 1)
		return;

	/*迁移中断要保证中断负载不能超过调节负载与最小负载差值的一半 */
	if ((lb_info->adjustment_load - info->load) > (lb_info->min_load + info->load)) {
		lb_info->adjustment_load -= info->load;
		lb_info->min_load += info->load;
	} else
		return;

	log(TO_CONSOLE, LOG_INFO, "Selecting irq %d for rebalancing\n", info->irq);

	/*将中断从原来的中断链表中删除，加入rebalance_irq_list链表中*/
	migrate_irq(&info->assigned_obj->interrupts, &rebalance_irq_list, info);

	info->assigned_obj = NULL;
}

/*判断该域的负载情况，如果负载够重，将其中断排序后迁移，直到域负载不再需要进行中断迁移*/
static void migrate_overloaded_irqs(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;

	/*如果该域开启了节能模式，更新负载均衡结构中的节能域计数器*/
	if (obj->powersave_mode)
		info->num_powersave++;

	/*如果该域的负载明显小于平均负载，更新负载均衡结构中的低负载域计数器。
	如果该域的负载明显大于平均负载，更新负载均衡结构中的高负载域计数器*/
	if ((obj->load + info->std_deviation) <= info->avg_load) {
		info->num_under++;
		if (power_thresh != ULONG_MAX && !info->powersave)
			if (!obj->powersave_mode)
				info->powersave = obj;
	} else if ((obj->load - info->std_deviation) >=info->avg_load) {
		info->num_over++;
	}

	if ((obj->load > info->min_load) &&
	    (g_list_length(obj->interrupts) > 1)) {
		/* 将该域的中断链表按照负载从小到大排序 */
		sort_irq_list(&obj->interrupts);

		/*便利该域的中断链表，将该域的中断从链表中移除，直到该域的负载已经无法满足迁移的条件 */
		info->adjustment_load = obj->load;
		for_each_irq(obj->interrupts, move_candidate_irqs, info);
	}
}

/*无条件将中断从一个链表迁移出去*/
static void force_irq_migration(struct irq_info *info, void *data __attribute__((unused)))
{
	migrate_irq(&info->assigned_obj->interrupts, &rebalance_irq_list, info);
	info->assigned_obj = NULL;
}

/*取消一个域的节能模式*/
static void clear_powersave_mode(struct topo_obj *obj, void *data __attribute__((unused)))
{
	obj->powersave_mode = 0;
}

/*找到一个链表域中负载重的域，然后迁移其中断，直到负载已经不再需要迁移中断*/
static void find_overloaded_objs(GList *name, struct load_balance_info *info) 
{
	/*先将负载均衡结构清0，再根据链表域中各个域的情况来计算负载均衡数据结构中各个变量的值*/
	memset(info, 0, sizeof(struct load_balance_info));
	for_each_object(name, gather_load_stats, info);
	info->load_sources = (info->load_sources == 0) ? 1 : (info->load_sources);
	info->avg_load = info->total_load / info->load_sources;
	for_each_object(name, compute_deviations, info);
	/* Don't divide by zero if there is a single load source */
	if (info->load_sources == 1)
		info->std_deviation = 0;
	else {
		info->std_deviation = (long double)(info->deviations / (info->load_sources - 1));
		info->std_deviation = sqrt(info->std_deviation);
	}
	/*遍历链表域，根据域负载和负载均衡数据结构来迁移域中断*/
	for_each_object(name, migrate_overloaded_irqs, info);
}

/*从底层的CPU域开始进行负载的迁移，往上依次到cache域，package域，节点域，更新整个CPU拓扑结构的中断负载均衡状态*/
void update_migration_status(void)
{
	struct load_balance_info info;
	find_overloaded_objs(cpus, &info);
	if (power_thresh != ULONG_MAX && cycle_count > 5) {
		if (!info.num_over && (info.num_under >= power_thresh) && info.powersave) {
			log(TO_ALL, LOG_INFO, "cpu %d entering powersave mode\n", info.powersave->number);
			info.powersave->powersave_mode = 1;
			if (g_list_length(info.powersave->interrupts) > 0)
				for_each_irq(info.powersave->interrupts, force_irq_migration, NULL);
		} else if ((info.num_over) && (info.num_powersave)) {
			log(TO_ALL, LOG_INFO, "Load average increasing, re-enabling all cpus for irq balancing\n");
			for_each_object(cpus, clear_powersave_mode, NULL);
		}
	}
	find_overloaded_objs(cache_domains, &info);
	find_overloaded_objs(packages, &info);
	find_overloaded_objs(numa_nodes, &info);
}

static void dump_workload(struct irq_info *info, void *unused __attribute__((unused)))
{
	log(TO_CONSOLE, LOG_INFO, "Interrupt %i node_num %d (class %s) has workload %lu \n",
	    info->irq, irq_numa_node(info)->number, classes[info->class], (unsigned long)info->load);
}

void dump_workloads(void)
{
	for_each_irq(NULL, dump_workload, NULL);
}

