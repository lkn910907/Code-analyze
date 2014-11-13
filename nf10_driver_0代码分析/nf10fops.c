#include "nf10fops.h"
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/sockios.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <asm/tsc.h>
#include <linux/interrupt.h>
#include <asm/irq.h>

static dev_t devno;
static struct class *dev_class;

static int axi_wr_cnt = 0;
static DEFINE_SPINLOCK(axi_lock);//自旋锁

//声明文件操作接口
static struct file_operations nf10_fops=
{
    .owner = THIS_MODULE,
    .open = nf10fops_open,
    .unlocked_ioctl = nf10fops_ioctl,
    .release = nf10fops_release
};


int nf10fops_open (struct inode *n, struct file *f)
{
    //container_of(ptr,type,member) 根据一个结构体变量(type)中的一个域成员变量(member)的
    //指针(ptr)来获取得指向整个结构体变量的指针
    struct nf10_card *card = (struct nf10_card *)container_of(n->i_cdev, struct nf10_card, cdev);
    f->private_data = card;
    return 0;
}


long nf10fops_ioctl (struct file *f, unsigned int cmd, unsigned long arg)
{
    struct nf10_card *card = (struct nf10_card *)f->private_data;
    uint64_t addr, val;
    unsigned long flags;

    switch(cmd)
    {
    case NF10_IOCTL_CMD_READ_STAT:
        //copy_from_user(void *to, const void __user *from, unsigned long n)
        //从用户空间拷贝数据到内核空间，
        //失败返回没有被拷贝的字节数，成功返回0。
        if(copy_from_user(&addr, (uint64_t*)arg, 8))
            printk(KERN_ERR "nf10: ioctl copy_from_user fail\n");
        if(addr >= 4096/8) return -EINVAL;
        val = *(((uint64_t*)card->cfg_addr) + 4096/8 + addr);
        if(copy_to_user((uint64_t*)arg, &val, 8))
            printk(KERN_ERR "nf10: ioctl copy_to_user fail\n");
        break;
    case NF10_IOCTL_CMD_WRITE_REG:
        // check for write buffer overflow
        spin_lock_irqsave(&axi_lock, flags);
        if(axi_wr_cnt < 64)
        {
            axi_wr_cnt++;
        }
        else
        {
            val = *(((uint64_t*)card->cfg_addr) + 130);
            if(val & 0x1)  // buffer empty
            {
                axi_wr_cnt = 1;
            }
            else if(~(val & 0x2))  // buffer not almost full
            {
                axi_wr_cnt = 49;
            }
            else if(~(val & 0x4))  // buffer not full
            {
                axi_wr_cnt = 64;
            }
            else  // buffer full
            {
                msleep(1);

                val = *(((uint64_t*)card->cfg_addr) + 130);
                if(val & 0x1)
                {
                    axi_wr_cnt = 1;
                }
                else if(~(val & 0x2))
                {
                    axi_wr_cnt = 49;
                }
                else if(~(val & 0x4))
                {
                    axi_wr_cnt = 64;
                }
                else
                {
                    axi_wr_cnt = 65;
                }
            }
        }
        spin_unlock_irqrestore(&axi_lock, flags);
        if(axi_wr_cnt > 64)
        {
            printk(KERN_ERR "nf10: AXI write buffer full\n");
            return -EFAULT;
        }

        // write reg
        *(((uint64_t*)card->cfg_addr) + 128) = (uint64_t)arg;

        break;
    case NF10_IOCTL_CMD_READ_REG:
        if(copy_from_user(&addr, (uint64_t*)arg, 8))
            printk(KERN_ERR "nf10: ioctl copy_from_user fail\n");
        *(((uint64_t*)card->cfg_addr) + 129) = (addr << 32);
        val = *(((uint64_t*)card->cfg_addr) + 129);
        if(copy_to_user((uint64_t*)arg, &val, 8))
            printk(KERN_ERR "nf10: ioctl copy_to_user fail\n");
        spin_lock_irqsave(&axi_lock, flags);
        axi_wr_cnt = 0;
        spin_unlock_irqrestore(&axi_lock, flags);
        break;
    default:
        printk(KERN_ERR "nf10: unknown ioctl\n");
        break;
    }

    return 0;
}

int nf10fops_release (struct inode *n, struct file *f)
{
    f->private_data = NULL;
    return 0;
}

int nf10fops_probe(struct pci_dev *pdev, struct nf10_card *card)
{
    int err;
/*在注册字符设备之前为其动态分配设备号，函数原型
Int alloc_chrdev_region(dev_t *dev,unsigned int firstminor,unsigned int count,char *name);
其中Firstminor : 通常为0; *dev:存放返回的设备号;*/ 
    err = alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    if (err)
    {
        printk(KERN_ERR "nf10: Error allocating chrdev\n");
        return err;
    }
    cdev_init(&card->cdev, &nf10_fops);
    card->cdev.owner = THIS_MODULE;
    card->cdev.ops = &nf10_fops;
//注册字符设备
    err = cdev_add(&card->cdev, devno, 1);
    if (err)
    {
        printk(KERN_ERR "nf10: Error adding /dev/nf10\n");
        return err;
    }
/*内核中定义了struct class结构体，顾名思义，一个struct class结构体类型变量对应一个类，内核同时提供了
class_create(…)函数，可以用它来创建一个类，这个类存放于sysfs下面，一旦创建好了这个类，
再调用device_create(…)函数来在/dev目录下创建相应的设备节点。这样，加载模块的时候，用户空间中的
udev会自动响应 device_create(…)函数，去/sysfs下寻找对应的类从而创建设备节点。*/
    dev_class = class_create(THIS_MODULE, DEVICE_NAME);
    device_create(dev_class, NULL, devno, NULL, DEVICE_NAME);
    return 0;
}

int nf10fops_remove(struct pci_dev *pdev, struct nf10_card *card)
{
    device_destroy(dev_class, devno);
    class_unregister(dev_class);
    class_destroy(dev_class);
    cdev_del(&card->cdev);
    unregister_chrdev_region(devno, 1);
    return 0;
}
