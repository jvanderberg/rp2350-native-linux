// SPDX-License-Identifier: GPL-2.0
/*
 * RP2350 Wi-Fi as Ethernet: eth0 over the frame rings in the I/O area
 * (rp2350-io.h). Core 1 runs the CYW43439 radio (boot/src/wifi.rs), joins the network stored in the Wi-Fi settings sector and
 * passes whole Ethernet frames; this driver is a plain Ethernet device on
 * top. Core 1 rings the doorbell (RPIO_BELL_NET) when it received a frame,
 * when the link changed and when a full transmit ring got room; Linux rings
 * core 1's when it queued a frame or freed a receive slot.
 */
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/delay.h>

#include "rp2350-io.h"

/* how long ifup may wait for the radio to come up after boot */
#define RADIO_WAIT_MS	10000

struct rpnet {
	void __iomem *io;
	struct net_device *dev;
	struct napi_struct napi;
};

static void rpnet_get_mac(struct rpnet *p, u8 *mac)
{
	u32 lo = readl(p->io + RPIO_NET_MAC_LO), hi = readl(p->io + RPIO_NET_MAC_HI);

	mac[0] = lo; mac[1] = lo >> 8; mac[2] = lo >> 16; mac[3] = lo >> 24;
	mac[4] = hi; mac[5] = hi >> 8;
}

static void rpnet_link(struct rpnet *p)
{
	bool up = readl(p->io + RPIO_NET_LINK);

	if (up && !netif_carrier_ok(p->dev)) {
		netif_carrier_on(p->dev);
		netdev_info(p->dev, "link up\n");
	} else if (!up && netif_carrier_ok(p->dev)) {
		netif_carrier_off(p->dev);
		netdev_info(p->dev, "link down\n");
	}
}

static bool rpnet_tx_room(struct rpnet *p)
{
	return readl(p->io + RPIO_NET_TX_TAIL) - readl(p->io + RPIO_NET_TX_HEAD) < RPIO_NET_SLOTS;
}

static int rpnet_poll(struct napi_struct *napi, int budget)
{
	struct rpnet *p = container_of(napi, struct rpnet, napi);
	struct net_device *dev = p->dev;
	int done = 0;

	while (done < budget) {
		u32 head = readl(p->io + RPIO_NET_RX_HEAD);
		void __iomem *slot;
		struct sk_buff *skb;
		u32 len;

		if (head == readl(p->io + RPIO_NET_RX_TAIL))
			break;
		rmb();
		slot = p->io + RPIO_NET_RX_RING + (head % RPIO_NET_SLOTS) * RPIO_NET_SLOT;
		len = min_t(u32, readl(slot), RPIO_NET_FRAME_MAX);
		skb = napi_alloc_skb(napi, len);
		if (skb)
			memcpy_fromio(skb_put(skb, len), slot + 4, len);
		mb();
		writel(head + 1, p->io + RPIO_NET_RX_HEAD);
		done++;
		if (!skb) {
			dev->stats.rx_dropped++;
			continue;
		}
		skb->protocol = eth_type_trans(skb, dev);
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += len;
		napi_gro_receive(napi, skb);
	}
	if (done)
		rpio_ring_core1(RPIO_BELL1_NET);	/* receive slots free */
	rpnet_link(p);
	if (netif_queue_stopped(dev) && rpnet_tx_room(p))
		netif_wake_queue(dev);
	if (done < budget && napi_complete_done(napi, done)) {
		/* a frame that came in after the last look, with its bell taken */
		if (readl(p->io + RPIO_NET_RX_HEAD) != readl(p->io + RPIO_NET_RX_TAIL))
			napi_schedule(napi);
	}
	return done;
}

static irqreturn_t rpnet_irq(int irq, void *data)
{
	struct rpnet *p = data;

	if (!rpio_take_bell(RPIO_BELL_NET))
		return IRQ_NONE;
	if (netif_running(p->dev))
		napi_schedule(&p->napi);
	return IRQ_HANDLED;
}

static netdev_tx_t rpnet_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rpnet *p = netdev_priv(dev);
	u32 tail = readl(p->io + RPIO_NET_TX_TAIL);
	void __iomem *slot;

	if (skb->len > RPIO_NET_FRAME_MAX) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (!rpnet_tx_room(p)) {
		netif_stop_queue(dev);
		writel(1, p->io + RPIO_NET_TX_WAKE);
		return NETDEV_TX_BUSY;
	}
	slot = p->io + RPIO_NET_TX_RING + (tail % RPIO_NET_SLOTS) * RPIO_NET_SLOT;
	writel(skb->len, slot);
	skb_copy_bits(skb, 0, (void __force *)(slot + 4), skb->len);
	wmb();
	writel(tail + 1, p->io + RPIO_NET_TX_TAIL);
	rpio_ring_core1(RPIO_BELL1_NET);
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;
	dev_consume_skb_any(skb);
	if (!rpnet_tx_room(p)) {
		netif_stop_queue(dev);
		writel(1, p->io + RPIO_NET_TX_WAKE);
		mb();
		/* core 1 may have taken frames before it saw the flag */
		if (rpnet_tx_room(p))
			netif_wake_queue(dev);
	}
	return NETDEV_TX_OK;
}

static int rpnet_open(struct net_device *dev)
{
	struct rpnet *p = netdev_priv(dev);
	unsigned long end = jiffies + msecs_to_jiffies(RADIO_WAIT_MS);
	u8 mac[ETH_ALEN];

	while (readl(p->io + RPIO_NET_RADIO) == RPIO_RADIO_STARTING && time_before(jiffies, end))
		msleep(50);
	switch (readl(p->io + RPIO_NET_RADIO)) {
	case RPIO_RADIO_UP:
		break;
	case RPIO_RADIO_ABSENT:
		netdev_err(dev, "the radio did not come up\n");
		return -ENODEV;
	default:
		netdev_err(dev, "the radio is still starting\n");
		return -EAGAIN;
	}
	rpnet_get_mac(p, mac);
	if (!ether_addr_equal(mac, dev->dev_addr))
		eth_hw_addr_set(dev, mac);
	netif_carrier_off(dev);
	napi_enable(&p->napi);
	netif_start_queue(dev);
	napi_schedule(&p->napi);	/* the link state, frames already waiting */
	return 0;
}

static int rpnet_stop(struct net_device *dev)
{
	struct rpnet *p = netdev_priv(dev);

	netif_stop_queue(dev);
	napi_disable(&p->napi);
	netif_carrier_off(dev);
	return 0;
}

static const struct net_device_ops rpnet_ops = {
	.ndo_open = rpnet_open,
	.ndo_stop = rpnet_stop,
	.ndo_start_xmit = rpnet_xmit,
	.ndo_validate_addr = eth_validate_addr,
};

static ssize_t rejoins_show(struct device *d, struct device_attribute *attr, char *buf)
{
	struct rpnet *p = netdev_priv(to_net_dev(d));

	return sysfs_emit(buf, "%u\n", readl(p->io + RPIO_NET_REJOINS));
}
static DEVICE_ATTR_RO(rejoins);

/* the radio's joins: succeeded, failed, rejoins after a lost link */
static ssize_t joins_show(struct device *d, struct device_attribute *attr, char *buf)
{
	struct rpnet *p = netdev_priv(to_net_dev(d));

	return sysfs_emit(buf, "%u %u %u\n", readl(p->io + RPIO_NET_JOINS),
			  readl(p->io + RPIO_NET_JOIN_FAILS), readl(p->io + RPIO_NET_REJOINS));
}
static DEVICE_ATTR_RO(joins);

/* writing 1 makes core 1 leave the network and join again (a test of
 * the link-loss path) */
static ssize_t rejoin_store(struct device *d, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct rpnet *p = netdev_priv(to_net_dev(d));

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	writel(RPIO_NET_CMD_REJOIN, p->io + RPIO_NET_CMD);
	rpio_ring_core1(RPIO_BELL1_NET);
	return count;
}
static DEVICE_ATTR_WO(rejoin);

/* core 1: loop passes, wakes by USB, PIO0, doorbell, tick; last join error;
 * gSPI transfers abandoned; SD bus time; Wi-Fi RX FIFO found full, words read */
static ssize_t core1_stats_show(struct device *d, struct device_attribute *attr, char *buf)
{
	struct rpnet *p = netdev_priv(to_net_dev(d));
	int i, n = 0;

	for (i = 0; i < RPIO_DBG_WORDS; i++)
		n += sysfs_emit_at(buf, n, "%u%c", readl(p->io + RPIO_DBG + 4 * i),
				   i == RPIO_DBG_WORDS - 1 ? '\n' : ' ');
	return n;
}
static DEVICE_ATTR_RO(core1_stats);

static struct attribute *rpnet_attrs[] = {
	&dev_attr_rejoins.attr, &dev_attr_joins.attr, &dev_attr_core1_stats.attr,
	&dev_attr_rejoin.attr, NULL
};
static const struct attribute_group rpnet_group = { .attrs = rpnet_attrs };

static int rpnet_probe(struct platform_device *pdev)
{
	struct net_device *dev;
	struct resource *res;
	struct rpnet *p;
	int irq, ret;
	u8 mac[ETH_ALEN];

	dev = devm_alloc_etherdev(&pdev->dev, sizeof(*p));
	if (!dev)
		return -ENOMEM;
	p = netdev_priv(dev);
	p->dev = dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	/* not requested: the I/O area is shared by the SD and flash drivers */
	p->io = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!p->io)
		return -ENOMEM;
	if (readl(p->io) != RPIO_MAGIC)
		return dev_err_probe(&pdev->dev, -ENODEV, "no I/O area (core 1 firmware too old?)\n");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	SET_NETDEV_DEV(dev, &pdev->dev);
	dev->netdev_ops = &rpnet_ops;
	dev->sysfs_groups[0] = &rpnet_group;
	dev->max_mtu = ETH_DATA_LEN;
	if (readl(p->io + RPIO_NET_RADIO) == RPIO_RADIO_UP) {
		rpnet_get_mac(p, mac);
		eth_hw_addr_set(dev, mac);
	} else {
		/* the real one is set when the interface comes up */
		eth_hw_addr_random(dev);
	}
	netif_napi_add(dev, &p->napi, rpnet_poll);
	netif_carrier_off(dev);
	ret = devm_request_irq(&pdev->dev, irq, rpnet_irq, IRQF_SHARED, "rp2350-net", p);
	if (ret)
		return ret;
	ret = devm_register_netdev(&pdev->dev, dev);
	if (ret)
		return ret;
	netdev_info(dev, "Wi-Fi via core 1, radio %s\n",
		    readl(p->io + RPIO_NET_RADIO) == RPIO_RADIO_UP ? "up" : "starting");
	return 0;
}

static const struct of_device_id rpnet_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-net" },
	{}
};

static struct platform_driver rpnet_driver = {
	.probe = rpnet_probe,
	.driver = {
		.name = "rp2350-net",
		.of_match_table = rpnet_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rpnet_driver);
