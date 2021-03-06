/* airport.c 0.13e
 *
 * A driver for "Hermes" chipset based Apple Airport wireless
 * card.
 *
 * Copyright notice & release notes in file orinoco.c
 * 
 * Note specific to airport stub:
 * 
 *  0.05 : first version of the new split driver
 *  0.06 : fix possible hang on powerup, add sleep support
 */

#include <linux/config.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/ioport.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/wireless.h>

#include <asm/io.h>
#include <asm/system.h>
#include <asm/current.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/pmac_feature.h>
#include <asm/irq.h>
#include <asm/uaccess.h>

#include "orinoco.h"

#define AIRPORT_IO_LEN	(0x1000)	/* one page */

struct airport {
	struct device_node *node;
	void *vaddr;
	int irq_requested;
	int ndev_registered;
};

static int
airport_suspend(struct macio_dev *mdev, u32 state)
{
	struct net_device *dev = dev_get_drvdata(&mdev->ofdev.dev);
	struct orinoco_private *priv = dev->priv;
	struct airport *card = priv->card;
	unsigned long flags;
	int err;

	printk(KERN_DEBUG "%s: Airport entering sleep mode\n", dev->name);

	err = orinoco_lock(priv, &flags);
	if (err) {
		printk(KERN_ERR "%s: hw_unavailable on PBOOK_SLEEP_NOW\n",
		       dev->name);
		return 0;
	}

	err = __orinoco_down(dev);
	if (err)
		printk(KERN_WARNING "%s: PBOOK_SLEEP_NOW: Error %d downing interface\n",
		       dev->name, err);

	netif_device_detach(dev);

	priv->hw_unavailable++;

	orinoco_unlock(priv, &flags);

	disable_irq(dev->irq);
	pmac_call_feature(PMAC_FTR_AIRPORT_ENABLE, card->node, 0, 0);

	return 0;
}

static int
airport_resume(struct macio_dev *mdev)
{
	struct net_device *dev = dev_get_drvdata(&mdev->ofdev.dev);
	struct orinoco_private *priv = dev->priv;
	struct airport *card = priv->card;
	unsigned long flags;
	int err;

	printk(KERN_DEBUG "%s: Airport waking up\n", dev->name);

	pmac_call_feature(PMAC_FTR_AIRPORT_ENABLE, card->node, 0, 1);
	mdelay(200);

	enable_irq(dev->irq);

	err = orinoco_reinit_firmware(dev);
	if (err) {
		printk(KERN_ERR "%s: Error %d re-initializing firmware on PBOOK_WAKE\n",
		       dev->name, err);
		return 0;
	}

	spin_lock_irqsave(&priv->lock, flags);

	netif_device_attach(dev);

	priv->hw_unavailable--;

	if (priv->open && (! priv->hw_unavailable)) {
		err = __orinoco_up(dev);
		if (err)
			printk(KERN_ERR "%s: Error %d restarting card on PBOOK_WAKE\n",
			       dev->name, err);
	}


	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int
airport_detach(struct macio_dev *mdev)
{
	struct net_device *dev = dev_get_drvdata(&mdev->ofdev.dev);
	struct orinoco_private *priv = dev->priv;
	struct airport *card = priv->card;

	if (card->ndev_registered)
		unregister_netdev(dev);
	card->ndev_registered = 0;

	if (card->irq_requested)
		free_irq(dev->irq, dev);
	card->irq_requested = 0;

	if (card->vaddr)
		iounmap(card->vaddr);
	card->vaddr = 0;

	dev->base_addr = 0;

	release_OF_resource(card->node, 0);

	pmac_call_feature(PMAC_FTR_AIRPORT_ENABLE, card->node, 0, 0);
	current->state = TASK_UNINTERRUPTIBLE;
	schedule_timeout(HZ);

	dev_set_drvdata(&mdev->ofdev.dev, NULL);
	free_netdev(dev);

	return 0;
}

static int airport_hard_reset(struct orinoco_private *priv)
{
	/* It would be nice to power cycle the Airport for a real hard
	 * reset, but for some reason although it appears to
	 * re-initialize properly, it falls in a screaming heap
	 * shortly afterwards. */
#if 0
	struct net_device *dev = priv->ndev;
	struct airport *card = priv->card;

	/* Vitally important.  If we don't do this it seems we get an
	 * interrupt somewhere during the power cycle, since
	 * hw_unavailable is already set it doesn't get ACKed, we get
	 * into an interrupt loop and the the PMU decides to turn us
	 * off. */
	disable_irq(dev->irq);

	pmac_call_feature(PMAC_FTR_AIRPORT_ENABLE, card->node, 0, 0);
	current->state = TASK_UNINTERRUPTIBLE;
	schedule_timeout(HZ);
	pmac_call_feature(PMAC_FTR_AIRPORT_ENABLE, card->node, 0, 1);
	current->state = TASK_UNINTERRUPTIBLE;
	schedule_timeout(HZ);

	enable_irq(dev->irq);
	schedule_timeout(HZ);
#endif

	return 0;
}

static int
airport_attach(struct macio_dev *mdev, const struct of_match *match)
{
	struct orinoco_private *priv;
	struct net_device *dev;
	struct airport *card;
	unsigned long phys_addr;
	struct device_node *of_node = mdev->ofdev.node;
	hermes_t *hw;

	if (of_node->n_addrs < 1 || of_node->n_intrs < 1) {
		printk(KERN_ERR "airport: wrong interrupt/addresses in OF tree\n");
		return -ENODEV;
	}

	/* Allocate space for private device-specific data */
	dev = alloc_orinocodev(sizeof(*card), airport_hard_reset);
	if (! dev) {
		printk(KERN_ERR "airport: can't allocate device datas\n");
		return -ENODEV;
	}
	priv = dev->priv;
	card = priv->card;

	hw = &priv->hw;
	card->node = of_node;

	if (! request_OF_resource(of_node, 0, " (airport)")) {
		printk(KERN_ERR "airport: can't request IO resource !\n");
		kfree(dev);
		return -ENODEV;
	}

	dev->name[0] = '\0';	/* register_netdev will give us an ethX name */
	SET_MODULE_OWNER(dev);
	SET_NETDEV_DEV(dev, &mdev->ofdev.dev);

	dev_set_drvdata(&mdev->ofdev.dev, dev);

	/* Setup interrupts & base address */
	dev->irq = of_node->intrs[0].line;
	phys_addr = of_node->addrs[0].address;  /* Physical address */
	printk(KERN_DEBUG "Airport at physical address %lx\n", phys_addr);
	dev->base_addr = phys_addr;
	card->vaddr = ioremap(phys_addr, AIRPORT_IO_LEN);
	if (! card->vaddr) {
		printk("airport: ioremap() failed\n");
		goto failed;
	}

	hermes_struct_init(hw, (ulong)card->vaddr,
			HERMES_MEM, HERMES_16BIT_REGSPACING);
		
	/* Power up card */
	pmac_call_feature(PMAC_FTR_AIRPORT_ENABLE, card->node, 0, 1);
	current->state = TASK_UNINTERRUPTIBLE;
	schedule_timeout(HZ);

	/* Reset it before we get the interrupt */
	hermes_init(hw);

	if (request_irq(dev->irq, orinoco_interrupt, 0, "Airport", dev)) {
		printk(KERN_ERR "airport: Couldn't get IRQ %d\n", dev->irq);
		goto failed;
	}
	card->irq_requested = 1;

	/* Tell the stack we exist */
	if (register_netdev(dev) != 0) {
		printk(KERN_ERR "airport: register_netdev() failed\n");
		goto failed;
	}
	printk(KERN_DEBUG "airport: card registered for interface %s\n", dev->name);
	card->ndev_registered = 1;
	return 0;
 failed:
	airport_detach(mdev);
	return -ENODEV;
}				/* airport_attach */


static char version[] __initdata = "airport.c 0.13e (Benjamin Herrenschmidt <benh@kernel.crashing.org>)";
MODULE_AUTHOR("Benjamin Herrenschmidt <benh@kernel.crashing.org>");
MODULE_DESCRIPTION("Driver for the Apple Airport wireless card.");
MODULE_LICENSE("Dual MPL/GPL");

static struct of_match airport_match[] = 
{
	{
	.name 		= "radio",
	.type		= OF_ANY_MATCH,
	.compatible	= OF_ANY_MATCH
	},
	{},
};

static struct macio_driver airport_driver = 
{
	.name 		= "airport",
	.match_table	= airport_match,
	.probe		= airport_attach,
	.remove		= airport_detach,
	.suspend	= airport_suspend,
	.resume		= airport_resume,
};

static int __init
init_airport(void)
{
	printk(KERN_DEBUG "%s\n", version);

	return macio_register_driver(&airport_driver);
}

static void __exit
exit_airport(void)
{
	return macio_unregister_driver(&airport_driver);
}

module_init(init_airport);
module_exit(exit_airport);
