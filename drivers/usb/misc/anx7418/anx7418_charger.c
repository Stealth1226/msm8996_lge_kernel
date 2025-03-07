#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>

#include "anx7418.h"
#include "anx7418_charger.h"

#define CHG_WORK_DELAY 5000

static char *chg_supplicants[] = {
	"battery",
};

static enum power_supply_property chg_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_TYPE,
};

static const char *chg_to_string(enum power_supply_type type)
{
	switch (type) {
	case POWER_SUPPLY_TYPE_CTYPE:
		return "USB Type-C Charger";
	case POWER_SUPPLY_TYPE_CTYPE_PD:
		return "USB Type-C PD Charger";
	default:
		return "Unknown Charger";
	}
}

static int chg_get_property(struct power_supply *psy,
		enum power_supply_property prop,
		union power_supply_propval *val)
{
	struct anx7418_charger *chg = container_of(psy,
			struct anx7418_charger, psy);
	struct anx7418 *anx = chg->anx;
	struct device *cdev = &chg->anx->client->dev;
	int rc;

	switch(prop) {
	case POWER_SUPPLY_PROP_USB_OTG:
		dev_dbg(cdev, "%s: is_otg(%d)\n", __func__,
				chg->is_otg);
		val->intval = chg->is_otg;
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		dev_dbg(cdev, "%s: is_present(%d)\n", __func__,
				chg->is_present);
		val->intval = chg->is_present;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		dev_dbg(cdev, "%s: volt_max(%dmV)\n", __func__, chg->volt_max);
		val->intval = chg->volt_max;
		break;

	case POWER_SUPPLY_PROP_CURRENT_MAX:
		dev_dbg(cdev, "%s: curr_max(%dmA)\n", __func__, chg->curr_max);
		val->intval = chg->curr_max;
		break;

	case POWER_SUPPLY_PROP_TYPE:
		dev_dbg(cdev, "%s: type(%s)\n", __func__,
				chg_to_string(chg->psy.type));
		val->intval = chg->psy.type;
		break;

	case POWER_SUPPLY_PROP_TYPEC_MODE:
		rc = anx7418_read_reg(anx->client, CC_STATUS);
		dev_dbg(cdev, "%s: CC_STATUS(%02X)\n", __func__, rc);

		val->intval = (rc == 0x11) ?
			POWER_SUPPLY_TYPE_CTYPE_DEBUG_ACCESSORY :
			POWER_SUPPLY_TYPE_UNKNOWN;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int chg_set_property(struct power_supply *psy,
		enum power_supply_property prop,
		const union power_supply_propval *val)
{
	struct anx7418_charger *chg = container_of(psy,
			struct anx7418_charger, psy);
	struct anx7418 *anx = chg->anx;
	struct device *cdev = &chg->anx->client->dev;
	int rc;

	switch (prop) {
	case POWER_SUPPLY_PROP_USB_OTG:
		if (chg->is_otg == val->intval)
			break;
		dev_dbg(cdev, "%s: is_otg(%d)\n", __func__, chg->is_otg);
		chg->is_otg = val->intval;

		anx_dbg_event("VBUS REG", chg->is_otg);
		if (chg->is_otg) {
			rc = regulator_enable(anx->vbus_reg);
			if (rc)
				dev_err(cdev, "unable to enable vbus\n");
			anx_dbg_event("VBUS", 1);
		} else {
			rc = regulator_disable(anx->vbus_reg);
			if (rc)
				dev_err(cdev, "unable to disable vbus\n");
			anx_dbg_event("VBUS", 0);
		}
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		if (anx->is_dbg_acc || anx->mode == DUAL_ROLE_PROP_MODE_NONE) {
			if (val->intval) {
				dev_info(cdev, "power on by charger\n");
				anx7418_set_dr(anx, DUAL_ROLE_PROP_DR_DEVICE);
			} else {
				if (anx->dr == DUAL_ROLE_PROP_DR_DEVICE) {
					dev_info(cdev, "power down by charger\n");
					anx7418_set_dr(anx, DUAL_ROLE_PROP_DR_NONE);
				} else if (anx->dr == DUAL_ROLE_PROP_DR_NONE) {
					union power_supply_propval prop;
					anx->usb_psy->get_property(anx->usb_psy,
							POWER_SUPPLY_PROP_PRESENT, &prop);
					if (prop.intval) {
						dev_info(cdev, "power down by charger\n");
						power_supply_set_present(anx->usb_psy, 0);
					}
				}
			}
		}

		if (chg->is_present == val->intval)
			break;
		chg->is_present = val->intval;
		dev_dbg(cdev, "%s: is_present(%d)\n", __func__, chg->is_present);

		if (chg->is_present) {
			schedule_delayed_work(&chg->chg_work,
					msecs_to_jiffies(CHG_WORK_DELAY));
		} else {
			cancel_delayed_work(&chg->chg_work);
			chg->curr_max = 0;
			chg->volt_max = 0;
			chg->ctype_charger = ANX7418_UNKNOWN_CHARGER;
		}

		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		chg->volt_max = val->intval;
		dev_dbg(cdev, "%s: volt_max(%dmV)\n", __func__, chg->volt_max);
		break;

	case POWER_SUPPLY_PROP_CURRENT_MAX:
		chg->curr_max = val->intval;
		dev_dbg(cdev, "%s: curr_max(%dmA)\n", __func__, chg->curr_max);
		break;

	case POWER_SUPPLY_PROP_TYPE:
		switch (val->intval) {
		case POWER_SUPPLY_TYPE_CTYPE:
		case POWER_SUPPLY_TYPE_CTYPE_PD:
		case POWER_SUPPLY_TYPE_USB_HVDCP:
		case POWER_SUPPLY_TYPE_USB_HVDCP_3:
			psy->type = val->intval;
			break;
		default:
			psy->type = POWER_SUPPLY_TYPE_UNKNOWN;
			break;
		}
		dev_dbg(cdev, "%s: type(%s)\n", __func__, chg_to_string(psy->type));

		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int chg_is_writeable(struct power_supply *psy,
		enum power_supply_property prop)
{
	int rc;

	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_TYPE:
		rc = 1;
		break;
	default:
		rc = 0;
		break;
	}

	return rc;
}

static void chg_work(struct work_struct *w)
{
	struct anx7418_charger *chg = container_of(w,
			struct anx7418_charger, chg_work.work);
	struct anx7418 *anx = chg->anx;
	struct i2c_client *client = anx->client;
	struct device *cdev = &client->dev;
	int rc = 0;

	down_read(&anx->rwsem);

	if (!atomic_read(&anx->pwr_on))
		goto out;

	if (chg->psy.type == POWER_SUPPLY_TYPE_USB_HVDCP ||
	    chg->psy.type == POWER_SUPPLY_TYPE_USB_HVDCP_3)
		goto out;

	if (chg->ctype_charger != ANX7418_CTYPE_PD_CHARGER) {
		/* check ctype charger */
		rc = anx7418_read_reg(client, POWER_DOWN_CTRL);

		if (rc & (CC1_VRD_3P0 | CC2_VRD_3P0)) {
			// 5V@3A
			chg->volt_max = 5000;
			chg->curr_max = 2000;
			chg->ctype_charger = ANX7418_CTYPE_CHARGER;

		} else if (rc & (CC1_VRD_1P5 | CC2_VRD_1P5)) {
			// 5V@1.5A
			chg->volt_max = 5000;
			chg->curr_max = 1500;
			chg->ctype_charger = ANX7418_CTYPE_CHARGER;

		} else {
			// Default USB Current
			dev_info(cdev, "%s: Default USB Power\n", __func__);
			chg->volt_max = 5000;
			chg->curr_max = 0;
			chg->ctype_charger = ANX7418_UNKNOWN_CHARGER;
		}
	}

	/* Update ctype(ctype-pd) charger */
	switch (chg->ctype_charger) {
	case ANX7418_CTYPE_CHARGER:
		power_supply_set_supply_type(&chg->psy,
					POWER_SUPPLY_TYPE_CTYPE);
		break;
	case ANX7418_CTYPE_PD_CHARGER:
		power_supply_set_supply_type(&chg->psy,
					POWER_SUPPLY_TYPE_CTYPE_PD);
		break;
	default: // unknown charger
		goto out;
	}

	dev_info(cdev, "%s: %s, %dmV, %dmA\n", __func__,
			chg_to_string(chg->psy.type),
			chg->volt_max,
			chg->curr_max);

	power_supply_changed(&chg->psy);
out:
	up_read(&anx->rwsem);
}

int anx7418_charger_init(struct anx7418 *anx)
{
	struct anx7418_charger *chg = &anx->chg;
	struct device *cdev = &anx->client->dev;
	int rc;

	chg->psy.name = "usb_pd";
	chg->psy.type = POWER_SUPPLY_TYPE_CTYPE;
	chg->psy.get_property = chg_get_property;
	chg->psy.set_property = chg_set_property;
	chg->psy.property_is_writeable = chg_is_writeable;
	chg->psy.properties = chg_properties;
	chg->psy.num_properties = ARRAY_SIZE(chg_properties);
	chg->psy.supplied_to = chg_supplicants;
	chg->psy.num_supplicants = ARRAY_SIZE(chg_supplicants);

	chg->anx = anx;
	INIT_DELAYED_WORK(&chg->chg_work, chg_work);

	rc = power_supply_register(cdev, &chg->psy);
	if (rc < 0) {
		dev_err(cdev, "Unalbe to register ctype_psy rc = %d\n", rc);
		return -EPROBE_DEFER;
	}

	power_supply_set_supply_type(&chg->psy, POWER_SUPPLY_TYPE_UNKNOWN);

	return 0;
}
