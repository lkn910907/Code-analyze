#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include "types.h"
#include "irqbalance.h"


GList *rebalance_irq_list;

/*最优迁移目标数据结构*/
struct obj_placement {
		struct topo_obj *best;	//负载最少的域
		struct topo_obj *least_irqs;	//负载最少并且中断数目也最少的域
		uint64_t best_cost;	 //负载最少的域的负载值
		struct irq_info *info;
};

/*判断域d是否适合作为迁移中断的最优目标选择*/
static void find_best_object(struct topo_obj *d, void *data)
{
	struct obj_placement *best = (struct obj_placement *)data;
	uint64_t newload;
	cpumask_t subset;

	/*不考虑无用的NUMA头结点 */
	if (numa_avail && (d->obj_type == OBJ_TYPE_NODE) && (d->number == -1))
		return;

	/*不考虑无可用CPU的NUMA节点 */
	if ((d->obj_type == OBJ_TYPE_NODE) &&
	    (!cpus_intersects(d->mask, unbanned_cpus)))
		return;

	/*保证该域满足中断的亲和度设置要求 */
	if (best->info->hint_policy == HINT_POLICY_SUBSET) {
		if (!cpus_empty(best->info->affinity_hint)) {
			cpus_and(subset, best->info->affinity_hint, d->mask);
			if (cpus_empty(subset))
				return;
		}
	}

	if (d->powersave_mode)
		return;

	newload = d->load;

	/*如果域d中负载值小于记录的最优值，将其作为最优域*/
	if (newload < best->best_cost) {
		best->best = d;
		best->best_cost = newload;
		best->least_irqs = NULL;
	}

	/*如果g域的负载与记录最优值一样并且中断数目更少，将其标记*/
	if (newload == best->best_cost) {
		if (g_list_length(d->interrupts) < g_list_length(best->best->interrupts))
			best->least_irqs = d;
	}
}

/*为需要找迁移目标的中断找最优域*/
static void find_best_object_for_irq(struct irq_info *info, void *data)
{
	struct obj_placement place;
	struct topo_obj *d = data;
	struct topo_obj *asign;

	if (!info->moved)
		return;
	/*从节点域开始找*/
	switch (d->obj_type) {
	case OBJ_TYPE_NODE:
		if (info->level == BALANCE_NONE)
			return;
		break;

	case OBJ_TYPE_PACKAGE:
		if (info->level == BALANCE_PACKAGE)
			return;
		break;

	case OBJ_TYPE_CACHE:
		if (info->level == BALANCE_CACHE)
			return;
		break;

	case OBJ_TYPE_CPU:
		if (info->level == BALANCE_CORE)
			return;
		break;
	}

	/*先初始化最优选择数据结构*/
	place.info = info;
	place.best = NULL;
	place.least_irqs = NULL;
	place.best_cost = ULLONG_MAX;

	/*遍历整个节点域和其子域，找到最优的子域后，信息保存到最优选择数据结构中*/
	for_each_object(d->children, find_best_object, &place);

	/*如果有负载最少并且中断数目也最少的，将其作为迁移目标，否则选择负载最少的最为迁移目标*/
	asign = place.least_irqs ? place.least_irqs : place.best;

	/*将中断迁移到目标域上，更新域的负载*/
	if (asign) {
		migrate_irq(&d->interrupts, &asign->interrupts, info);
		info->assigned_obj = asign;
		asign->load += info->load;
	}
}

/*为域d的中断找到最合适的子域*/
static void place_irq_in_object(struct topo_obj *d, void *data __attribute__((unused)))
{
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, find_best_object_for_irq, d);
}

/*将中断插入合适的域中*/
static void place_irq_in_node(struct irq_info *info, void *data __attribute__((unused)))
{
	struct obj_placement place;
	struct topo_obj *asign;

	if ((info->level == BALANCE_NONE) && cpus_empty(banned_cpus))
		return;
	/*如果该节点有归属的节点域，优先考虑其节点域是否是否合法*/
	if (irq_numa_node(info)->number != -1) 
	{
		/*如果该节点域不可用时，跳转去寻找最优的域，如果该节点域可用，则直接将中断插入节点域的中断链表中*/
		if (!cpus_intersects(irq_numa_node(info)->mask, unbanned_cpus))
			goto find_placement;

		migrate_irq(&rebalance_irq_list, &irq_numa_node(info)->interrupts, info);
		info->assigned_obj = irq_numa_node(info);
		irq_numa_node(info)->load += info->load + 1;
		return;
	}
	
/*通过最优数据结构以及遍历所有域的方式来寻找最优的域*/
find_placement:
	place.best_cost = ULLONG_MAX;
	place.best = NULL;
	place.least_irqs = NULL;
	place.info = info;

	for_each_object(numa_nodes, find_best_object, &place);

	asign = place.least_irqs ? place.least_irqs : place.best;

	if (asign) {
		migrate_irq(&rebalance_irq_list, &asign->interrupts, info);
		info->assigned_obj = asign;
		asign->load += info->load;
	}
}

/*检查中断归属域是否正确*/
static void validate_irq(struct irq_info *info, void *data)
{
	if (info->assigned_obj != data)
		log(TO_CONSOLE, LOG_INFO, "object validation error: irq %d is wrong, points to %p, should be %p\n",
			info->irq, info->assigned_obj, data);
}

/*检查该域中所有的中断是否确实属于该域*/
static void validate_object(struct topo_obj *d, void *data __attribute__((unused)))
{
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, validate_irq, d);
}

/*检查整个拓扑结构的中断归属是否正确*/
static void validate_object_tree_placement(void)
{
	for_each_object(packages, validate_object, NULL);	
	for_each_object(cache_domains, validate_object, NULL);
	for_each_object(cpus, validate_object, NULL);
}

/*全局的中断调整，将迁移的中断插入域中，并对整个拓扑结构优化中断的分配*/
void calculate_placement(void)
{
	sort_irq_list(&rebalance_irq_list);
	if (g_list_length(rebalance_irq_list) > 0) {
		for_each_irq(rebalance_irq_list, place_irq_in_node, NULL);
		for_each_object(numa_nodes, place_irq_in_object, NULL);
		for_each_object(packages, place_irq_in_object, NULL);
		for_each_object(cache_domains, place_irq_in_object, NULL);
	}
	if (debug_mode)
		validate_object_tree_placement();
}
