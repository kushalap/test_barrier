/*
 * GPIO interface for Intel Poulsbo SCH
 *
 *  Copyright (c) 2010 CompuLab Ltd
 *  Copyright (c) 2014-2015 Intel Corporation
 *  Author: Denis Turischev <denis@compulab.co.il>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/pci_ids.h>
#include <linux/uio_driver.h>

#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/bitmap.h>
#include <linux/types.h>


#define GEN	0x00
#define GIO	0x04
#define GLV	0x08
#define GTPE	0x0C
#define GTNE	0x10
#define GGPE	0x14
#define GSMI	0x18
#define GTS		0x1C

#define GNMIEN	0x40
#define RGNMIEN 0x44

/* Maximum number of resume GPIOS supported by this driver */
#define MAX_GPIO 64

/* Cache register context */
struct sch_gpio_context {
	/* Core well generic registers */
	u32 gen;
	u32 gio;
	u32 glvl;
	u32 gsmi;
	u32 gnmien;
	/* Core well interrupt trigger enable */
	u32 gtpe;
	u32 gtne;
	/* Resume well interrupt trigger enable */
	u32 rgtpe;
	u32 rgtne;
};

struct sch_gpio {
	int irq_num;
	int irq_desc_base;
	bool irq_support;
	DECLARE_BITMAP(wake_irqs, MAX_GPIO);

	struct uio_info info;
	struct sch_gpio_context context;
	struct gpio_chip chip;
	spinlock_t lock;
	unsigned short iobase;
	unsigned short core_base;
	unsigned short resume_base;
};

static void qrk_gpio_restrict_release(struct device *dev) {}
static struct platform_device qrk_gpio_restrict_pdev = {
	.name	= "qrk-gpio-restrict-nc",
	.dev.release = qrk_gpio_restrict_release,
};

#define to_sch_gpio(c)	container_of(c, struct sch_gpio, chip)
#define irq_to_gpio_number()	(d->irq - sch->irq_desc_base)

static unsigned sch_gpio_offset(struct sch_gpio *sch, unsigned gpio,
				unsigned reg)
{
	unsigned base = 0;

	if (gpio >= sch->resume_base) {
		gpio -= sch->resume_base;
		base += 0x20;
	}

	return base + reg + gpio / 8;
}

static unsigned sch_gpio_bit(struct sch_gpio *sch, unsigned gpio)
{
	if (gpio >= sch->resume_base)
		gpio -= sch->resume_base;
	return gpio % 8;
}

static int sch_gpio_reg_rdbit(struct sch_gpio *sch, unsigned gpio, unsigned reg)
{
	u8 curr_val;
	unsigned short offset, bit;

	offset = sch_gpio_offset(sch, gpio, reg);
	bit = sch_gpio_bit(sch, gpio);
	curr_val = inb(sch->iobase + offset) & BIT(bit);
	return(!!curr_val);
}

static void sch_gpio_reg_wrbit(struct sch_gpio *sch, unsigned gpio, unsigned reg, int val)
{
	u8 curr_val;
	unsigned short offset, bit;

	offset = sch_gpio_offset(sch, gpio, reg);
	bit = sch_gpio_bit(sch, gpio);
	curr_val = inb(sch->iobase + offset);
	if(val)
		outb(curr_val | BIT(bit), sch->iobase + offset);
	else
		outb(curr_val & ~BIT(bit), sch->iobase + offset);
	return;
}

static void sch_gpio_enable(struct sch_gpio *sch, unsigned gpio)
{
	spin_lock(&sch->lock);
	sch_gpio_reg_wrbit(sch, gpio, GEN, 1);
	spin_unlock(&sch->lock);
}

static int sch_gpio_direction_in(struct gpio_chip *gc, unsigned  gpio_num)
{
	struct sch_gpio *sch = to_sch_gpio(gc);

	spin_lock(&sch->lock);
	sch_gpio_reg_wrbit(sch, gpio_num, GIO, 1);
	spin_unlock(&sch->lock);
	return 0;
}

static int sch_gpio_get(struct gpio_chip *gc, unsigned gpio_num)
{
	struct sch_gpio *sch = to_sch_gpio(gc);
	int ret;

	ret = sch_gpio_reg_rdbit(sch, gpio_num, GLV);
	return ret;
}

static void sch_gpio_set(struct gpio_chip *gc, unsigned gpio_num, int val)
{
	struct sch_gpio *sch = to_sch_gpio(gc);

	spin_lock(&sch->lock);
	sch_gpio_reg_wrbit(sch, gpio_num, GLV, val);
	spin_unlock(&sch->lock);
}

static int sch_gpio_direction_out(struct gpio_chip *gc, unsigned gpio_num,
				  int val)
{
	struct sch_gpio *sch = to_sch_gpio(gc);

	spin_lock(&sch->lock);
	sch_gpio_reg_wrbit(sch, gpio_num, GIO, 0);
	/*
	 * according to the datasheet, writing to the level register has no
	 * effect when GPIO is programmed as input.
	 * Actually the the level register is read-only when configured as input.
	 * Thus presetting the output level before switching to output is _NOT_ possible.
	 * Hence we set the level after configuring the GPIO as output.
	 * But we cannot prevent a short low pulse if direction is set to high
	 * and an external pull-up is connected.
	 */
	sch_gpio_reg_wrbit(sch, gpio_num, GLV, val);
	spin_unlock(&sch->lock);
	return 0;
}


static void sch_gpio_irq_enable(struct irq_data *d)
{
	struct sch_gpio *sch = irq_data_get_irq_chip_data(d);
	u32 gpio_num;
	unsigned long flags;

	gpio_num = irq_to_gpio_number();

	spin_lock_irqsave(&sch->lock, flags);
	sch_gpio_reg_wrbit(sch,gpio_num,GGPE,1);
	spin_unlock_irqrestore(&sch->lock, flags);
}

static void sch_gpio_irq_disable(struct irq_data *d)
{
	struct sch_gpio *sch = irq_data_get_irq_chip_data(d);
	u32 gpio_num;
	unsigned long flags;

	gpio_num = irq_to_gpio_number();

	spin_lock_irqsave(&sch->lock, flags);
	sch_gpio_reg_wrbit(sch,gpio_num,GGPE,0);
	spin_unlock_irqrestore(&sch->lock, flags);
}

static void sch_gpio_irq_ack(struct irq_data *d)
{
	struct sch_gpio *sch = irq_data_get_irq_chip_data(d);
	u32 gpio_num;
	unsigned long flags;

	gpio_num = irq_to_gpio_number();

	spin_lock_irqsave(&sch->lock, flags);
	sch_gpio_reg_wrbit(sch,gpio_num,GTS,1);
	spin_unlock_irqrestore(&sch->lock, flags);
}

static int sch_gpio_irq_type(struct irq_data *d, unsigned type)
{
	struct sch_gpio *sch = irq_data_get_irq_chip_data(d);
	u32 gpio_num;
	unsigned long flags;

	gpio_num = irq_to_gpio_number();

	spin_lock_irqsave(&sch->lock, flags);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		sch_gpio_reg_wrbit(sch, gpio_num, GTPE, 1);
		sch_gpio_reg_wrbit(sch, gpio_num, GTNE, 0);
		break;

	case IRQ_TYPE_EDGE_FALLING:
		sch_gpio_reg_wrbit(sch, gpio_num, GTNE, 1);
		sch_gpio_reg_wrbit(sch, gpio_num, GTPE, 0);
		break;

	case IRQ_TYPE_EDGE_BOTH:
		sch_gpio_reg_wrbit(sch, gpio_num, GTPE, 1);
		sch_gpio_reg_wrbit(sch, gpio_num, GTNE, 1);
		break;

	case IRQ_TYPE_NONE:
		sch_gpio_reg_wrbit(sch, gpio_num, GTPE, 0);
		sch_gpio_reg_wrbit(sch, gpio_num, GTNE, 0);
		break;

	default:
		spin_unlock_irqrestore(&sch->lock, flags);
		return -EINVAL;
	}

	spin_unlock_irqrestore(&sch->lock, flags);

	return 0;
}

int sch_gpio_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct sch_gpio *sch = irq_data_get_irq_chip_data(d);
	u32 gpio_num = 0;
	int ret = 0;

	if(d == NULL){
		ret = -EFAULT;
		goto end;
	}

	gpio_num = irq_to_gpio_number();

	if (gpio_num >= MAX_GPIO) {
		ret = -EFAULT;
		goto end;
	}
	// only resume pins
	if (gpio_num < sch->resume_base) {
		ret = -EFAULT;
		goto end;
	}

	if (on)
		set_bit(gpio_num, sch->wake_irqs);
	else
		clear_bit(gpio_num, sch->wake_irqs);

end:
	return ret;
}


static irqreturn_t sch_gpio_irq_handler(int irq, void *dev_id)
{
	int ret = IRQ_NONE;

	struct sch_gpio *sch = dev_id;
	int i;

	for (i = 0; i < sch->chip.ngpio; i++) {
		if(sch_gpio_reg_rdbit(sch,i,GTS)){
			sch_gpio_reg_wrbit(sch,i,GTS,1);
			generic_handle_irq(sch->irq_desc_base + i);
			ret = IRQ_HANDLED;
		}
	}

	return ret;
}


static struct irq_chip sch_irq = {
	.name		= "gsi",
	.irq_ack		= sch_gpio_irq_ack,
	.irq_set_type	= sch_gpio_irq_type,
	.irq_enable		= sch_gpio_irq_enable,
	.irq_disable	= sch_gpio_irq_disable,
	.irq_set_wake	= sch_gpio_irq_set_wake,
};


static void sch_gpio_irq_disable_all(struct sch_gpio *sch)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&sch->lock, flags);

	outl(0,	sch->iobase + GTPE);
	outl(0,	sch->iobase + GTNE);
	outl(0,	sch->iobase + GGPE);
	outl(0,	sch->iobase + GSMI);
	outl(0,	sch->iobase + GNMIEN);

	outl(0,	sch->iobase + GTPE + 0x20);
	outl(0,	sch->iobase + GTNE + 0x20);
	outl(0,	sch->iobase + GGPE + 0x20);
	outl(0,	sch->iobase + GSMI + 0x20);
	outl(0,	sch->iobase + RGNMIEN);

	// clear any pending interrupt
	outl(0xFFFFFFFF, sch->iobase + GTS);
	outl(0xFFFFFFFF, sch->iobase + GTS + 0x20);

	spin_unlock_irqrestore(&sch->lock, flags);
}


static void sch_gpio_irqs_init(struct sch_gpio *sch)
{
	int i;

	for (i = 0; i < sch->chip.ngpio; i++) {
		irq_set_chip_data(i + sch->irq_desc_base, sch);
		irq_set_chip_and_handler_name(i + sch->irq_desc_base,
						&sch_irq,
						handle_edge_irq,
						"sch_gpio_irq");
	}
}

static void sch_gpio_irqs_deinit(struct sch_gpio *sch)
{
	int i;

	for (i = 0; i < sch->chip.ngpio; i++) {
		irq_set_chip_data(i + sch->irq_desc_base, 0);
		irq_set_chip_and_handler_name(i + sch->irq_desc_base, 0, 0, 0);
	}
}


static int sch_gpio_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct sch_gpio *sch = to_sch_gpio(gc);
	return sch->irq_desc_base + offset;
}


static struct gpio_chip sch_gpio_chip = {
	.label			= "sch_gpio",
	.owner			= THIS_MODULE,
	.direction_input	= sch_gpio_direction_in,
	.get			= sch_gpio_get,
	.direction_output	= sch_gpio_direction_out,
	.set			= sch_gpio_set,
	.to_irq			= sch_gpio_to_irq,
};

static int sch_gpio_findme(struct gpio_chip *chip, void *data)
{
	return !strcmp(chip->label, data);
}

static int sch_gpio_remove(struct platform_device *pdev)
{
	struct sch_gpio *sch = platform_get_drvdata(pdev);
	struct resource *res;
	int	ret = -ENODEV;

	if(sch){
		// free irq environment
		if(sch->irq_support){
			devm_free_irq(&pdev->dev, sch->irq_num, sch);
			sch_gpio_irqs_deinit(sch);
			irq_free_descs(sch->irq_desc_base, sch->chip.ngpio);
			sch->irq_num=0;
			sch->irq_desc_base=0;
			sch->irq_support = false;
		}

		// free drivers & data
		if(gpiochip_find("sch_gpio",sch_gpio_findme)){
			gpiochip_remove(&sch->chip);
		}
		if(sch->info.uio_dev)
			uio_unregister_device(&sch->info);
		platform_device_unregister(&qrk_gpio_restrict_pdev);
		res = platform_get_resource(pdev, IORESOURCE_IO, 0);
		if(res)
			devm_release_region(&pdev->dev, res->start, resource_size(res));
		devm_kfree(&pdev->dev, sch);
		ret=0;
	}
	return ret;
}

static int sch_gpio_probe(struct platform_device *pdev)
{
	struct sch_gpio *sch;
	struct resource *res, *res_irq;
	int ret;

	sch = devm_kzalloc(&pdev->dev, sizeof(*sch), GFP_KERNEL);
	if (!sch)
		return -ENOMEM;

	res_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res_irq) {
		sch->irq_num = res_irq->start;
		sch->irq_support = true;
	}else{
		sch->irq_num = 0;
		sch->irq_support = false;
	}
	sch->chip.label = dev_name(&pdev->dev);
	sch->chip = sch_gpio_chip;
	sch->chip.dev = &pdev->dev;
	spin_lock_init(&sch->lock);

	platform_set_drvdata(pdev, sch);

	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!res){
		ret = -EBUSY;
		goto err1;
	}

	if (!devm_request_region(&pdev->dev, res->start, resource_size(res),
				 pdev->name)) {
		ret = -EBUSY;
		goto err1;
	}

	sch->iobase = res->start;

	switch (pdev->id) {
	case PCI_DEVICE_ID_INTEL_SCH_LPC:
		sch->core_base = 0;
		sch->resume_base = 10;
		sch->chip.ngpio = 14;

		/*
		 * GPIO[6:0] enabled by default
		 * GPIO7 is configured by the CMC as SLPIOVR
		 * Enable GPIO[9:8] core powered gpios explicitly
		 */
		sch_gpio_enable(sch, 8);
		sch_gpio_enable(sch, 9);
		/*
		 * SUS_GPIO[2:0] enabled by default
		 * Enable SUS_GPIO3 resume powered gpio explicitly
		 */
		sch_gpio_enable(sch, 13);
		break;

	case PCI_DEVICE_ID_INTEL_ITC_LPC:
		sch->core_base = 0;
		sch->resume_base = 5;
		sch->chip.ngpio = 14;
		break;

	case PCI_DEVICE_ID_INTEL_CENTERTON_ILB:
		sch->core_base = 0;
		sch->resume_base = 21;
		sch->chip.ngpio = 30;
		break;

	case PCI_DEVICE_ID_INTEL_QUARK_X1000_ILB:
		sch->core_base = 0;
		sch->resume_base = 2;
		sch->chip.ngpio = 8;
		break;

	default:
		ret = -ENODEV;
		goto err1;
	}

	ret = platform_device_register(&qrk_gpio_restrict_pdev);
	if (ret < 0)
		goto err1;

	/* setup irq environment */
	if(sch->irq_support){
		sch->irq_desc_base = irq_alloc_descs(-1, 0, sch->chip.ngpio, NUMA_NO_NODE);
		if(sch->irq_desc_base < 0){
			ret = -ENODEV;
			goto err1;
		}

		sch_gpio_irq_disable_all(sch);

		ret = devm_request_irq(&pdev->dev, sch->irq_num, sch_gpio_irq_handler, IRQF_SHARED, KBUILD_MODNAME, sch);
		if(ret){
			goto err1;
		}

		sch_gpio_irqs_init(sch);
	}

	/* UIO */
	sch->info.port[0].name = "gpio_regs";
	sch->info.port[0].start = res->start;
	sch->info.port[0].size = resource_size(res);
	sch->info.port[0].porttype = UIO_PORT_X86;
	sch->info.name = "sch_gpio";
	sch->info.version = "0.0.1";

	ret = uio_register_device(&pdev->dev, &sch->info);
	if (ret)
		goto err1;

	return gpiochip_add(&sch->chip);
  err1:
	sch_gpio_remove(pdev);
	return ret;
}

/*
 * Disables IRQ line of Legacy GPIO chip so that its state is not controlled by
 * PM framework (disabled before calling suspend_noirq callback and re-enabled
 * after calling resume_noirq callback of devices).
 */
static int sch_gpio_suspend_sys(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct sch_gpio *sch = platform_get_drvdata(pdev);

	disable_irq(sch->irq_num);
	return 0;
}

/*
 * Saves the state of configuration registers for Core Well GPIOs.
 * Don't touch Core Well interrupt triggers and SCI/GPE because they are
 * handled by the irqchip subsystem.
 * Don't touch Suspend Well GPIO registers because they are alive and
 * functional in both S3 and S0 states.
 */
static int sch_gpio_suspend_sys_noirq(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct sch_gpio *sch = platform_get_drvdata(pdev);
	struct sch_gpio_context *regs = &sch->context;

	regs->gen	= inl(sch->iobase + GEN);
	regs->gio	= inl(sch->iobase + GIO);
	regs->glvl	= inl(sch->iobase + GLV);
	regs->gsmi	= inl(sch->iobase + GSMI);
	regs->gnmien = inl(sch->iobase + GNMIEN);

	return 0;
}

/*
 * Restore the context saved by sch_gpio_suspend_sys_noirq().
 */
static int sch_gpio_resume_sys_noirq(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct sch_gpio *sch = platform_get_drvdata(pdev);
	struct sch_gpio_context *regs = &sch->context;

	outl(regs->gen,		sch->iobase + GEN);
	outl(regs->gio,		sch->iobase + GIO);
	outl(regs->glvl,	sch->iobase + GLV);
	outl(regs->gsmi,	sch->iobase + GSMI);
	outl(regs->gnmien,	sch->iobase + GNMIEN);

	return 0;
}

/*
 * Re-enables the IRQ line of Legacy GPIO chip.
 * Done here instead of dpm_resume_no_irq() PM handler in order to be sure that
 * all the system busses (I2C, SPI) are resumed when the IRQ is fired, otherwise
 * a SPI or I2C device might fail to handle its own interrupt because the IRQ
 * handler (bottom half) involves talking to the device.
 */
static int sch_gpio_resume_sys(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct sch_gpio *sch = platform_get_drvdata(pdev);

	enable_irq(sch->irq_num);
	return 0;
}

const struct dev_pm_ops sch_gpio_pm_ops = {
	.suspend	= sch_gpio_suspend_sys,
	.suspend_noirq	= sch_gpio_suspend_sys_noirq,
	.resume_noirq	= sch_gpio_resume_sys_noirq,
	.resume		= sch_gpio_resume_sys,
};

static struct platform_driver sch_gpio_driver = {
	.driver = {
		.name = "sch_gpio",
		.owner	= THIS_MODULE,
		.pm	= &sch_gpio_pm_ops,
	},
	.probe		= sch_gpio_probe,
	.remove		= sch_gpio_remove,
};

module_platform_driver(sch_gpio_driver);

MODULE_AUTHOR("Denis Turischev <denis@compulab.co.il>");
MODULE_DESCRIPTION("GPIO interface for Intel Poulsbo SCH");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sch_gpio");
