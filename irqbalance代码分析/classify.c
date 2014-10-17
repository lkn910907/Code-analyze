#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <assert.h>

#include "irqbalance.h"
#include "types.h"


char *classes[] = {
	"other",
	"legacy",
	"storage",
	"video",
	"ethernet",
	"gbit-ethernet",
	"10gbit-ethernet",
	"virt-event",
	0
};

static int map_class_to_level[8] =
{ BALANCE_PACKAGE, BALANCE_CACHE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE };


#define MAX_CLASS 0x12
/*
 * Class codes lifted from pci spec, appendix D.
 * and mapped to irqbalance types here
 */
static short class_codes[MAX_CLASS] = {
	IRQ_OTHER,
	IRQ_SCSI,
	IRQ_ETH,
	IRQ_VIDEO,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_LEGACY,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_LEGACY,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_LEGACY,
	IRQ_ETH,
	IRQ_SCSI,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_OTHER,
};

struct user_irq_policy {
	int ban;
	int level;
	int numa_node_set;
	int numa_node;
	enum hp_e hintpolicy;
};

static GList *interrupts_db = NULL;
static GList *banned_irqs = NULL;
static GList *cl_banned_irqs = NULL;

#define SYSDEV_DIR "/sys/bus/pci/devices"

/*用于比较两个中断是否一致*/
static gint compare_ints(gconstpointer a, gconstpointer b)
{
	const struct irq_info *ai = a;
	const struct irq_info *bi = b;

	return ai->irq - bi->irq;
}

/*增加一个banned中断*/
static void add_banned_irq(int irq, GList **list)
{
	struct irq_info find, *new;
	GList *entry;

	find.irq = irq;
	/*遍历中断链表，如果发现该中断已存在，不添加，直接返回*/
	entry = g_list_find_custom(*list, &find, compare_ints);
	if (entry)
		return;
	/*是新的中断，分配空间、填充中断信息并加入中断链表中*/
	new = calloc(sizeof(struct irq_info), 1);
	if (!new) {
		log(TO_CONSOLE, LOG_WARNING, "No memory to ban irq %d\n", irq);
		return;
	}

	new->irq = irq;
	new->flags |= IRQ_FLAG_BANNED;
	new->hint_policy = HINT_POLICY_EXACT;

	*list = g_list_append(*list, new);
	return;
}

/*如果irq是新中断，为其分配数据结构并将其加入cl_banned_irqs链表中*/
void add_cl_banned_irq(int irq)
{
	add_banned_irq(irq, &cl_banned_irqs);
}

/*判断irq是否在banned_irqs链表上*/
static int is_banned_irq(int irq)
{
	GList *entry;
	struct irq_info find;

	find.irq = irq;

	entry = g_list_find_custom(banned_irqs, &find, compare_ints);
	return entry ? 1:0;
}

			
/*将irq加入到中断数据库链表中，并填充中断的信息，包括类型，亲和度策略，非一致性内存访问节点等信息，其中devpath为
文件系统中指向设备的路径*/
static struct irq_info *add_one_irq_to_db(const char *devpath, int irq, struct user_irq_policy *pol)
{
	int class = 0;
	int rc;
	struct irq_info *new, find;
	int numa_node;
	char path[PATH_MAX];
	FILE *fd;
	char *lcpu_mask;
	GList *entry;
	ssize_t ret;
	size_t blen;

	/*检查这个中断是否已存在，已存在则返回空 */
	find.irq = irq;
	entry = g_list_find_custom(interrupts_db, &find, compare_ints);
	if (entry) {
		log(TO_CONSOLE, LOG_INFO, "DROPPING DUPLICATE ENTRY FOR IRQ %d on path %s\n", irq, devpath);
		return NULL;
	}

	/*检查该中断是否是banned中断，是则返回空*/
	if (is_banned_irq(irq)) {
		log(TO_ALL, LOG_INFO, "SKIPPING BANNED IRQ %d\n", irq);
		return NULL;
	}

	/*分配中断信息空间并填充*/
	new = calloc(sizeof(struct irq_info), 1);
	if (!new)
		return NULL;

	new->irq = irq;
	new->class = IRQ_OTHER;
	new->hint_policy = pol->hintpolicy; 

	/*将该中断加入中断链表中*/
	interrupts_db = g_list_append(interrupts_db, new);

	sprintf(path, "%s/class", devpath);

	fd = fopen(path, "r");

	if (!fd) {
		perror("Can't open class file: ");
		goto get_numa_node;
	}

	rc = fscanf(fd, "%x", &class);
	fclose(fd);

	if (!rc)
		goto get_numa_node;

	/*设置非一致性内存访问节点和层次*/
	class >>= 16;

	if (class >= MAX_CLASS)
		goto get_numa_node;

	new->class = class_codes[class];
	if (pol->level >= 0)
		new->level = pol->level;
	else
		new->level = map_class_to_level[class_codes[class]];

get_numa_node:
	numa_node = -1;
	if (numa_avail) {
		sprintf(path, "%s/numa_node", devpath);
		fd = fopen(path, "r");
		if (fd) {
			rc = fscanf(fd, "%d", &numa_node);
			fclose(fd);
		}
	}

	if (pol->numa_node_set == 1)
		new->numa_node = get_numa_node(pol->numa_node);
	else
		new->numa_node = get_numa_node(numa_node);

	sprintf(path, "%s/local_cpus", devpath);
	fd = fopen(path, "r");
	if (!fd) {
		cpus_setall(new->cpumask);
		goto assign_affinity_hint;
	}
	lcpu_mask = NULL;
	ret = getline(&lcpu_mask, &blen, fd);
	fclose(fd);
	if (ret <= 0) {
		cpus_setall(new->cpumask);
	} else {
		cpumask_parse_user(lcpu_mask, ret, new->cpumask);
	}
	free(lcpu_mask);

/*设置irq的affinity_hint*/
assign_affinity_hint:
	cpus_clear(new->affinity_hint);
	sprintf(path, "/proc/irq/%d/affinity_hint", irq);
	fd = fopen(path, "r");
	if (!fd)
		goto out;
	lcpu_mask = NULL;
	ret = getline(&lcpu_mask, &blen, fd);
	fclose(fd);
	if (ret <= 0)
		goto out;
	/*将字符串转换成位图并赋值给affinity_hint*/
	cpumask_parse_user(lcpu_mask, ret, new->affinity_hint);
	free(lcpu_mask);
out:
	log(TO_CONSOLE, LOG_INFO, "Adding IRQ %d to database\n", irq);
	return new;
}

/*解析buf中的用户设置，保存到pol中*/
static void parse_user_policy_key(char *buf, int irq, struct user_irq_policy *pol)
{
	char *key, *value, *end;
	char *levelvals[] = { "none", "package", "cache", "core" };
	int idx;
	int key_set = 1;

	key = buf;
	/*返回首次出现'='的位置的指针*/
	value = strchr(buf, '=');

	if (!value) {
		log(TO_SYSLOG, LOG_WARNING, "Bad format for policy, ignoring: %s\n", buf);
		return;
	}

	/*终结这个buf字符串中出现等号之前的部分，即value之前 */
	*value = '\0';

	/*end为value字符串尾部，所以value指向‘=’与‘/n’之间的字符串*/
	value++;
	end = strchr(value, '\n');
	if (end)
		*end = '\0';

	/*strcasecmp用于忽略大小写比较字符串*/

	/*解析用户的禁止、非一致性节点以及亲和度策略设置*/
	if (!strcasecmp("ban", key)) {
		if (!strcasecmp("false", value))
			pol->ban = 0;
		else if (!strcasecmp("true", value))
			pol->ban = 1;
		else {
			key_set = 0;
			log(TO_ALL, LOG_WARNING, "Unknown value for ban poilcy: %s\n", value);
		}
	} else if (!strcasecmp("balance_level", key)) {
		for (idx=0; idx<4; idx++) {
			if (!strcasecmp(levelvals[idx], value))
				break;
		}

		if (idx>3) {
			key_set = 0;
			log(TO_ALL, LOG_WARNING, "Bad value for balance_level policy: %s\n", value);
		} else
			pol->level = idx;
	} else if (!strcasecmp("numa_node", key)) {
		idx = strtoul(value, NULL, 10);	
		if (!get_numa_node(idx)) {
			log(TO_ALL, LOG_WARNING, "NUMA node %d doesn't exist\n",
				idx);
			return;
		}
		pol->numa_node = idx;
		pol->numa_node_set = 1;
	} else if (!strcasecmp("hintpolicy", key)) {
		if (!strcasecmp("exact", value))
			pol->hintpolicy = HINT_POLICY_EXACT;
		else if (!strcasecmp("subset", value))
			pol->hintpolicy = HINT_POLICY_SUBSET;
		else if (!strcasecmp("ignore", value))
			pol->hintpolicy = HINT_POLICY_IGNORE;
		else {
			key_set = 0;
			log(TO_ALL, LOG_WARNING, "Unknown value for hitpolicy: %s\n", value);
		}
	} else {
		key_set = 0;
		log(TO_ALL, LOG_WARNING, "Unknown key returned, ignoring: %s\n", key);
	}

	if (key_set)
		log(TO_ALL, LOG_INFO, "IRQ %d: Override %s to %s\n", irq, key, value);

	
}

/*根据用户的策略脚本来设置中断策略 */
static void get_irq_user_policy(char *path, int irq, struct user_irq_policy *pol)
{
	char *cmd;
	FILE *output;
	char buffer[128];
	char *brc;

	/* 初始化数据结构pol全为-1，并设置亲和度策略为HINT_POLICY_IGNORE*/
	memset(pol, -1, sizeof(struct user_irq_policy));
	pol->hintpolicy = global_hint_policy;

	/* 如果没有设置策略脚本，直接返回 */
	if (!polscript)
		return;

	/*为操作命令分配空间并赋值*/
	cmd = alloca(strlen(path)+strlen(polscript)+64);
	if (!cmd)
		return;
	sprintf(cmd, "exec %s %s %d", polscript, path, irq);

    /*popen() 函数通过创建一个管道，调用fork 产生一个子进程，执行一个shell 以运行命令来开启一个进程。
    这个进程必须由 pclose() 函数关闭*/
	output = popen(cmd, "r");
	if (!output) {
		log(TO_ALL, LOG_WARNING, "Unable to execute user policy script %s\n", polscript);
		return;
	}

	while(!feof(output)) {
		/*从output中读取字符串，成功则返回buffer地址*/
		brc = fgets(buffer, 128, output);
		/* 根据读取的信息设置pol*/
		if (brc)
			parse_user_policy_key(brc, irq, pol);
	}
	pclose(output);
}

/*检查是否要ban一个irq，如果有ban策略或者已经被ban，返回1，否则返回0*/
static int check_for_irq_ban(char *path, int irq)
{
	char *cmd;
	int rc;
	struct irq_info find;
	GList *entry;

	/* 检查该中断是否已在cl_banned_irqs链表中 */
	find.irq = irq;
	entry = g_list_find_custom(cl_banned_irqs, &find, compare_ints);
	if (entry)
		return 1;

	/*没有ban的策略脚本*/
	if (!banscript)
		return 0;

	/*路径不合法*/
	if (!path)
		return 0;

	/*执行ban策略脚本*/
	cmd = alloca(strlen(path)+strlen(banscript)+32);
	if (!cmd)
		return 0;
	
	sprintf(cmd, "%s %s %d > /dev/null",banscript, path, irq);
	rc = system(cmd);

	/*
 	 * The system command itself failed
 	 */
	if (rc == -1) {
		log(TO_ALL, LOG_WARNING, "%s failed, please check the --banscript option\n", cmd);
		return 0;
	}

	if (WEXITSTATUS(rc)) {
		log(TO_ALL, LOG_INFO, "irq %d is baned by %s\n", irq, banscript);
		return 1;
	}
	return 0;

}

/*为该路径下的设备配置中断入口，包括msi-x以及int中断 */
static void build_one_dev_entry(const char *dirname)
{
	struct dirent *entry;
	DIR *msidir;
	FILE *fd;
	int irqnum;
	struct irq_info *new;
	char path[PATH_MAX];
	char devpath[PATH_MAX];
	struct user_irq_policy pol;

	sprintf(path, "%s/%s/msi_irqs", SYSDEV_DIR, dirname);
	sprintf(devpath, "%s/%s", SYSDEV_DIR, dirname);

/*如果是msi-x中断的话，遍历所有的中断向量入口，为那些没有设置中断策略的入口设置中断策略，并且加入中断链表中*/	
	msidir = opendir(path);
	if (msidir) {
		do {
			entry = readdir(msidir);
			if (!entry)
				break;
			/*strtol()会扫描参数d_name字符串，跳过前面的空格字符，直到遇上数字或正负符号才开始做转换，第三个参数为
10代表转换为十进制，再遇到非数字或字符串结束时('\0')结束转换，并将结果返回。*/
			irqnum = strtol(entry->d_name, NULL, 10);
			if (irqnum) {
				new = get_irq_info(irqnum);
				if (new)
					continue;
				get_irq_user_policy(devpath, irqnum, &pol);
				if ((pol.ban == 1) || (check_for_irq_ban(devpath, irqnum))) {
					add_banned_irq(irqnum, &banned_irqs);
					continue;
				}
				new = add_one_irq_to_db(devpath, irqnum, &pol);
				if (!new)
					continue;
				/*设置中断类型*/
				new->type = IRQ_TYPE_MSIX;
			}
		} while (entry != NULL);
		closedir(msidir);
		return;
	}

	sprintf(path, "%s/%s/irq", SYSDEV_DIR, dirname);
	fd = fopen(path, "r");
	if (!fd)
		return;
	if (fscanf(fd, "%d", &irqnum) < 0)
		goto done;

	/*对于传统中断而言，一个设备只有一个int中断号，如果该中断未设置中断策略，便设置并加入中断链表中 */
	if (irqnum) {
		new = get_irq_info(irqnum);
		if (new)
			goto done;
		get_irq_user_policy(devpath, irqnum, &pol);
		if ((pol.ban == 1) || (check_for_irq_ban(path, irqnum))) {
			add_banned_irq(irqnum, &banned_irqs);
			goto done;
		}

		new = add_one_irq_to_db(devpath, irqnum, &pol);
		if (!new)
			goto done;
		new->type = IRQ_TYPE_LEGACY;
	}

done:
	fclose(fd);
	return;
}

/*释放一个中断(中断信息结构) */
static void free_irq(struct irq_info *info, void *data __attribute__((unused)))
{
	free(info);
}

/*释放中断和中断链表*/
void free_irq_db(void)
{
	for_each_irq(NULL, free_irq, NULL);
	g_list_free(interrupts_db);
	interrupts_db = NULL;
	for_each_irq(banned_irqs, free_irq, NULL);
	g_list_free(banned_irqs);
	banned_irqs = NULL;
	g_list_free(rebalance_irq_list);
	rebalance_irq_list = NULL;
}

/*为一个新的中断设置中断信息，并加入中断链表*/
static void add_new_irq(int irq, struct irq_info *hint)
{
	struct irq_info *new;
	struct user_irq_policy pol;

	new = get_irq_info(irq);
	if (new)
		return;

	get_irq_user_policy("/sys", irq, &pol);
	if ((pol.ban == 1) || check_for_irq_ban(NULL, irq)) {
		add_banned_irq(irq, &banned_irqs);
		new = get_irq_info(irq);
	} else
		new = add_one_irq_to_db("/sys", irq, &pol);

	if (!new) {
		log(TO_CONSOLE, LOG_WARNING, "add_new_irq: Failed to add irq %d\n", irq);
		return;
	}

	/*
	 * Override some of the new irq defaults here
	 */
	if (hint) {
		new->type = hint->type;
		new->class = hint->class;
	}

	new->level = map_class_to_level[new->class];
}

/*为新中断设置中断信息，并加入链表中*/
static void add_missing_irq(struct irq_info *info, void *unused __attribute__((unused)))
{
	struct irq_info *lookup = get_irq_info(info->irq);

	if (!lookup)
		add_new_irq(info->irq, info);
	
}

/*为系统设备配置中断入口，并将中断加入中断链表中*/
void rebuild_irq_db(void)
{
	DIR *devdir;
	struct dirent *entry;
	GList *tmp_irqs = NULL;

	free_irq_db();

	/*获取系统中断链表*/
	tmp_irqs = collect_full_irq_list();

	devdir = opendir(SYSDEV_DIR);
	if (!devdir)
		goto free;

	do {
		entry = readdir(devdir);

		if (!entry)
			break;

		build_one_dev_entry(entry->d_name);

	} while (entry != NULL);

	closedir(devdir);


	for_each_irq(tmp_irqs, add_missing_irq, NULL);

free:
	g_list_free_full(tmp_irqs, free);

}

/*遍历中断链表，默认是遍历中断数据库链表*/
void for_each_irq(GList *list, void (*cb)(struct irq_info *info, void *data), void *data)
{
	GList *entry = g_list_first(list ? list : interrupts_db);
	GList *next;

	while (entry) {
		next = g_list_next(entry);
		cb(entry->data, data);
		entry = next;
	}
}

/*获取中断信息*/
struct irq_info *get_irq_info(int irq)
{
	GList *entry;
	struct irq_info find;

	find.irq = irq;
	entry = g_list_find_custom(interrupts_db, &find, compare_ints);

	if (!entry)
		entry = g_list_find_custom(banned_irqs, &find, compare_ints);

	return entry ? entry->data : NULL;
}


/*中断从一个链表迁移到另一个链表中，并标记为已迁移*/
void migrate_irq(GList **from, GList **to, struct irq_info *info)
{
	GList *entry;
	struct irq_info find, *tmp;

	find.irq = info->irq;
	entry = g_list_find_custom(*from, &find, compare_ints);

	if (!entry)
		return;

	tmp = entry->data;
	*from = g_list_delete_link(*from, entry);


	*to = g_list_append(*to, tmp);
	info->moved = 1;
}

/*中断是否正确排序*/
static gint sort_irqs(gconstpointer A, gconstpointer B)
{
        struct irq_info *a, *b;
        a = (struct irq_info*)A;
        b = (struct irq_info*)B;

	if (a->class < b->class || a->load < b->load || a < b)
		return 1;
        return -1;
}

/*检查中断链表是否按照正确方式排序，如果没有，则重新读链表排序*/
void sort_irq_list(GList **list)
{
	*list = g_list_sort(*list, sort_irqs);
}
