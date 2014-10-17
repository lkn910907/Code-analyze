#include "config.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_GETOPT_LONG 
#include <getopt.h>
#endif

#ifdef HAVE_LIBCAP_NG
#include <cap-ng.h>
#endif
#include "irqbalance.h"

volatile int keep_going = 1;
int one_shot_mode;
int debug_mode;
int foreground_mode;
int numa_avail;
int need_rescan;
unsigned int log_mask = TO_ALL;
enum hp_e global_hint_policy = HINT_POLICY_IGNORE;
unsigned long power_thresh = ULONG_MAX;
unsigned long deepest_cache = 2;
unsigned long long cycle_count = 0;
char *pidfile = NULL;
char *banscript = NULL;
char *polscript = NULL;
long HZ;

/*暂停函数，只能获得微秒等级的精确度*/
static void sleep_approx(int seconds)
{
	struct timespec ts;	//有秒和纳秒部分
	struct timeval tv;	//有秒和微秒部分
	/*获取时间函数，精确到微秒级*/
	gettimeofday(&tv, NULL);

	/*tv_sec为秒数部分，tv_usec为微秒数部分*/
	ts.tv_sec = seconds;
	ts.tv_nsec = -tv.tv_usec*1000;
	while (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000;
	}
	//暂停进程，知道设定的时间结束
	nanosleep(&ts, NULL);
}

#ifdef HAVE_GETOPT_LONG
struct option lopts[] = {
	{"oneshot", 0, NULL, 'o'},
	{"debug", 0, NULL, 'd'},
	{"foreground", 0, NULL, 'f'},
	{"hintpolicy", 1, NULL, 'h'},
	{"powerthresh", 1, NULL, 'p'},
	{"banirq", 1 , NULL, 'i'},
	{"banscript", 1, NULL, 'b'},
	{"deepestcache", 1, NULL, 'c'},
	{"policyscript", 1, NULL, 'l'},
	{"pid", 1, NULL, 's'},
	{0, 0, 0, 0}
};

static void usage(void)
{
	log(TO_CONSOLE, LOG_INFO, "irqbalance [--oneshot | -o] [--debug | -d] [--foreground | -f] [--hintpolicy= | -h [exact|subset|ignore]]\n");
	log(TO_CONSOLE, LOG_INFO, "	[--powerthresh= | -p <off> | <n>] [--banirq= | -i <n>] [--policyscript=<script>] [--pid= | -s <file>] [--deepestcache= | -c <n>]\n");
}

/*解析命令*/
static void parse_command_line(int argc, char **argv)
{
	int opt;
	int longind;
	unsigned long val;

	while ((opt = getopt_long(argc, argv,
		"odfh:i:p:s:c:b:l:",
		lopts, &longind)) != -1) {

		switch(opt) {
			case '?':
				usage();
				exit(1);
				break;
			case 'b':
#ifndef INCLUDE_BANSCRIPT
				/*
				 * Banscript is no longer supported unless
				 * explicitly enabled
				 */
				log(TO_CONSOLE, LOG_INFO, "--banscript is not supported on this version of irqbalance, please use --polscript");
				usage();
				exit(1);
#endif
				banscript = strdup(optarg);
				break;
			case 'c':
				deepest_cache = strtoul(optarg, NULL, 10);
				if (deepest_cache == ULONG_MAX || deepest_cache < 1) {
					usage();
					exit(1);
				}
				break;
			case 'd':
				debug_mode=1;
				foreground_mode=1;
				break;
			case 'f':
				foreground_mode=1;
				break;
			case 'h':
				if (!strncmp(optarg, "exact", strlen(optarg)))
					global_hint_policy = HINT_POLICY_EXACT;
				else if (!strncmp(optarg, "subset", strlen(optarg)))
					global_hint_policy = HINT_POLICY_SUBSET;
				else if (!strncmp(optarg, "ignore", strlen(optarg)))
					global_hint_policy = HINT_POLICY_IGNORE;
				else {
					usage();
					exit(1);
				}
				break;
			case 'i':
				val = strtoull(optarg, NULL, 10);
				if (val == ULONG_MAX) {
					usage();
					exit(1);
				}
				add_cl_banned_irq((int)val);
				break;
			case 'l':
				polscript = strdup(optarg);
				break;
			case 'p':
				if (!strncmp(optarg, "off", strlen(optarg)))
					power_thresh = ULONG_MAX;
				else {
					power_thresh = strtoull(optarg, NULL, 10);
					if (power_thresh == ULONG_MAX) {
						usage();
						exit(1);
					}
				}
				break;
			case 'o':
				one_shot_mode=1;
				break;
			case 's':
				pidfile = optarg;
				break;
		}
	}
}
#endif

/* 构建拓扑结构树，从上往下，顶端为numa_nodes，向下以此为CPU packages、Cache domains以及CPU cores
一个域的负载为其子域负载的总和 */
static void build_object_tree(void)
{
	build_numa_node_list();
	parse_cpu_tree();
	rebuild_irq_db();
}

/*释放整个拓扑结构以及中断链表*/
static void free_object_tree(void)
{
	free_numa_node_list();
	clear_cpu_tree();
	free_irq_db();
}

/*打印拓扑结构信息*/
static void dump_object_tree(void)
{
	for_each_object(numa_nodes, dump_numa_node_info, NULL);
}

/*将中断加入到迁移中断链表中*/
static void force_rebalance_irq(struct irq_info *info, void *data __attribute__((unused)))
{
	if (info->level == BALANCE_NONE)
		return;

	if (info->assigned_obj == NULL)
		rebalance_irq_list = g_list_append(rebalance_irq_list, info);
	else
		migrate_irq(&info->assigned_obj->interrupts, &rebalance_irq_list, info);

	info->assigned_obj = NULL;
}

static void handler(int signum)
{
	(void)signum;
	keep_going = 0;
}

/*设定重新建立域拓扑结构的标志位*/
static void force_rescan(int signum)
{
	(void)signum;
	if (cycle_count)
		need_rescan = 1;
}

int main(int argc, char** argv)
{
	struct sigaction action, hupaction;
/*
struct sigaction {
	void (*sa_handler)(int); //信号处理函数
	void (*sa_sigaction)(int, siginfo_t *, void *);
	sigset_t sa_mask;  //用来设置在处理该信号时暂时将sa_mask 指定的信号集搁置
	int sa_flags;		//设置信号处理的其他相关操作
	void (*sa_restorer)(void);	暂未使用该参数
}
*/

#ifdef HAVE_GETOPT_LONG
	parse_command_line(argc, argv);
#else
/*irqbalance的运行模式*/
	if (argc>1 && strstr(argv[1],"--debug")) {
		debug_mode=1;
		foreground_mode=1;
	}
	if (argc>1 && strstr(argv[1],"--foreground"))
		foreground_mode=1;
	if (argc>1 && strstr(argv[1],"--oneshot"))
		one_shot_mode=1;
#endif

	/*
 	 * Open the syslog connection
 	 */
	openlog(argv[0], 0, LOG_DAEMON);

	if (getenv("IRQBALANCE_BANNED_CPUS"))  {
		cpumask_parse_user(getenv("IRQBALANCE_BANNED_CPUS"), strlen(getenv("IRQBALANCE_BANNED_CPUS")), banned_cpus);
	}

	if (getenv("IRQBALANCE_ONESHOT")) 
		one_shot_mode=1;

	if (getenv("IRQBALANCE_DEBUG")) 
		debug_mode=1;

	/*
 	 * If we are't in debug mode, don't dump anything to the console
 	 * note that everything goes to the console before we check this
 	 */
	if (!debug_mode)
		log_mask &= ~TO_CONSOLE;

	if (numa_available() > -1) {
		numa_avail = 1;
	} else 
		log(TO_CONSOLE, LOG_INFO, "This machine seems not NUMA capable.\n");

	if (banscript) {
		char *note = "Please note that --banscript is deprecated, please use --policyscript instead";
		log(TO_ALL, LOG_WARNING, "%s\n", note);
	}

	HZ = sysconf(_SC_CLK_TCK);
	if (HZ == -1) {
		log(TO_ALL, LOG_WARNING, "Unable to determin HZ defaulting to 100\n");
		HZ = 100;
	}

	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);	//清空此信号集
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);	//设置信号处理方式，SIGINT   ：来自键盘的中断信号 ( ctrl + c ) .

	/*构建整个域拓扑结构*/
	build_object_tree();
	/*在debug模式下打印出域拓扑结构信息*/
	if (debug_mode)
		dump_object_tree();


	/*对于只有一个CPU的系统，irqbalance没有意义*/
	if (core_count<2) {
		char *msg = "Balancing is ineffective on systems with a "
			    "single cpu.  Shutting down\n";

		log(TO_ALL, LOG_WARNING, "%s", msg);
		exit(EXIT_SUCCESS);
	}

	if (!foreground_mode) {
		int pidfd = -1;
		if (daemon(0,0))
			exit(EXIT_FAILURE);
		/* Write pidfile */
		if (pidfile && (pidfd = open(pidfile,
			O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) >= 0) {
			char str[16];
			snprintf(str, sizeof(str), "%u\n", getpid());
			write(pidfd, str, strlen(str));
			close(pidfd);
		}
	}


#ifdef HAVE_LIBCAP_NG
	// Drop capabilities
	capng_clear(CAPNG_SELECT_BOTH);
	capng_lock();
	capng_apply(CAPNG_SELECT_BOTH);
#endif

	for_each_irq(NULL, force_rebalance_irq, NULL);

	parse_proc_interrupts();
	parse_proc_stat();

	hupaction.sa_handler = force_rescan;	//设置该信号的处理函数
	sigemptyset(&hupaction.sa_mask);
	hupaction.sa_flags = 0;
	sigaction(SIGHUP, &hupaction, NULL);  // SIGHUP ：从终端上发出的结束信号.

	while (keep_going) {
		/*运行间隔为10s*/
		sleep_approx(SLEEP_INTERVAL);
		log(TO_CONSOLE, LOG_INFO, "\n\n\n-----------------------------------------------------------------------------\n");


		clear_work_stats();
		parse_proc_interrupts();
		parse_proc_stat();

		/* 如果发现需要重新建立拓扑结构，则重新执行一次初始化步骤 */
		if (need_rescan) {
			need_rescan = 0;
			cycle_count = 0;
			log(TO_CONSOLE, LOG_INFO, "Rescanning cpu topology \n");
			clear_work_stats();

			free_object_tree();
			build_object_tree();
			for_each_irq(NULL, force_rebalance_irq, NULL);
			parse_proc_interrupts();
			parse_proc_stat();
			sleep_approx(SLEEP_INTERVAL);
			clear_work_stats();
			parse_proc_interrupts();
			parse_proc_stat();
		} 

		if (cycle_count)	
			update_migration_status();

		calculate_placement();
		activate_mappings();
	
		if (debug_mode)
			dump_tree();
		if (one_shot_mode)
			keep_going = 0;
		cycle_count++;

	}
	free_object_tree();

	/* Remove pidfile */
	if (!foreground_mode && pidfile)
		unlink(pidfile);

	return EXIT_SUCCESS;
}
