/*
 * FPC1020 Fingerprint sensor device driver
 *
 * This driver will control the platform resources that the FPC fingerprint
 * sensor needs to operate. The major things are probing the sensor to check
 * that it is actually connected and let the Kernel know this and with that also
 * enabling and disabling of regulators, enabling and disabling of platform
 * clocks, controlling GPIOs such as SPI chip select, sensor reset line, sensor
 * IRQ line, MISO and MOSI lines.
 *
 * The driver will expose most of its available functionality in sysfs which
 * enables dynamic control of these features from eg. a user space process.
 *
 * The sensor's IRQ events will be pushed to Kernel's event handling system and
 * are exposed in the drivers event node. This makes it possible for a user
 * space process to poll the input node and receive IRQ events easily. Usually
 * this node is available under /dev/input/eventX where 'X' is a number given by
 * the event system. A user space process will need to traverse all the event
 * nodes and ask for its parent's name (through EVIOCGNAME) which should match
 * the value in device tree named input-device-name.
 *
 * This driver will NOT send any SPI commands to the sensor it only controls the
 * electrical parts.
 *
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <soc/qcom/scm.h>

#include <linux/wakelock.h>
#define FPC1020_RESET_LOW_US 1000
#define FPC1020_RESET_HIGH1_US 100
#define FPC1020_RESET_HIGH2_US 1250
#define FPC_TTW_HOLD_TIME 1000
#define SET_PIPE_OWNERSHIP
#define ENABLE_WAKEUP
/*
 * Temporary work around that frees irq resource completely on suspend
 * Reacquire IRQ resource on resume. See below suspend/resume code
 * */
#define HW240_TEMP_WA
static const char * const pctl_names[] = {
	"fpc1020_spi_active",
	"fpc1020_reset_reset",
	"fpc1020_reset_active",
	"fpc1020_cs_low",
	"fpc1020_cs_high",
	"fpc1020_cs_active",
	"fpc1020_irq_active",
};

struct vreg_config {
	char *name;
	unsigned long vmin;
	unsigned long vmax;
	int ua_load;
};

static const struct vreg_config vreg_conf[] = {
	{ "vdd_ana", 1800000UL, 1800000UL, 6000, },
	{ "vcc_spi", 1800000UL, 1800000UL, 10, },
	{ "vdd_io", 1800000UL, 1800000UL, 6000, },
};

struct fpc1020_data {
	struct device *dev;
	struct spi_device *spi;
	struct pinctrl *fingerprint_pinctrl;
	struct pinctrl_state *pinctrl_state[ARRAY_SIZE(pctl_names)];
	struct clk *iface_clk;
	struct clk *core_clk;
	struct regulator *vreg[ARRAY_SIZE(vreg_conf)];

    struct wake_lock ttw_wl;
	int irq_gpio;
	int cs0_gpio;
	int cs1_gpio;
	int rst_gpio;
	int qup_id;
	struct mutex lock;
	bool prepared;
	bool wakeup_enabled;
	bool clocks_enabled;
	bool clocks_suspended;
};

static int vreg_setup(struct fpc1020_data *fpc1020, const char *name,
	bool enable)
{
	size_t i;
	int rc;
	struct regulator *vreg;
	struct device *dev = fpc1020->dev;

	for (i = 0; i < ARRAY_SIZE(fpc1020->vreg); i++) {
		const char *n = vreg_conf[i].name;
		if (!strncmp(n, name, strlen(n)))
			goto found;
	}
	dev_err(dev, "Regulator %s not found\n", name);
	return -EINVAL;
found:
	vreg = fpc1020->vreg[i];
	if (enable) {
		if (!vreg) {
			vreg = regulator_get(dev, name);
			if (!vreg) {
				dev_err(dev, "Unable to get  %s\n", name);
				return -ENODEV;
			}
		}
		if (regulator_count_voltages(vreg) > 0) {
			rc = regulator_set_voltage(vreg, vreg_conf[i].vmin,
					vreg_conf[i].vmax);
			if (rc)
				dev_err(dev,
					"Unable to set voltage on %s, %d\n",
					name, rc);
		}
		/*
		rc = regulator_set_optimum_mode(vreg, vreg_conf[i].ua_load);
		if (rc < 0)
			dev_err(dev, "Unable to set current on %s, %d\n",
					name, rc);
		*/
		rc = regulator_enable(vreg);
		if (rc) {
			dev_err(dev, "error enabling %s: %d\n", name, rc);
			regulator_put(vreg);
			vreg = NULL;
		}
		fpc1020->vreg[i] = vreg;
	} else {
		if (vreg) {
			if (regulator_is_enabled(vreg)) {
				regulator_disable(vreg);
				dev_dbg(dev, "disabled %s\n", name);
			}
			regulator_put(vreg);
			fpc1020->vreg[i] = NULL;
		}
		rc = 0;
	}
	return rc;
}

/**
 * Prepare or unprepare the SPI master that we are soon to transfer something
 * over SPI.
 *
 * Please see Linux Kernel manual for SPI master methods for more information.
 *
 * @see Linux SPI master methods
 */
static int spi_set_fabric(struct fpc1020_data *fpc1020, bool active)
{
	struct spi_master *master = fpc1020->spi->master;

	int rc = active ?
		master->prepare_transfer_hardware(master) :
		master->unprepare_transfer_hardware(master);
	if (rc)
		dev_err(fpc1020->dev, "%s: rc %d\n", __func__, rc);
	else
		dev_dbg(fpc1020->dev, "%s: %d ok\n", __func__, active);
	return rc;
}

/**
 * Changes ownership of SPI transfers from TEE to REE side or vice versa.
 *
 * SPI transfers can be owned only by one of TEE or REE side at any given time.
 * This can be changed dynamically if needed but of course that needs support
 * from underlaying layers. This function will transfer the ownership from REE
 * to TEE or vice versa.
 *
 * If REE side uses the SPI master when TEE owns the pipe or vice versa the
 * system will most likely crash dump.
 *
 * If available this should be set at boot time to eg. TEE side and not
 * dynamically as that will increase the security of the system. This however
 * implies that there are no other SPI slaves connected that should be handled
 * from REE side.
 *
 * @see SET_PIPE_OWNERSHIP
 */
static int set_pipe_ownership(struct fpc1020_data *fpc1020, bool to_tz)
{
	int rc;
	const u32 TZ_BLSP_MODIFY_OWNERSHIP_ID = 3;
	const u32 TZBSP_APSS_ID = 3;
	const u32 TZBSP_TZ_ID = 1;
	struct scm_desc desc = {
		.arginfo = SCM_ARGS(2),
		.args[0] = fpc1020->qup_id,
		.args[1] = to_tz ? TZBSP_TZ_ID : TZBSP_APSS_ID,
	};

	rc = scm_call2(SCM_SIP_FNID(SCM_SVC_TZ, TZ_BLSP_MODIFY_OWNERSHIP_ID),
		&desc);

	if (rc || desc.ret[0]) {
		dev_err(fpc1020->dev, "%s: scm_call2: responce %llu, rc %d\n",
				__func__, desc.ret[0], rc);
		return -EINVAL;
	}
	dev_dbg(fpc1020->dev, "%s: scm_call2: ok\n", __func__);
	return 0;
}

static int set_clks(struct fpc1020_data *fpc1020, bool enable)
{
	int rc = 0;
	mutex_lock(&fpc1020->lock);

	if (enable == fpc1020->clocks_enabled)
		goto out;

	if (enable) {
		rc = clk_prepare_enable(fpc1020->core_clk);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error enabling core clk: %d\n",
					__func__, rc);
			goto out;
		}

		rc = clk_prepare_enable(fpc1020->iface_clk);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error enabling iface clk: %d\n",
					__func__, rc);
			clk_disable_unprepare(fpc1020->core_clk);
			goto out;
		}
		dev_dbg(fpc1020->dev, "%s ok. clk rate %u hz\n", __func__,
				fpc1020->spi->max_speed_hz);

		fpc1020->clocks_enabled = true;
	} else {
		clk_disable_unprepare(fpc1020->iface_clk);
		clk_disable_unprepare(fpc1020->core_clk);
		fpc1020->clocks_enabled = false;
	}

out:
	mutex_unlock(&fpc1020->lock);
	return rc;
}


static ssize_t clk_enable_set(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	return set_clks(fpc1020, (*buf == '1')) ? : count;
}

static DEVICE_ATTR(clk_enable, S_IWUSR, NULL, clk_enable_set);

/**
 * Will try to select the set of pins (GPIOS) defined in a pin control node of
 * the device tree named @p name.
 *
 * The node can contain several eg. GPIOs that is controlled when selecting it.
 * The node may activate or deactivate the pins it contains, the action is
 * defined in the device tree node itself and not here. The states used
 * internally is fetched at probe time.
 *
 * @see pctl_names
 * @see fpc1020_probe
 */
static int select_pin_ctl(struct fpc1020_data *fpc1020, const char *name)
{
	size_t i;
	int rc;
	struct device *dev = fpc1020->dev;
	for (i = 0; i < ARRAY_SIZE(fpc1020->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		if (!strncmp(n, name, strlen(n))) {
			rc = pinctrl_select_state(fpc1020->fingerprint_pinctrl,
					fpc1020->pinctrl_state[i]);
			if (rc)
				dev_err(dev, "cannot select '%s'\n", name);
			else
				dev_dbg(dev, "Selected '%s'\n", name);
			goto exit;
		}
	}
	rc = -EINVAL;
	dev_err(dev, "%s:'%s' not found\n", __func__, name);
exit:
	return rc;
}

/**
 * sysfs node handler to support dynamic change of SPI transfers' ownership
 * between TEE and REE side.
 *
 * An owner in this context is REE or TEE.
 *
 * @see set_pipe_ownership
 * @see SET_PIPE_OWNERSHIP
 */
static ssize_t spi_owner_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc;
	bool to_tz;

	if (!strncmp(buf, "tz", strlen("tz")))
		to_tz = true;
	else if (!strncmp(buf, "app", strlen("app")))
		to_tz = false;
	else
		return -EINVAL;

	rc = set_pipe_ownership(fpc1020, to_tz);
	return rc ? rc : count;
}
static DEVICE_ATTR(spi_owner, S_IWUSR, NULL, spi_owner_set);

static ssize_t pinctl_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc = select_pin_ctl(fpc1020, buf);
	return rc ? rc : count;
}
static DEVICE_ATTR(pinctl_set, S_IWUSR, NULL, pinctl_set);

/**
 * Will indicate to the SPI driver that a message is soon to be delivered over
 * it.
 *
 * Exactly what fabric resources are requested is up to the SPI device driver.
 *
 * @see spi_set_fabric
 */
static ssize_t fabric_vote_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc = spi_set_fabric(fpc1020, *buf == '1');
	return rc ? rc : count;
}
static DEVICE_ATTR(fabric_vote, S_IWUSR, NULL, fabric_vote_set);

static ssize_t regulator_enable_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	char op;
	char name[16];
	int rc;
	bool enable;

	if (2 != sscanf(buf, "%15s,%c", name, &op))
		return -EINVAL;
	if (op == 'e')
		enable = true;
	else if (op == 'd')
		enable = false;
	else
		return -EINVAL;
	rc = vreg_setup(fpc1020, name, enable);
	return rc ? rc : count;
}
static DEVICE_ATTR(regulator_enable, S_IWUSR, NULL, regulator_enable_set);

static ssize_t spi_bus_lock_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "lock", strlen("lock")))
		spi_bus_lock(fpc1020->spi->master);
	else if (!strncmp(buf, "unlock", strlen("unlock")))
		spi_bus_unlock(fpc1020->spi->master);
	else
		return -EINVAL;
	return count;
}
static DEVICE_ATTR(bus_lock, S_IWUSR, NULL, spi_bus_lock_set);

static int hw_reset(struct  fpc1020_data *fpc1020)
{
	int irq_gpio;
	struct device *dev = fpc1020->dev;

	int rc = select_pin_ctl(fpc1020, "fpc1020_reset_active");
	if (rc)
		goto exit;
	usleep_range(FPC1020_RESET_HIGH1_US, FPC1020_RESET_HIGH1_US + 100);

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_reset");
	if (rc)
		goto exit;
	usleep_range(FPC1020_RESET_LOW_US, FPC1020_RESET_LOW_US + 100);

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_active");
	if (rc)
		goto exit;
	usleep_range(FPC1020_RESET_HIGH1_US, FPC1020_RESET_HIGH1_US + 100);

	irq_gpio = gpio_get_value(fpc1020->irq_gpio);
	dev_info(dev, "IRQ after reset %d\n", irq_gpio);
exit:
	return rc;
}

static ssize_t hw_reset_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "reset", strlen("reset")))
		rc = hw_reset(fpc1020);
	else
		return -EINVAL;
	return rc ? rc : count;
}
static DEVICE_ATTR(hw_reset, S_IWUSR, NULL, hw_reset_set);

/**
 * Will setup clocks, GPIOs, and regulators to correctly initialize the touch
 * sensor to be ready for work.
 *
 * In the correct order according to the sensor spec this function will
 * enable/disable regulators, SPI platform clocks, and reset line, all to set
 * the sensor in a correct power on or off state "electrical" wise.
 *
 * @see  spi_prepare_set
 * @note This function will not send any commands to the sensor it will only
 *       control it "electrically".
 */
static int device_prepare(struct  fpc1020_data *fpc1020, bool enable)
{
	int rc;

	mutex_lock(&fpc1020->lock);
	if (enable && !fpc1020->prepared) {
		spi_bus_lock(fpc1020->spi->master);
		fpc1020->prepared = true;
		select_pin_ctl(fpc1020, "fpc1020_reset_reset");
/*
		rc = vreg_setup(fpc1020, "vcc_spi", true);
		if (rc)
			goto exit;
*/
		rc = vreg_setup(fpc1020, "vdd_io", true);
		if (rc)
			goto exit_1;
/*
		rc = vreg_setup(fpc1020, "vdd_ana", true);
		if (rc)
			goto exit_2;
*/
		usleep_range(100, 1000);
/*
		rc = spi_set_fabric(fpc1020, true);
		if (rc)
			goto exit_3;
*/
		(void)select_pin_ctl(fpc1020, "fpc1020_cs_high");
		(void)select_pin_ctl(fpc1020, "fpc1020_reset_active");
		usleep_range(100, 200);
		(void)select_pin_ctl(fpc1020, "fpc1020_cs_active");

#ifdef SET_PIPE_OWNERSHIP
		rc = set_pipe_ownership(fpc1020, true);
		if (rc)
			goto exit_5;
#endif
	} else if (!enable && fpc1020->prepared) {
		rc = 0;
#ifdef SET_PIPE_OWNERSHIP
		(void)set_pipe_ownership(fpc1020, false);
exit_5:
#endif
//		(void)spi_set_fabric(fpc1020, false);
//exit_3:
		(void)select_pin_ctl(fpc1020, "fpc1020_cs_high");
		(void)select_pin_ctl(fpc1020, "fpc1020_reset_reset");
		usleep_range(100, 1000);

//		(void)vreg_setup(fpc1020, "vdd_ana", false);
//exit_2:
		(void)vreg_setup(fpc1020, "vdd_io", false);
exit_1:
//		(void)vreg_setup(fpc1020, "vcc_spi", false);
//exit:
		(void)select_pin_ctl(fpc1020, "fpc1020_cs_low");

		fpc1020->prepared = false;
		spi_bus_unlock(fpc1020->spi->master);
	} else {
		rc = 0;
	}
	mutex_unlock(&fpc1020->lock);
	return rc;
}

static ssize_t spi_prepare_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", strlen("enable")))
		rc = device_prepare(fpc1020, true);
	else if (!strncmp(buf, "disable", strlen("disable")))
		rc = device_prepare(fpc1020, false);
	else
		return -EINVAL;
	return rc ? rc : count;
}
static DEVICE_ATTR(spi_prepare, S_IWUSR, NULL, spi_prepare_set);

/**
 * sysfs node for controlling whether the driver is allowed
 * to wake up the platform on interrupt.
 */
static ssize_t wakeup_enable_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", strlen("enable")))
	{
		fpc1020->wakeup_enabled = true;
		smp_wmb();
	}
	else if (!strncmp(buf, "disable", strlen("disable")))
	{
		fpc1020->wakeup_enabled = false;
		smp_wmb();
	}
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR(wakeup_enable, S_IWUSR, NULL, wakeup_enable_set);


/**
 * sysf node to check the interrupt status of the sensor, the interrupt
 * handler should perform sysf_notify to allow userland to poll the node.
 */
static ssize_t irq_get(struct device* device,
			     struct device_attribute* attribute,
			     char* buffer)
{
	struct fpc1020_data* fpc1020 = dev_get_drvdata(device);
	int irq = gpio_get_value(fpc1020->irq_gpio);
	return scnprintf(buffer, PAGE_SIZE, "%i\n", irq);
}


/**
 * writing to the irq node will just drop a printk message
 * and return success, used for latency measurement.
 */
static ssize_t irq_ack(struct device* device,
			     struct device_attribute* attribute,
			     const char* buffer, size_t count)
{
	struct fpc1020_data* fpc1020 = dev_get_drvdata(device);
	dev_dbg(fpc1020->dev, "%s\n", __func__);
	return count;
}

static DEVICE_ATTR(irq, S_IRUSR | S_IWUSR, irq_get, irq_ack);

static struct attribute *attributes[] = {
	&dev_attr_pinctl_set.attr,
	&dev_attr_spi_owner.attr,
	&dev_attr_spi_prepare.attr,
	&dev_attr_fabric_vote.attr,
	&dev_attr_regulator_enable.attr,
	&dev_attr_bus_lock.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;
	dev_dbg(fpc1020->dev, "%s\n", __func__);

	/* Make sure 'wakeup_enabled' is updated before using it
	** since this is interrupt context (other thread...) */
	smp_rmb();

	if (fpc1020->wakeup_enabled ) {
		wake_lock_timeout(&fpc1020->ttw_wl, msecs_to_jiffies(FPC_TTW_HOLD_TIME));
	}

	sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);

	return IRQ_HANDLED;
}

static int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
		const char *label, int *gpio)
{
	struct device *dev = fpc1020->dev;
	struct device_node *np = dev->of_node;
	int rc = of_get_named_gpio(np, label, 0);
	if (rc < 0) {
		dev_err(dev, "failed to get '%s'\n", label);
		return rc;
	}
	*gpio = rc;
	rc = devm_gpio_request(dev, *gpio, label);
	if (rc) {
		dev_err(dev, "failed to request gpio %d\n", *gpio);
		return rc;
	}
	dev_dbg(dev, "%s %d\n", label, *gpio);
	return 0;
}

static int fpc1020_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	int rc = 0;
	size_t i;
	int irqf;
	struct device_node *np = dev->of_node;
	u32 val;

	struct fpc1020_data *fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020),
			GFP_KERNEL);
	if (!fpc1020) {
		dev_err(dev,
			"failed to allocate memory for struct fpc1020_data\n");
		rc = -ENOMEM;
		goto exit;
	}

	fpc1020->dev = dev;
	dev_set_drvdata(dev, fpc1020);
	fpc1020->spi = spi;

	if (!np) {
		dev_err(dev, "no of node found\n");
		rc = -EINVAL;
		goto exit;
	}

	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_irq",
			&fpc1020->irq_gpio);
	if (rc)
		goto exit;
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_cs0",
			&fpc1020->cs0_gpio);
	if (rc)
		goto exit;
/*
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_cs1",
			&fpc1020->cs1_gpio);
	if (rc)
		goto exit;
*/
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_rst",
			&fpc1020->rst_gpio);
	if (rc)
		goto exit;

	fpc1020->iface_clk = clk_get(dev, "iface_clk");
	if (IS_ERR(fpc1020->iface_clk)) {
		dev_err(dev, "%s: Failed to get iface_clk\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	fpc1020->core_clk = clk_get(dev, "core_clk");
	if (IS_ERR(fpc1020->core_clk)) {
		dev_err(dev, "%s: Failed to get core_clk\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	rc = of_property_read_u32(np, "spi-qup-id", &val);
	if (rc < 0) {
		dev_err(dev, "spi-qup-id not found\n");
		goto exit;
	}
	fpc1020->qup_id = val;
	dev_dbg(dev, "spi-qup-id %d\n", fpc1020->qup_id);

	fpc1020->fingerprint_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fpc1020->fingerprint_pinctrl)) {
		if (PTR_ERR(fpc1020->fingerprint_pinctrl) == -EPROBE_DEFER) {
			dev_info(dev, "pinctrl not ready\n");
			rc = -EPROBE_DEFER;
			goto exit;
		}
		dev_err(dev, "Target does not use pinctrl\n");
		fpc1020->fingerprint_pinctrl = NULL;
		rc = -EINVAL;
		goto exit;
	}

	for (i = 0; i < ARRAY_SIZE(fpc1020->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		struct pinctrl_state *state =
			pinctrl_lookup_state(fpc1020->fingerprint_pinctrl, n);
		if (IS_ERR(state)) {
			dev_err(dev, "cannot find '%s'\n", n);
			rc = -EINVAL;
			goto exit;
		}
		dev_info(dev, "found pin control %s\n", n);
		fpc1020->pinctrl_state[i] = state;
	}

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_reset");
	if (rc)
		goto exit;
	rc = select_pin_ctl(fpc1020, "fpc1020_cs_low");
	if (rc)
		goto exit;
	rc = select_pin_ctl(fpc1020, "fpc1020_irq_active");
	if (rc)
		goto exit;

//	rc = select_pin_ctl(fpc1020, "fpc1020_spi_active");
	if (rc)
		goto exit;

	fpc1020->wakeup_enabled = false;
	fpc1020->clocks_enabled = false;
	fpc1020->clocks_suspended = false;
	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;

	/* Although wakeup feature is desirable, it is doomed that the device will be
	 * woken up due to false interrupt caused by hw reset.
	 * As a proactive measure, disable the wakeup feature until further notice.*/
#ifdef ENABLE_WAKEUP
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		irqf |= IRQF_NO_SUSPEND;
		dev_info(&spi->dev, "%s enable-wakeup \n",__func__);
		device_init_wakeup(dev, 1);
	}
#endif
	mutex_init(&fpc1020->lock);
	rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
			NULL, fpc1020_irq_handler, irqf,
			dev_name(dev), fpc1020);
	if (rc) {
		dev_err(dev, "could not request irq %d\n",
				gpio_to_irq(fpc1020->irq_gpio));
		goto exit;
	}
	dev_dbg(dev, "requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));

#ifdef ENABLE_WAKEUP
	/* Request that the interrupt should be wakeable */
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		enable_irq_wake( gpio_to_irq( fpc1020->irq_gpio ) );
	}
#endif
	wake_lock_init(&fpc1020->ttw_wl, WAKE_LOCK_SUSPEND, "fpc_ttw_wl");

	rc = sysfs_create_group(&dev->kobj, &attribute_group);
	if (rc) {
		dev_err(dev, "could not create sysfs\n");
		goto exit;
	}

	if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot")) {
		dev_info(dev, "Enabling hardware\n");
		(void)device_prepare(fpc1020, true);
//		(void)set_clks(fpc1020, false);
	}

	dev_info(dev, "%s: ok\n", __func__);
exit:
	return rc;
}

static int fpc1020_remove(struct spi_device *spi)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(&spi->dev);

	sysfs_remove_group(&spi->dev.kobj, &attribute_group);
	mutex_destroy(&fpc1020->lock);
	wake_lock_destroy(&fpc1020->ttw_wl);
	(void)vreg_setup(fpc1020, "vdd_io", false);
//	(void)vreg_setup(fpc1020, "vcc_spi", false);
//	(void)vreg_setup(fpc1020, "vdd_ana", false);
	dev_info(&spi->dev, "%s\n", __func__);
	return 0;
}

/*static int fpc1020_suspend(struct spi_device *spi, pm_message_t mesg)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(&spi->dev);
	struct device *dev = &spi->dev;
//	fpc1020->clocks_suspended = fpc1020->clocks_enabled;
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		dev_info(&spi->dev, "%s do nothing\n",__func__);
	} else {
		select_pin_ctl(fpc1020, "fpc1020_reset_reset");
		(void)vreg_setup(fpc1020, "vdd_io", false);
		disable_irq( gpio_to_irq( fpc1020->irq_gpio ));
#ifdef HW240_TEMP_WA
		devm_free_irq(dev, gpio_to_irq(fpc1020->irq_gpio), fpc1020);
#endif
	}
//	set_clks(fpc1020, false);
	return 0;
} */

/*static int fpc1020_resume(struct spi_device *spi)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(&spi->dev);

#ifdef HW240_TEMP_WA
	int rc = 0;
	int irqf;
	struct device *dev = &spi->dev;
#endif
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		dev_info(&spi->dev, "%s do nothing\n",__func__);
	} else {
		(void)vreg_setup(fpc1020, "vdd_io", true);
		hw_reset(fpc1020);
#ifdef HW240_TEMP_WA
		irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
		rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
				NULL, fpc1020_irq_handler, irqf,
				dev_name(dev), fpc1020);
		if (rc) {
			dev_err(dev, "could not request irq %d\n",
					gpio_to_irq(fpc1020->irq_gpio));
		}
		dev_dbg(dev, "requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));
#else
		enable_irq( gpio_to_irq( fpc1020->irq_gpio ));
#endif
	}
//	if (fpc1020->clocks_suspended)
//		set_clks(fpc1020, true);
	return 0;
} */

static struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{}
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct spi_driver fpc1020_driver = {
	.driver = {
		.name	= "fpc1020",
		.owner	= THIS_MODULE,
		.of_match_table = fpc1020_of_match,
	},
	.probe		= fpc1020_probe,
	.remove		= fpc1020_remove,
	//.suspend	= fpc1020_suspend,
	//.resume		= fpc1020_resume,
};

static int __init fpc1020_init(void)
{
	int rc = spi_register_driver(&fpc1020_driver);
	if (!rc)
		pr_info("%s OK\n", __func__);
	else
		pr_err("%s %d\n", __func__, rc);
	return rc;
}

static void __exit fpc1020_exit(void)
{
	pr_info("%s\n", __func__);
	spi_unregister_driver(&fpc1020_driver);
}

module_init(fpc1020_init);
module_exit(fpc1020_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
