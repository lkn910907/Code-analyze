#include "nf10iface.h"
#include "nf10driver.h"
#include "nf10priv.h"

#include <linux/interrupt.h>
#include <linux/pci.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>

//网口的中断处理函数，与网口设备的中断号绑定
irqreturn_t int_handler(int irq, void *dev_id)
{
    struct pci_dev *pdev = dev_id;
/*在nf10_probe函数中使用函数platform_set_drvdata()将ndev保存成平台总线设备的私有数据。
使用它时只需调用platform_get_drvdata()就可以了*/
	struct nf10_card *card = (struct nf10_card*)pci_get_drvdata(pdev);

/*调度执行一个指定workqueue中的任务，第一个参数为指定的workqueue指针，第二个参数为具体的任务对象的指针 */
    queue_work(card->wq, (struct work_struct*)&card->work);

    return IRQ_HANDLED;   //表示中断处理完成，与之对应的是IRQ_NONE
}

static netdev_tx_t nf10i_tx(struct sk_buff *skb, struct net_device *dev)
{
//	通过netdev_priv访问到其私有数据， 即通过struct net_device *dev首地址加对齐后的偏移量就得到了私有数据的首地址
	struct nf10_card* card = ((struct nf10_ndev_priv*)netdev_priv(dev))->card;

//为了后面的包传输，先获取这个网络结构的端口号，需要注意的是网卡有4个
    int port = ((struct nf10_ndev_priv*)netdev_priv(dev))->port_num;

	//传输之前要先检查包大小是否符合要求
    // meet minimum size requirement
    if(skb->len < 60)
    {
    //当包大小达不到最小要求时，将不足的尾部补0，函数第一个参数为进行pad的skb地址，后面为pad的长度
        skb_pad(skb, 60 - skb->len);
	//skb_put() 增长数据区的长度来为memcpy准备空间. 许多的网络操作需要加入一些桢头
        skb_put(skb, 60 - skb->len);
    }

    if(skb->len > 1514)
    {
        printk(KERN_ERR "nf10: packet too big, dropping");
/*当包超过最大值时，释放sk_buff结构
Linux内核使用kfree_skb(),dev_kfree_skb()函数用于非中断上下文，
dev_kfree_skb_irq(),用于中断上下文。
dev_kfree_skb_any()表示中断与非中断皆可用*/
		dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    // transmit packet，对于大小符合要求的包，调用发送函数将数据包发送出去
    if(nf10priv_xmit(card, skb, port))
    {
	//如果返回非0，即表示发送出错
		printk(KERN_ERR "nf10: dropping packet at port %d", port);
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    // update stats，发送完包后，将对应网络结构发送包的数目以及发送的总字节数刷新
    card->ndev[port]->stats.tx_packets++;
    card->ndev[port]->stats.tx_bytes += skb->len;

    return NETDEV_TX_OK;
}

static int nf10i_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
    return 0;
}

static int nf10i_open(struct net_device *dev)
{
//函数netdev_priv获取网卡的私有数据地址，直接返回了net_device结构末端地址。因为priv成员紧跟在dev结构体后面，
//返回的也就是priv的首地址。
//将port开启判断变量置1表示该网络结构可以使用
	((struct nf10_ndev_priv*)netdev_priv(dev))->port_up = 1;
    return 0;
}

static int nf10i_stop(struct net_device *dev)
{
//将port开启判断变量清0表示该网络结构已被禁用了
	((struct nf10_ndev_priv*)netdev_priv(dev))->port_up = 0;
    return 0;
}

//为网络设备设置mac地址
static int nf10i_set_mac(struct net_device *dev, void *a)
{
/*
sockaddr其定义如下：
struct sockaddr {
　　unsigned short sa_family; 
　　char sa_data[14]; 
　　};
说明：sa_family ：是2字节的地址家族，一般都是“AF_xxx”的形式。通常用的都是AF_INET。
　　  sa_data ： 是14字节的协议地址。*/
	struct sockaddr *addr = (struct sockaddr *) a;

//判断地址是否合法
    if (!is_valid_ether_addr(addr->sa_data))
        return -EADDRNOTAVAIL;

//从源src所指的内存地址的起始位置开始拷贝n个字节到目标dest所指的内存地址的起始位置中
//函数原型void *memcpy(void *dest, const void *src, size_t n);
	memcpy(dev->dev_addr, addr->sa_data, ETH_ALEN);

    return 0;
}

static struct net_device_stats *nf10i_stats(struct net_device *dev)
{
//返回统计信息，如发送的包的数目以及发送的总的字节数目
	return &dev->stats;
}

//定义操作函数接口
static const struct net_device_ops nf10_ops =
{
    .ndo_open            = nf10i_open,
    .ndo_stop            = nf10i_stop,
    .ndo_do_ioctl        = nf10i_ioctl,
    .ndo_get_stats       = nf10i_stats,
    .ndo_start_xmit      = nf10i_tx,
    .ndo_set_mac_address = nf10i_set_mac
};

// init called by alloc_netdev，网络接口的初始化，为其指定操作结构、最大传输单元等
static void nf10iface_init(struct net_device *dev)
{
    ether_setup(dev); /* assign some of the fields */

    dev->netdev_ops      = &nf10_ops;
    dev->watchdog_timeo  = msecs_to_jiffies(5000);
    dev->mtu             = MTU;

}

//在使用网口之前需要先配置中断并初始化其4个网络设备结构
int nf10iface_probe(struct pci_dev *pdev, struct nf10_card *card)
{
    int ret = -ENODEV;
    int i;

    struct net_device *netdev;

    char *devname = "nf%d";

    // request IRQ，注册中断，并绑定中断处理函数
    if(request_irq(pdev->irq, int_handler, 0, DEVICE_NAME, pdev) != 0)
    {
        printk(KERN_ERR "nf10: request_irq failed\n");
        goto err_out_free_none;
    }

    // Set up the network device
    //建立4个网络设备结构，为其分配空间并初始化...
    for (i = 0; i < 4; i++)
    {
/*alloc_netdev()函数生成一个net_device结构体，对其成员赋值并返回该结构体的指针。
第一个参数是设备私有成员的大小，第二个参数为设备名，第三个参数为net_device的setup()函数指针�
setup()函数接收的参数为struct net_device指针，用于预置net_device成员的值。*/
		netdev = card->ndev[i] = alloc_netdev(sizeof(struct nf10_ndev_priv),
                                              devname, nf10iface_init);
        if(netdev == NULL)
        {
            printk(KERN_ERR "nf10: Could not allocate ethernet device.\n");
            ret = -ENOMEM;
            goto err_out_free_dev;
        }

		//所有的4个网络设备结构都共用物理设备的中断号
        netdev->irq = pdev->irq;

        ((struct nf10_ndev_priv*)netdev_priv(netdev))->card     = card;
		//不用的网络设备结构对应不用的port
        ((struct nf10_ndev_priv*)netdev_priv(netdev))->port_num = i;
        ((struct nf10_ndev_priv*)netdev_priv(netdev))->port_up  = 0;

        // assign some made up MAC adddr
        memcpy(netdev->dev_addr, "\0NF10C0", ETH_ALEN);
        netdev->dev_addr[ETH_ALEN - 1] = i;
		//注册网络设备结构
        if(register_netdev(netdev))
        {
            printk(KERN_ERR "nf10: register_netdev failed\n");
        }
	//用来告诉上层网络协定这个驱动程序有空的缓冲区可用，请把下一个封包送进来。
        netif_start_queue(netdev);
    }

    // give some descriptors to the card，分配指定数量的接收描述符
    for(i = 0; i < card->mem_rx_dsc.cl_size-2; i++)
    {
		nf10priv_send_rx_dsc(card);
    }

    // yay
    return 0;

    // fail
err_out_free_dev:
    for (i = 0; i < 4; i++)
    {
        if(card->ndev[i])
        {
            unregister_netdev(card->ndev[i]);
            free_netdev(card->ndev[i]);
        }
    }

err_out_free_none:
    return ret;
}

//移除网络接口时，要将4个网络设备结构全部释放
int nf10iface_remove(struct pci_dev *pdev, struct nf10_card *card)
{
    int i;

    for (i = 0; i < 4; i++)
    {
        if(card->ndev[i])
        {
            unregister_netdev(card->ndev[i]);
            free_netdev(card->ndev[i]);
        }
    }
    free_irq(pdev->irq, pdev);
    return 0;
}
