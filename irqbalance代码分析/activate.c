#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include "irqbalance.h"

/*检查irq_info的中断亲和度设置信息，判断applied_mask是否与其一致*/
static int check_affinity(struct irq_info *info, cpumask_t applied_mask)
{
	cpumask_t current_mask;
	char buf[PATH_MAX];
	char *line = NULL;
	size_t size = 0;
	FILE *file;
    /*buf保存该中断的亲和度目录路径*/
	sprintf(buf, "/proc/irq/%i/smp_affinity", info->irq);
	file = fopen(buf, "r");
	if (!file)
		return 1;
	/*getline用于读取一行字符直到换行符或者文件尾，成功时返回读取的字节数，line保存读取的字符串*/
	if (getline(&line, &size, file)==0) {
		free(line);
		fclose(file);
		return 1;
	}
	/*将得到的亲和度信息转换成位图信息*/
	cpumask_parse_user(line, strlen(line), current_mask);
	fclose(file);
	free(line);
	/*判断applied_mask是否与该中断的亲和度设置一致*/
	return cpus_equal(applied_mask, current_mask);
}

/*对进行了迁移的中断重新设置亲和度映射*/
static void activate_mapping(struct irq_info *info, void *data __attribute__((unused)))
{
	char buf[PATH_MAX];
	FILE *file;
	cpumask_t applied_mask;
	int valid_mask = 0;

	/*只有进行了迁移并且没有激活映射的中断需要进行映射激活*/
	if (!info->moved)
		return;
	
	/*如果用户设置的策略是HINT_POLICY_EXACT，那么会参照/proc/irq/N/affinity_hint设置亲和度,
如果是HINT_POLICY_SUBSET, 那么会参照/proc/irq/N/affinity_hint & applied_mask 设置*/
	if ((info->hint_policy == HINT_POLICY_EXACT) &&
	    (!cpus_empty(info->affinity_hint))) 
	{
	    
	    /*检查用户设置的亲和度位图与禁止迁移的CPU位图是否有交叉*/
		if (cpus_intersects(info->affinity_hint, banned_cpus))
			log(TO_ALL, LOG_WARNING,
			    "irq %d affinity_hint and banned cpus confict\n",
			    info->irq);
		else {
		/*亲和度位图不冲突，设置applied_mask，并表示位图可用*/
			applied_mask = info->affinity_hint;
			valid_mask = 1;
		}
	} 
	else if (info->assigned_obj) 
	{
		applied_mask = info->assigned_obj->mask;
		if ((info->hint_policy == HINT_POLICY_SUBSET) &&
		    (!cpus_empty(info->affinity_hint))) 
		{
			cpus_and(applied_mask, applied_mask, info->affinity_hint);
			if (!cpus_intersects(applied_mask, unbanned_cpus))
				log(TO_ALL, LOG_WARNING,
				    "irq %d affinity_hint subset empty\n",
				   info->irq);
			else
				valid_mask = 1;
		}
		else 
		{
			valid_mask = 1;
		}
	}

	/*如果亲和度设置不成功或者设置的亲和度位图与irq_info中原来保存的一致，直接返回*/
	if (!valid_mask || check_affinity(info, applied_mask))
		return;

	if (!info->assigned_obj)
		return;

	sprintf(buf, "/proc/irq/%i/smp_affinity", info->irq);
	file = fopen(buf, "w");
	if (!file)
		return;

	/*将设置好的亲和度位图保存到该中断的亲和度路径中*/
	cpumask_scnprintf(buf, PATH_MAX, applied_mask);
	fprintf(file, "%s", buf);
	fclose(file);

	/*迁移的中断已经完成了亲和度映射*/
	info->moved = 0;
}

/*遍历系统中断，对进行了迁移并且未重新设置亲和度信息的中断设置亲和度信息*/
void activate_mappings(void)
{
	for_each_irq(NULL, activate_mapping, NULL);
}
