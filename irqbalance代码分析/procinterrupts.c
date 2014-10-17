#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <ctype.h>

#include "cpumask.h"
#include "irqbalance.h"

#define LINESIZE 4096

static int proc_int_has_msi = 0;
static int msi_found_in_sysfs = 0;

/*遍历中断目录，获取中断信息，填充中断信息结构部分信息，加入中断链表中*/
GList* collect_full_irq_list()
{
	GList *tmp_list = NULL;
	FILE *file;
	char *line = NULL;
	size_t size = 0;
	char *irq_name, *savedptr, *last_token, *p;

	file = fopen("/proc/interrupts", "r");
	if (!file)
		return NULL;

	/*第一行是CPU编号，不需要管 */
	if (getline(&line, &size, file)==0) {
		free(line);
		fclose(file);
		return NULL;
	}

	while (!feof(file)) {
		int	 number;
		struct irq_info *info;
		char *c;
		char savedline[1024];

		if (getline(&line, &size, file)==0)
			break;

		/*开始读取中断号或者名称 */
		c = line;
		while (isblank(*(c)))
			c++;
		/*使用中断号信息的中断才需要管，并且该类中断排在前面，一旦出现中断名称标注的中断，例如NMI、LOC等直接忽略*/	
		if (!(*c>='0' && *c<='9'))
			break;

		/*冒号紧跟在中断号后面*/
		c = strchr(line, ':');
		if (!c)
			continue;

		/*拷贝中断信息，并将信息按内容进行分割*/
		strncpy(savedline, line, sizeof(savedline));
		irq_name = strtok_r(savedline, " ", &savedptr);
		last_token = strtok_r(NULL, " ", &savedptr);
		while ((p = strtok_r(NULL, " ", &savedptr))) {
			irq_name = last_token;
			last_token = p;
		}

		/*将中断号按十进制保存，将C赋值为0就将中断号与后面的信息分隔开了*/
		*c = 0;
		c++;
		number = strtoul(line, NULL, 10);

		/*分配中断信息结构，并填充中断号以及其他部分信息*/
		info = calloc(sizeof(struct irq_info), 1);
		if (info) {
			info->irq = number;
			if (strstr(irq_name, "xen-dyn-event") != NULL) {
				info->type = IRQ_TYPE_VIRT_EVENT;
				info->class = IRQ_VIRT_EVENT;
			} else {
				info->type = IRQ_TYPE_LEGACY;
				info->class = IRQ_OTHER;
			} 
			info->hint_policy = global_hint_policy;

		/*将识别出的中断加入链表中*/
			tmp_list = g_list_append(tmp_list, info);
		}

	}
	fclose(file);
	free(line);
	return tmp_list;
}

/*更新中断出发次数的信息，并检查系统中断是否正确*/
void parse_proc_interrupts(void)
{
	FILE *file;
	char *line = NULL;
	size_t size = 0;

	file = fopen("/proc/interrupts", "r");
	if (!file)
		return;

	/*第一行是CPU编号，不需要管 */
	if (getline(&line, &size, file)==0) {
		free(line);
		fclose(file);
		return;
	}

	while (!feof(file)) {
		int cpunr;
		int	 number;
		uint64_t count;
		char *c, *c2;
		struct irq_info *info;
		char savedline[1024];

		if (getline(&line, &size, file)==0)
			break;

		/*判断是否有msi中断*/
		if (!proc_int_has_msi)
			if (strstr(line, "MSI") != NULL)
				proc_int_has_msi = 1;

		/*使用中断号信息的中断才需要管，并且该类中断排在前面，一旦出现中断名称标注的中断，例如NMI、LOC等直接忽略*/	
		c = line;
		while (isblank(*(c)))
			c++;	
		if (!(*c>='0' && *c<='9'))
			break;

		/*冒号紧跟在中断号后面*/
		c = strchr(line, ':');
		if (!c)
			continue;

		strncpy(savedline, line, sizeof(savedline));

		*c = 0;
		c++;
		/*以十进制方式保存中断号信息*/
		number = strtoul(line, NULL, 10);

		/*通过中断号获取中断信息结构，如果得到空结构，说明需要重新解析中断信息*/
		info = get_irq_info(number);
		if (!info) {
			need_rescan = 1;
			break;
		}
		/*用于记录一个中断触发的总次数以及系统中的CPU数目*/
		count = 0;
		cpunr = 0;

		/*获取中断在每个CPU上触发的次数并累加*/
		c2=NULL;
		while (1) {
			uint64_t C;
			C = strtoull(c, &c2, 10);
			if (c==c2) /* end of numbers */
				break;
			count += C;
			c=c2;
			cpunr++;
		}

		/*统计得到的CPU域数目与CPU域链表中数目不匹配，此时应该重新建立域拓扑结构*/
		if (cpunr != core_count) {
			need_rescan = 1;
			break;
		}

		/* 因为中断移除和插入会出现的情况，此时应该重新建立域拓扑结构 */
		if (count < info->irq_count) {
			need_rescan = 1;
			break;
		}

		/*更新中断触发次数的信息*/
		info->last_irq_count = info->irq_count;		
		info->irq_count = count;

		/* 如果有MSI/MSI-X中断，进行标记*/
		if ((info->type == IRQ_TYPE_MSI) || (info->type == IRQ_TYPE_MSIX))
			msi_found_in_sysfs = 1;
	}		
	if ((proc_int_has_msi) && (!msi_found_in_sysfs) && (!need_rescan)) {
		log(TO_ALL, LOG_WARNING, "WARNING: MSI interrupts found in /proc/interrupts\n");
		log(TO_ALL, LOG_WARNING, "But none found in sysfs, you need to update your kernel\n");
		log(TO_ALL, LOG_WARNING, "Until then, IRQs will be improperly classified\n");
		/*
 		 * Set msi_foun_in_sysfs, so we don't get this error constantly
 		 */
		msi_found_in_sysfs = 1;
	}
	fclose(file);
	free(line);
}

/*计算在一个周期内。将该中断触发的负载作为域负载进行记录*/
static void accumulate_irq_count(struct irq_info *info, void *data)
{
	uint64_t *acc = data;

	*acc += (info->irq_count - info->last_irq_count);
}

/**/
static void assign_load_slice(struct irq_info *info, void *data)
{
	uint64_t *load_slice = data;
	info->load = (info->irq_count - info->last_irq_count) * *load_slice;

	/*每一个中断的负载都至少要为正数*/
	if (!info->load)
		info->load++;
}

/*
 * Recursive helper to estimate the number of irqs shared between 
 * multiple topology objects that was handled by this particular object
 */
static uint64_t get_parent_branch_irq_count_share(struct topo_obj *d)
{
	uint64_t total_irq_count = 0;

	if (d->parent) {
		total_irq_count = get_parent_branch_irq_count_share(d->parent);
		total_irq_count /= g_list_length((d->parent)->children);
	}

	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, accumulate_irq_count, &total_irq_count);

	return total_irq_count;
}

static void compute_irq_branch_load_share(struct topo_obj *d, void *data __attribute__((unused)))
{
	uint64_t local_irq_counts = 0;
	uint64_t load_slice;
	int	load_divisor = g_list_length(d->children);

	d->load /= (load_divisor ? load_divisor : 1);

	if (g_list_length(d->interrupts) > 0) 
	{
		local_irq_counts = get_parent_branch_irq_count_share(d);
		load_slice = local_irq_counts ? (d->load / local_irq_counts) : 1;
		for_each_irq(d->interrupts, assign_load_slice, &load_slice);
	}

	if (d->parent)
		d->parent->load += d->load;
}

/*对域负载进行清0*/
static void reset_load(struct topo_obj *d, void *data __attribute__((unused)))
{
	if (d->parent)
		reset_load(d->parent, NULL);

	d->load = 0;
}

void parse_proc_stat(void)
{
	FILE *file;
	char *line = NULL;
	size_t size = 0;
	int cpunr, rc, cpucount;
	struct topo_obj *cpu;
	unsigned long long irq_load, softirq_load;

/*该目录下有每一个CPU的负载记录*/
	file = fopen("/proc/stat", "r");
	if (!file) {
		log(TO_ALL, LOG_WARNING, "WARNING cant open /proc/stat.  balacing is broken\n");
		return;
	}

	/* 第一行是CPU负载统计和，不需要*/
	if (getline(&line, &size, file)==0) {
		free(line);
		log(TO_ALL, LOG_WARNING, "WARNING read /proc/stat. balancing is broken\n");
		fclose(file);
		return;
	}

	cpucount = 0;
	while (!feof(file)) {
		if (getline(&line, &size, file)==0)
			break;

		/*只要CPU的负载信息，后面的忽略*/
		if (!strstr(line, "cpu"))
			break;
		/*将CPU编号按十进制保存*/
		cpunr = strtoul(&line[3], NULL, 10);

		/*如果这个CPU已经在ban列表中，跳过该CPU*/
		if (cpu_isset(cpunr, banned_cpus))
			continue;

		/*获取CPU的中断和软中断负载*/
		rc = sscanf(line, "%*s %*u %*u %*u %*u %*u %llu %llu", &irq_load, &softirq_load);
		if (rc < 2)
			break;	

		/*获取CPU域结构*/
		cpu = find_cpu_core(cpunr);
		if (!cpu)
			break;

		/*CPU计数器*/
		cpucount++;

		/*
 		 * For each cpu add the irq and softirq load and propagate that
 		 * all the way up the device tree
 		 */
		if (cycle_count) {
			cpu->load = (irq_load + softirq_load) - (cpu->last_load);
			/*
			 * the [soft]irq_load values are in jiffies, with
			 * HZ jiffies per second.  Convert the load to nanoseconds
			 * to get a better integer resolution of nanoseconds per
			 * interrupt.
			 */
			cpu->load *= NSEC_PER_SEC/HZ;
		}
		cpu->last_load = (irq_load + softirq_load);
	}

	fclose(file);
	free(line);
	if (cpucount != get_cpu_count()) {
		log(TO_ALL, LOG_WARNING, "WARNING, didn't collect load info for all cpus, balancing is broken\n");
		return;
	}

	/*清除CPU域以上的结构域的负载值 */
	for_each_object(cache_domains, reset_load, NULL);

	/*
 	 * Now that we have load for each cpu attribute a fair share of the load
 	 * to each irq on that cpu
 	 */
	for_each_object(cpus, compute_irq_branch_load_share, NULL);
	for_each_object(cache_domains, compute_irq_branch_load_share, NULL);
	for_each_object(packages, compute_irq_branch_load_share, NULL);
	for_each_object(numa_nodes, compute_irq_branch_load_share, NULL);

}
