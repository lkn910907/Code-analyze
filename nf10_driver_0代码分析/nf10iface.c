#include "nf10iface.h"
#include "nf10driver.h"
#include "nf10priv.h"

#include <linux/interrupt.h>
#include <linux/pci.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>

//Íø¿ÚµÄÖĞ¶Ï´¦Àíº¯Êı£¬ÓëÍø¿ÚÉè±¸µÄÖĞ¶ÏºÅ°ó¶¨
irqreturn_t int_handler(int irq, void *dev_id)
{
    struct pci_dev *pdev = dev_id;
/*ÔÚnf10_probeº¯ÊıÖĞÊ¹ÓÃº¯Êıplatform_set_drvdata()½«ndev±£´æ³ÉÆ½Ì¨×ÜÏßÉè±¸µÄË½ÓĞÊı¾İ¡£
Ê¹ÓÃËüÊ±Ö»Ğèµ÷ÓÃplatform_get_drvdata()¾Í¿ÉÒÔÁË*/
	struct nf10_card *card = (struct nf10_card*)pci_get_drvdata(pdev);

/*µ÷¶ÈÖ´ĞĞÒ»¸öÖ¸¶¨workqueueÖĞµÄÈÎÎñ£¬µÚÒ»¸ö²ÎÊıÎªÖ¸¶¨µÄworkqueueÖ¸Õë£¬µÚ¶ş¸ö²ÎÊıÎª¾ßÌåµÄÈÎÎñ¶ÔÏóµÄÖ¸Õë */
    queue_work(card->wq, (struct work_struct*)&card->work);

    return IRQ_HANDLED;   //±íÊ¾ÖĞ¶Ï´¦ÀíÍê³É£¬ÓëÖ®¶ÔÓ¦µÄÊÇIRQ_NONE
}

static netdev_tx_t nf10i_tx(struct sk_buff *skb, struct net_device *dev)
{
//	Í¨¹ınetdev_priv·ÃÎÊµ½ÆäË½ÓĞÊı¾İ£¬ ¼´Í¨¹ıstruct net_device *devÊ×µØÖ·¼Ó¶ÔÆëºóµÄÆ«ÒÆÁ¿¾ÍµÃµ½ÁËË½ÓĞÊı¾İµÄÊ×µØÖ·
	struct nf10_card* card = ((struct nf10_ndev_priv*)netdev_priv(dev))->card;

//ÎªÁËºóÃæµÄ°ü´«Êä£¬ÏÈ»ñÈ¡Õâ¸öÍøÂç½á¹¹µÄ¶Ë¿ÚºÅ£¬ĞèÒª×¢ÒâµÄÊÇÍø¿¨ÓĞ4¸ö
    int port = ((struct nf10_ndev_priv*)netdev_priv(dev))->port_num;

	//´«ÊäÖ®Ç°ÒªÏÈ¼ì²é°ü´óĞ¡ÊÇ·ñ·ûºÏÒªÇó
    // meet minimum size requirement
    if(skb->len < 60)
    {
    //µ±°ü´óĞ¡´ï²»µ½×îĞ¡ÒªÇóÊ±£¬½«²»×ãµÄÎ²²¿²¹0£¬º¯ÊıµÚÒ»¸ö²ÎÊıÎª½øĞĞpadµÄskbµØÖ·£¬ºóÃæÎªpadµÄ³¤¶È
        skb_pad(skb, 60 - skb->len);
	//skb_put() Ôö³¤Êı¾İÇøµÄ³¤¶ÈÀ´Îªmemcpy×¼±¸¿Õ¼ä. Ğí¶àµÄÍøÂç²Ù×÷ĞèÒª¼ÓÈëÒ»Ğ©èåÍ·
        skb_put(skb, 60 - skb->len);
    }

    if(skb->len > 1514)
    {
        printk(KERN_ERR "nf10: packet too big, dropping");
/*µ±°ü³¬¹ı×î´óÖµÊ±£¬ÊÍ·Åsk_buff½á¹¹
LinuxÄÚºËÊ¹ÓÃkfree_skb(),dev_kfree_skb()º¯ÊıÓÃÓÚ·ÇÖĞ¶ÏÉÏÏÂÎÄ£¬
dev_kfree_skb_irq(),ÓÃÓÚÖĞ¶ÏÉÏÏÂÎÄ¡£
dev_kfree_skb_any()±íÊ¾ÖĞ¶ÏÓë·ÇÖĞ¶Ï½Ô¿ÉÓÃ*/
		dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    // transmit packet£¬¶ÔÓÚ´óĞ¡·ûºÏÒªÇóµÄ°ü£¬µ÷ÓÃ·¢ËÍº¯Êı½«Êı¾İ°ü·¢ËÍ³öÈ¥
    if(nf10priv_xmit(card, skb, port))
    {
	//Èç¹û·µ»Ø·Ç0£¬¼´±íÊ¾·¢ËÍ³ö´í
		printk(KERN_ERR "nf10: dropping packet at port %d", port);
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    // update stats£¬·¢ËÍÍê°üºó£¬½«¶ÔÓ¦ÍøÂç½á¹¹·¢ËÍ°üµÄÊıÄ¿ÒÔ¼°·¢ËÍµÄ×Ü×Ö½ÚÊıË¢ĞÂ
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
//º¯Êınetdev_priv»ñÈ¡Íø¿¨µÄË½ÓĞÊı¾İµØÖ·£¬Ö±½Ó·µ»ØÁËnet_device½á¹¹Ä©¶ËµØÖ·¡£ÒòÎªpriv³ÉÔ±½ô¸úÔÚdev½á¹¹ÌåºóÃæ£¬
//·µ»ØµÄÒ²¾ÍÊÇprivµÄÊ×µØÖ·¡£
//½«port¿ªÆôÅĞ¶Ï±äÁ¿ÖÃ1±íÊ¾¸ÃÍøÂç½á¹¹¿ÉÒÔÊ¹ÓÃ
	((struct nf10_ndev_priv*)netdev_priv(dev))->port_up = 1;
    return 0;
}

static int nf10i_stop(struct net_device *dev)
{
//½«port¿ªÆôÅĞ¶Ï±äÁ¿Çå0±íÊ¾¸ÃÍøÂç½á¹¹ÒÑ±»½ûÓÃÁË
	((struct nf10_ndev_priv*)netdev_priv(dev))->port_up = 0;
    return 0;
}

//ÎªÍøÂçÉè±¸ÉèÖÃmacµØÖ·
static int nf10i_set_mac(struct net_device *dev, void *a)
{
/*
sockaddrÆä¶¨ÒåÈçÏÂ£º
struct sockaddr {
¡¡¡¡unsigned short sa_family; 
¡¡¡¡char sa_data[14]; 
¡¡¡¡};
ËµÃ÷£ºsa_family £ºÊÇ2×Ö½ÚµÄµØÖ·¼Ò×å£¬Ò»°ã¶¼ÊÇ¡°AF_xxx¡±µÄĞÎÊ½¡£Í¨³£ÓÃµÄ¶¼ÊÇAF_INET¡£
¡¡¡¡  sa_data £º ÊÇ14×Ö½ÚµÄĞ­ÒéµØÖ·¡£*/
	struct sockaddr *addr = (struct sockaddr *) a;

//ÅĞ¶ÏµØÖ·ÊÇ·ñºÏ·¨
    if (!is_valid_ether_addr(addr->sa_data))
        return -EADDRNOTAVAIL;

//´ÓÔ´srcËùÖ¸µÄÄÚ´æµØÖ·µÄÆğÊ¼Î»ÖÃ¿ªÊ¼¿½±´n¸ö×Ö½Úµ½Ä¿±êdestËùÖ¸µÄÄÚ´æµØÖ·µÄÆğÊ¼Î»ÖÃÖĞ
//º¯ÊıÔ­ĞÍvoid *memcpy(void *dest, const void *src, size_t n);
	memcpy(dev->dev_addr, addr->sa_data, ETH_ALEN);

    return 0;
}

static struct net_device_stats *nf10i_stats(struct net_device *dev)
{
//·µ»ØÍ³¼ÆĞÅÏ¢£¬Èç·¢ËÍµÄ°üµÄÊıÄ¿ÒÔ¼°·¢ËÍµÄ×ÜµÄ×Ö½ÚÊıÄ¿
	return &dev->stats;
}

//¶¨Òå²Ù×÷º¯Êı½Ó¿Ú
static const struct net_device_ops nf10_ops =
{
    .ndo_open            = nf10i_open,
    .ndo_stop            = nf10i_stop,
    .ndo_do_ioctl        = nf10i_ioctl,
    .ndo_get_stats       = nf10i_stats,
    .ndo_start_xmit      = nf10i_tx,
    .ndo_set_mac_address = nf10i_set_mac
};

// init called by alloc_netdev£¬ÍøÂç½Ó¿ÚµÄ³õÊ¼»¯£¬ÎªÆäÖ¸¶¨²Ù×÷½á¹¹¡¢×î´ó´«Êäµ¥ÔªµÈ
static void nf10iface_init(struct net_device *dev)
{
    ether_setup(dev); /* assign some of the fields */

    dev->netdev_ops      = &nf10_ops;
    dev->watchdog_timeo  = msecs_to_jiffies(5000);
    dev->mtu             = MTU;

}

//ÔÚÊ¹ÓÃÍø¿ÚÖ®Ç°ĞèÒªÏÈÅäÖÃÖĞ¶Ï²¢³õÊ¼»¯Æä4¸öÍøÂçÉè±¸½á¹¹
int nf10iface_probe(struct pci_dev *pdev, struct nf10_card *card)
{
    int ret = -ENODEV;
    int i;

    struct net_device *netdev;

    char *devname = "nf%d";

    // request IRQ£¬×¢²áÖĞ¶Ï£¬²¢°ó¶¨ÖĞ¶Ï´¦Àíº¯Êı
    if(request_irq(pdev->irq, int_handler, 0, DEVICE_NAME, pdev) != 0)
    {
        printk(KERN_ERR "nf10: request_irq failed\n");
        goto err_out_free_none;
    }

    // Set up the network device
    //½¨Á¢4¸öÍøÂçÉè±¸½á¹¹£¬ÎªÆä·ÖÅä¿Õ¼ä²¢³õÊ¼»¯...
    for (i = 0; i < 4; i++)
    {
/*alloc_netdev()º¯ÊıÉú³ÉÒ»¸önet_device½á¹¹Ìå£¬¶ÔÆä³ÉÔ±¸³Öµ²¢·µ»Ø¸Ã½á¹¹ÌåµÄÖ¸Õë¡£
µÚÒ»¸ö²ÎÊıÊÇÉè±¸Ë½ÓĞ³ÉÔ±µÄ´óĞ¡£¬µÚ¶ş¸ö²ÎÊıÎªÉè±¸Ãû£¬µÚÈı¸ö²ÎÊıÎªnet_deviceµÄsetup()º¯ÊıÖ¸Õë¡
setup()º¯Êı½ÓÊÕµÄ²ÎÊıÎªstruct net_deviceÖ¸Õë£¬ÓÃÓÚÔ¤ÖÃnet_device³ÉÔ±µÄÖµ¡£*/
		netdev = card->ndev[i] = alloc_netdev(sizeof(struct nf10_ndev_priv),
                                              devname, nf10iface_init);
        if(netdev == NULL)
        {
            printk(KERN_ERR "nf10: Could not allocate ethernet device.\n");
            ret = -ENOMEM;
            goto err_out_free_dev;
        }

		//ËùÓĞµÄ4¸öÍøÂçÉè±¸½á¹¹¶¼¹²ÓÃÎïÀíÉè±¸µÄÖĞ¶ÏºÅ
        netdev->irq = pdev->irq;

        ((struct nf10_ndev_priv*)netdev_priv(netdev))->card     = card;
		//²»ÓÃµÄÍøÂçÉè±¸½á¹¹¶ÔÓ¦²»ÓÃµÄport
        ((struct nf10_ndev_priv*)netdev_priv(netdev))->port_num = i;
        ((struct nf10_ndev_priv*)netdev_priv(netdev))->port_up  = 0;

        // assign some made up MAC adddr
        memcpy(netdev->dev_addr, "\0NF10C0", ETH_ALEN);
        netdev->dev_addr[ETH_ALEN - 1] = i;
		//×¢²áÍøÂçÉè±¸½á¹¹
        if(register_netdev(netdev))
        {
            printk(KERN_ERR "nf10: register_netdev failed\n");
        }
	//ÓÃÀ´¸æËßÉÏ²ãÍøÂçĞ­¶¨Õâ¸öÇı¶¯³ÌĞòÓĞ¿ÕµÄ»º³åÇø¿ÉÓÃ£¬Çë°ÑÏÂÒ»¸ö·â°üËÍ½øÀ´¡£
        netif_start_queue(netdev);
    }

    // give some descriptors to the card£¬·ÖÅäÖ¸¶¨ÊıÁ¿µÄ½ÓÊÕÃèÊö·û
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

//ÒÆ³ıÍøÂç½Ó¿ÚÊ±£¬Òª½«4¸öÍøÂçÉè±¸½á¹¹È«²¿ÊÍ·Å
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
