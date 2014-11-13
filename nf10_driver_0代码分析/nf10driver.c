#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/pci.h>
#include "nf10driver.h"
#include "nf10fops.h"
#include "nf10iface.h"

MODULE_LICENSE("GPL");//宏MODULE_LICENSE()用来声明模块的许可证
MODULE_AUTHOR("Mario Flajslik");//宏MODULE_AUTHOR()用来声明模块的作者
MODULE_DESCRIPTION("nf10 nic driver");//宏MODULE_DESCRIPTION()用来描述模块的用途

/* 指明该驱动程序适用于那种PCI设备 */
static struct pci_device_id pci_id[] =
{
    {PCI_DEVICE(PCI_VENDOR_ID_NF10,PCI_DEVICE_ID_NF10)},
    {0}
};
MODULE_DEVICE_TABLE(pci, pci_id);

// nf10_probe 函数的作用就是启动PCI设备，读取配置空间信息，进行相应的初始化
static int __devinit nf10_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int err;
    int i;
    int ret = -ENODEV;
    struct nf10_card *card;

    /* 启动PCI设备 */
    if((err = pci_enable_device(pdev)))
    {
        printk(KERN_ERR "nf10: Unable to enable the PCI device!\n");
        ret = -ENODEV;
        goto err_out_none;
    }

    // 设置DMA地址掩码 (full 64bit)
    if(dma_set_mask(&pdev->dev, DMA_BIT_MASK(64)) < 0)
    {
        printk(KERN_ERR "nf10: dma_set_mask fail!\n");
        ret = -EFAULT;
        goto err_out_disable_device;
    }

	//通知内核一致性内存分配的限制
    if(dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64)) < 0)
    {
        printk(KERN_ERR "nf10: dma_set_mask fail!\n");
        ret = -EFAULT;
        goto err_out_disable_device;
    }

    // enable BusMaster (enables generation of pcie requests)
    /* 设置成总线主DMA模式 */
    pci_set_master(pdev);

    // enable MSI
    /* 设置成MSI中断方式 */
    if(pci_enable_msi(pdev) != 0)
    {
        printk(KERN_ERR "nf10: failed to enable MSI interrupts\n");
        ret = -EFAULT;
        goto err_out_disable_device;
    }

    // be nice and tell kernel that we'll use this resource
    printk(KERN_INFO "nf10: Reserving memory region for NF10\n");


	//寄存器分为两个部分，第一个部分是通用寄存器,使用pci的第一个基址。
	//第二个部分是接受发送描述符寄存器，使用pci第三个基址。
	//发出申请的资源的请求，并检查资源是否可用，如果可用则申请成功，
	//并标志为已经使用，其他驱动如再申请该资源时就会失败
    if (!request_mem_region(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0), DEVICE_NAME))
    {
        //pci_resource_start(pdev, 0)返回第0个的基地址寄存器的基地址值
        //pci_resource_len(pdev, 0) 返回第0个的基地址寄存器的长度
        printk(KERN_ERR "nf10: Reserving memory region failed\n");
        ret = -ENOMEM;
        goto err_out_msi;
    }
    if (!request_mem_region(pci_resource_start(pdev, 2), pci_resource_len(pdev, 2), DEVICE_NAME))
    {
        //pci_resource_start(pdev, 2)返回第2个的基地址寄存器的基地址值
        //pci_resource_len(pdev, 2) 返回第2个的基地址寄存器的长度
        printk(KERN_ERR "nf10: Reserving memory region failed\n");
        ret = -ENOMEM;
        goto err_out_release_mem_region1;
    }

    // 创建私有结构变量，并为其分配空间资源
    card = (struct nf10_card*)kmalloc(sizeof(struct nf10_card), GFP_KERNEL);
    if (card == NULL)
    {
        printk(KERN_ERR "nf10: Private card memory alloc failed\n");
        ret = -ENOMEM;
        goto err_out_release_mem_region2;
    }
    memset(card, 0, sizeof(struct nf10_card));// 初始化card 空间全为0
    card->pdev = pdev;

    // map the cfg memory
    printk(KERN_INFO "nf10: mapping cfg memory\n");

    card->cfg_addr = ioremap_nocache(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
    //把以pci_resource_start(pdev, 0)为物理首地址的大小为pci_resource_len(pdev, 0)
    //的空间映射到CPU空间来进行访问（即通用寄存器从物理地址到CPU空间地址映射）
    if (!card->cfg_addr)
    {
        printk(KERN_ERR "nf10: cannot mem region len:%lx start:%lx\n",
               (long unsigned)pci_resource_len(pdev, 0),
               (long unsigned)pci_resource_start(pdev, 0));
        goto err_out_iounmap;
    }

    printk(KERN_INFO "nf10: mapping mem memory\n");

    //收发描述符的物理地址空间映射到CPU空间中
    card->tx_dsc = ioremap_nocache(pci_resource_start(pdev, 2) + 0 * 0x00100000ULL, 0x00100000ULL);
    card->rx_dsc = ioremap_nocache(pci_resource_start(pdev, 2) + 1 * 0x00100000ULL, 0x00100000ULL);

    if (!card->tx_dsc || !card->rx_dsc)
    {
        printk(KERN_ERR "nf10: cannot mem region len:%lx start:%lx\n",
               (long unsigned)pci_resource_len(pdev, 2),
               (long unsigned)pci_resource_start(pdev, 2));
        goto err_out_iounmap;
    }

    // reset
    *(((uint64_t*)card->cfg_addr)+30) = 1;//????
    msleep(1);

    // set buffer masks
    card->tx_dsc_mask = 0x000007ffULL;//11  "1"
    card->rx_dsc_mask = 0x000007ffULL;//11  "1"
    card->tx_pkt_mask = 0x00007fffULL;//15  "1"
    card->rx_pkt_mask = 0x00007fffULL;//15  "1"
    card->tx_dne_mask = 0x000007ffULL;//11  "1"
    card->rx_dne_mask = 0x000007ffULL;//11  "1"

    if(card->tx_dsc_mask > card->tx_dne_mask)
    {
        *(((uint64_t*)card->cfg_addr)+1) = card->tx_dne_mask;
        card->tx_dsc_mask = card->tx_dne_mask;
    }
    else if(card->tx_dne_mask > card->tx_dsc_mask)
    {
        *(((uint64_t*)card->cfg_addr)+7) = card->tx_dsc_mask;
        card->tx_dne_mask = card->tx_dsc_mask;
    }

    if(card->rx_dsc_mask > card->rx_dne_mask)
    {
        *(((uint64_t*)card->cfg_addr)+9) = card->rx_dne_mask;
        card->rx_dsc_mask = card->rx_dne_mask;
    }
    else if(card->rx_dne_mask > card->rx_dsc_mask)
    {
        *(((uint64_t*)card->cfg_addr)+15) = card->rx_dsc_mask;
        card->rx_dne_mask = card->rx_dsc_mask;
    }
    // 主机中PCI设备申请的发送/接收 DMA缓冲区，返回DMA缓冲区的虚拟地址给host_tx_dne_ptr,物理地址给host_tx_dne_dma
    /*	Linux内核提供了PCI设备申请DMA缓冲区的函数pci_alloc_consistent(),原型为：
		void *pci_alloc_consistent(struct pci_dev *dev, size_t size, dma_addr_t *dma_addrp);
       一致 DMA 映射 它们存在于驱动程序的生命周期内。一个被一致映射的缓冲区必须同时可被 CPU 和外围设备访问，
       这个缓冲区被处理器写时，可立即被设备读取而没有cache效应，反之亦然，使用函数pci_alloc_consistent建立一致映射。
	   当不再需要缓冲区时（通常在模块卸载时），应该调用函数 pci_free_consitent 将它返还给系统
	*/
    card->host_tx_dne_ptr = pci_alloc_consistent(pdev, card->tx_dne_mask+1, &(card->host_tx_dne_dma));
    card->host_rx_dne_ptr = pci_alloc_consistent(pdev, card->rx_dne_mask+1, &(card->host_rx_dne_dma));

    if( (card->host_rx_dne_ptr == NULL) ||
            (card->host_tx_dne_ptr == NULL) )
    {
        printk(KERN_ERR "nf10: cannot allocate dma buffer\n");
        goto err_out_free_private2;
    }

    // set host buffer addresses，将得到的DMA总线地址和掩码写入设备寄存器中，设备并不一定能在所有的内存地址上
    //执行DMA操作，因此需要DMA地址掩码
    *(((uint64_t*)card->cfg_addr)+16) = card->host_tx_dne_dma;
    *(((uint64_t*)card->cfg_addr)+17) = card->tx_dne_mask;
    *(((uint64_t*)card->cfg_addr)+18) = card->host_rx_dne_dma;
    *(((uint64_t*)card->cfg_addr)+19) = card->rx_dne_mask;

    // init mem buffers
    /*网卡的内存空间相关指针和计数器全部初始化为0*/
    card->mem_tx_dsc.wr_ptr = 0;
    card->mem_tx_dsc.rd_ptr = 0;
    atomic64_set(&card->mem_tx_dsc.cnt, 0);
    card->mem_tx_dsc.mask = card->tx_dsc_mask;
    card->mem_tx_dsc.cl_size = (card->tx_dsc_mask+1)/64;

    card->mem_tx_pkt.wr_ptr = 0;
    card->mem_tx_pkt.rd_ptr = 0;
    atomic64_set(&card->mem_tx_pkt.cnt, 0);
    card->mem_tx_pkt.mask = card->tx_pkt_mask;
    card->mem_tx_pkt.cl_size = (card->tx_pkt_mask+1)/64;

    card->mem_rx_dsc.wr_ptr = 0;
    card->mem_rx_dsc.rd_ptr = 0;
    atomic64_set(&card->mem_rx_dsc.cnt, 0);
    card->mem_rx_dsc.mask = card->rx_dsc_mask;
    card->mem_rx_dsc.cl_size = (card->rx_dsc_mask+1)/64;

    card->mem_rx_pkt.wr_ptr = 0;
    card->mem_rx_pkt.rd_ptr = 0;
    atomic64_set(&card->mem_rx_pkt.cnt, 0);
    card->mem_rx_pkt.mask = card->rx_pkt_mask;
    card->mem_rx_pkt.cl_size = (card->rx_pkt_mask+1)/64;

    card->host_tx_dne.wr_ptr = 0;
    card->host_tx_dne.rd_ptr = 0;
    atomic64_set(&card->host_tx_dne.cnt, 0);
    card->host_tx_dne.mask = card->tx_dne_mask;
    card->host_tx_dne.cl_size = (card->tx_dne_mask+1)/64;

    card->host_rx_dne.wr_ptr = 0;
    card->host_rx_dne.rd_ptr = 0;
    atomic64_set(&card->host_rx_dne.cnt, 0);
    card->host_rx_dne.mask = card->rx_dne_mask;
    card->host_rx_dne.cl_size = (card->rx_dne_mask+1)/64;

    for(i = 0; i < card->host_tx_dne.cl_size; i++)
        *(((uint32_t*)card->host_tx_dne_ptr) + i * 16) = 0xffffffff;

    for(i = 0; i < card->host_rx_dne.cl_size; i++)
        *(((uint64_t*)card->host_rx_dne_ptr) + i * 8 + 7) = 0xffffffffffffffffULL;

    // initialize work queue
    /* 用于创建一个workqueue队列，为系统中的每个CPU都创建一个内核线程。
    内核进程worker_thread做的事情很简单，死循环而已，不停的执行workqueue上的work_list */
    if(!(card->wq = create_workqueue("int_hndlr")))      //创建工作队列
    {
        printk(KERN_ERR "nf10: workqueue failed\n");
    }

	/*INIT_WORK会在定义的_work工作队列里面增加一个工作任务，该任务就是第二个参数
	  work就是一个工作队列，一般是结构体work_struct，主要的目的就是用来处理中断的。
	  比如在中断里面要做很多事，但是比较耗时，这时就可以把耗时的工作放到工作队列。*/
    INIT_WORK((struct work_struct*)&card->work, work_handler);//初始化工作队列
    card->work.card = card;

    // allocate book keeping structures
    card->tx_bk_skb = (struct sk_buff**)kmalloc(card->mem_tx_dsc.cl_size*sizeof(struct sk_buff*), GFP_KERNEL);
    card->tx_bk_dma_addr = (uint64_t*)kmalloc(card->mem_tx_dsc.cl_size*sizeof(uint64_t), GFP_KERNEL);
    card->tx_bk_size = (uint64_t*)kmalloc(card->mem_tx_dsc.cl_size*sizeof(uint64_t), GFP_KERNEL);
    card->tx_bk_port = (uint64_t*)kmalloc(card->mem_tx_dsc.cl_size*sizeof(uint64_t), GFP_KERNEL);

    card->rx_bk_skb = (struct sk_buff**)kmalloc(card->mem_rx_dsc.cl_size*sizeof(struct sk_buff*), GFP_KERNEL);
    card->rx_bk_dma_addr = (uint64_t*)kmalloc(card->mem_rx_dsc.cl_size*sizeof(uint64_t), GFP_KERNEL);
    card->rx_bk_size = (uint64_t*)kmalloc(card->mem_rx_dsc.cl_size*sizeof(uint64_t), GFP_KERNEL);

    if(card->tx_bk_skb == NULL || card->tx_bk_dma_addr == NULL || card->tx_bk_size == NULL || card->tx_bk_port == NULL ||
            card->rx_bk_skb == NULL || card->rx_bk_dma_addr == NULL || card->rx_bk_size == NULL)
    {
        printk(KERN_ERR "nf10: kmalloc failed");
        goto err_out_free_private2;
    }

    // store private data to pdev
    /*card是在probe函数中定义的局部变量，如果我想在其他地方使用它怎么办呢？ 这就需要把它保存起来。
    内核提供了这个方法，使用函数platform_set_drvdata()可以将ndev保存成平台总线设备的私有数据。以后
    再要使用它时只需调用platform_get_drvdata()就可以了*/
    pci_set_drvdata(pdev, card);

    // success
    //初始化nf10端口，并配置分配资源
    ret = nf10iface_probe(pdev, card);
    if(ret < 0)
    {
        printk(KERN_ERR "nf10: failed to initialize interfaces\n");
        goto err_out_free_private2;
    }
	//初始化设备文件及相关文件操作
    ret = nf10fops_probe(pdev, card);
    if(ret < 0)
    {
        printk(KERN_ERR "nf10: failed to initialize dev file\n");
        goto err_out_free_private2;
    }
    else
    {
        printk(KERN_INFO "nf10: device ready\n");
        return ret;
    }

// error out
err_out_free_private2:
    if(card->tx_bk_dma_addr) kfree(card->tx_bk_dma_addr);
    if(card->tx_bk_skb) kfree(card->tx_bk_skb);
    if(card->tx_bk_size) kfree(card->tx_bk_size);
    if(card->tx_bk_port) kfree(card->tx_bk_port);
    if(card->rx_bk_dma_addr) kfree(card->rx_bk_dma_addr);
    if(card->rx_bk_skb) kfree(card->rx_bk_skb);
    if(card->rx_bk_size) kfree(card->rx_bk_size);
    pci_free_consistent(pdev, card->tx_dne_mask+1, card->host_tx_dne_ptr, card->host_tx_dne_dma);
    pci_free_consistent(pdev, card->rx_dne_mask+1, card->host_rx_dne_ptr, card->host_rx_dne_dma);
err_out_iounmap:
    if(card->tx_dsc) iounmap(card->tx_dsc);
    if(card->rx_dsc) iounmap(card->rx_dsc);
    if(card->cfg_addr)   iounmap(card->cfg_addr);
    pci_set_drvdata(pdev, NULL);
    kfree(card);
err_out_release_mem_region2:
    release_mem_region(pci_resource_start(pdev, 2), pci_resource_len(pdev, 2));
err_out_release_mem_region1:
    release_mem_region(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
err_out_msi:
    pci_disable_msi(pdev);
err_out_disable_device:
    pci_disable_device(pdev);
err_out_none:
    return ret;
}

static void __devexit nf10_remove(struct pci_dev *pdev)
{
    struct nf10_card *card;

    // free private data
    printk(KERN_INFO "nf10: releasing private memory\n");
    card = (struct nf10_card*)pci_get_drvdata(pdev);
    if(card)
    {

        nf10fops_remove(pdev, card);
        nf10iface_remove(pdev, card);

        if(card->cfg_addr) iounmap(card->cfg_addr);

        if(card->tx_dsc) iounmap(card->tx_dsc);
        if(card->rx_dsc) iounmap(card->rx_dsc);

        pci_free_consistent(pdev, card->tx_dne_mask+1, card->host_tx_dne_ptr, card->host_tx_dne_dma);
        pci_free_consistent(pdev, card->rx_dne_mask+1, card->host_rx_dne_ptr, card->host_rx_dne_dma);

        if(card->tx_bk_dma_addr) kfree(card->tx_bk_dma_addr);
        if(card->tx_bk_skb) kfree(card->tx_bk_skb);
        if(card->tx_bk_size) kfree(card->tx_bk_size);
        if(card->tx_bk_port) kfree(card->tx_bk_port);
        if(card->rx_bk_dma_addr) kfree(card->rx_bk_dma_addr);
        if(card->rx_bk_skb) kfree(card->rx_bk_skb);
        if(card->rx_bk_size) kfree(card->rx_bk_size);

        kfree(card);
    }

    pci_set_drvdata(pdev, NULL);

    // release memory
    printk(KERN_INFO "nf10: releasing mem region\n");
    release_mem_region(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
    release_mem_region(pci_resource_start(pdev, 2), pci_resource_len(pdev, 2));

    // disabling device
    printk(KERN_INFO "nf10: disabling device\n");
    pci_disable_msi(pdev);
    pci_disable_device(pdev);
}

pci_ers_result_t nf10_pcie_error(struct pci_dev *dev, enum pci_channel_state state)
{
    printk(KERN_ALERT "nf10: PCIe error: %d\n", state);
    return PCI_ERS_RESULT_RECOVERED;
}

static struct pci_error_handlers pcie_err_handlers =
{
    .error_detected = nf10_pcie_error
};

static struct pci_driver pci_driver =
{
    .name = "nf10",
    .id_table = pci_id,
    .probe = nf10_probe,
    .remove = __devexit_p(nf10_remove),
    .err_handler = &pcie_err_handlers
};

static int __init nf10_init(void)
{
    printk(KERN_INFO "nf10: module loaded\n");
    return pci_register_driver(&pci_driver);
}

static void __exit nf10_exit(void)
{
    pci_unregister_driver(&pci_driver);
    printk(KERN_INFO "nf10: module unloaded\n");
}

module_init(nf10_init);
module_exit(nf10_exit);
