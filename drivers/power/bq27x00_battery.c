/*
 * BQ27x00 battery driver
 *
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Copyright (C) 2010-2011 Lars-Peter Clausen <lars@metafoo.de>
 * Copyright (C) 2011 Pali Rohár <pali.rohar@gmail.com>
 *
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

/*
 * Datasheets:
 * http://focus.ti.com/docs/prod/folders/print/bq27000.html
 * http://focus.ti.com/docs/prod/folders/print/bq27500.html
 */

#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <asm/unaligned.h>

#include <linux/power/bq27x00_battery.h>

#define DRIVER_VERSION			"1.2.0"

#define BQ27x00_REG_TEMP		0x06
#define BQ27x00_REG_VOLT		0x08
#define BQ27x00_REG_AI			0x14
#define BQ27x00_REG_FLAGS		0x0A
#define BQ27x00_REG_TTE			0x16
#define BQ27x00_REG_TTF			0x18
#define BQ27x00_REG_TTECP		0x26
#define BQ27x00_REG_NAC			0x0C /* Nominal available capaciy */
#define BQ27x00_REG_LMD			0x12 /* Last measured discharge */
#define BQ27x00_REG_CYCT		0x2A /* Cycle count total */
#define BQ27x00_REG_AE			0x22 /* Available enery */

#define BQ27000_REG_RSOC		0x0B /* Relative State-of-Charge */
#define BQ27000_REG_ILMD		0x76 /* Initial last measured discharge */
#define BQ27000_FLAG_CHGS		BIT(7)
#define BQ27000_FLAG_FC			BIT(5)

#define BQ27500_REG_SOC			0x2C
#define BQ27500_REG_DCAP		0x3C /* Design capacity */
#define BQ27500_FLAG_DSC		BIT(0)
#define BQ27500_FLAG_FC			BIT(9)

#define BQ27000_RS			20 /* Resistor sense */

struct bq27x00_device_info;
struct bq27x00_access_methods {
	int (*read)(struct bq27x00_device_info *di, u8 reg, bool single);
};

enum bq27x00_chip { BQ27000, BQ27500 };

struct bq27x00_reg_cache {
	int temperature;
	int time_to_empty;
	int time_to_empty_avg;
	int time_to_full;
	int charge_full;
	int cycle_count;
	int capacity;
	int flags;

	int current_now;
};

struct bq27x00_device_info {
	struct device 		*dev;
	int			id;
	enum bq27x00_chip	chip;

	struct bq27x00_reg_cache cache;
	int charge_design_full;

	unsigned long last_update;
	struct delayed_work work;

	struct power_supply	bat;

	struct bq27x00_access_methods bus;

	struct mutex lock;
};

static enum power_supply_property bq27x00_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW,
};

static unsigned int poll_interval = 360;
module_param(poll_interval, uint, 0644);
MODULE_PARM_DESC(poll_interval, "battery poll interval in seconds - " \
				"0 disables polling");

/*
 * Common code for BQ27x00 devices
 */

static inline int bq27x00_read(struct bq27x00_device_info *di, u8 reg,
		bool single)
{
	return di->bus.read(di, reg, single);
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_rsoc(struct bq27x00_device_info *di)
{
	int rsoc;

	if (di->chip == BQ27500)
		rsoc = bq27x00_read(di, BQ27500_REG_SOC, false);
	else
		rsoc = bq27x00_read(di, BQ27000_REG_RSOC, true);

	if (rsoc < 0)
		dev_err(di->dev, "error reading relative State-of-Charge\n");

	return rsoc;
}

/*
 * Return a battery charge value in µAh
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_charge(struct bq27x00_device_info *di, u8 reg)
{
	int charge;

	charge = bq27x00_read(di, reg, false);
	if (charge < 0) {
		dev_err(di->dev, "error reading nominal available capacity\n");
		return charge;
	}

	if (di->chip == BQ27500)
		charge *= 1000;
	else
		charge = charge * 3570 / BQ27000_RS;

	return charge;
}

/*
 * Return the battery Nominal available capaciy in µAh
 * Or < 0 if something fails.
 */
static inline int bq27x00_battery_read_nac(struct bq27x00_device_info *di)
{
	return bq27x00_battery_read_charge(di, BQ27x00_REG_NAC);
}

/*
 * Return the battery Last measured discharge in µAh
 * Or < 0 if something fails.
 */
static inline int bq27x00_battery_read_lmd(struct bq27x00_device_info *di)
{
	return bq27x00_battery_read_charge(di, BQ27x00_REG_LMD);
}

/*
 * Return the battery Initial last measured discharge in µAh
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_ilmd(struct bq27x00_device_info *di)
{
	int ilmd;

	if (di->chip == BQ27500)
		ilmd = bq27x00_read(di, BQ27500_REG_DCAP, false);
	else
		ilmd = bq27x00_read(di, BQ27000_REG_ILMD, true);

	if (ilmd < 0) {
		dev_err(di->dev, "error reading initial last measured discharge\n");
		return ilmd;
	}

	if (di->chip == BQ27500)
		ilmd *= 1000;
	else
		ilmd = ilmd * 256 * 3570 / BQ27000_RS;

	return ilmd;
}

/*
 * Return the battery Cycle count total
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_cyct(struct bq27x00_device_info *di)
{
	int cyct;

	cyct = bq27x00_read(di, BQ27x00_REG_CYCT, false);
	if (cyct < 0)
		dev_err(di->dev, "error reading cycle count total\n");

	return cyct;
}

/*
 * Read a time register.
 * Return < 0 if something fails.
 */
static int bq27x00_battery_read_time(struct bq27x00_device_info *di, u8 reg)
{
	int tval;

	tval = bq27x00_read(di, reg, false);
	if (tval < 0) {
		dev_err(di->dev, "error reading register %02x: %d\n", reg, tval);
		return tval;
	}

	if (tval == 65535)
		return -ENODATA;

	return tval * 60;
}

static void bq27x00_update(struct bq27x00_device_info *di)
{
	struct bq27x00_reg_cache cache = {0, };
	bool is_bq27500 = di->chip == BQ27500;

	cache.flags = bq27x00_read(di, BQ27x00_REG_FLAGS, is_bq27500);
	if (cache.flags >= 0) {
		cache.capacity = bq27x00_battery_read_rsoc(di);
		cache.temperature = bq27x00_read(di, BQ27x00_REG_TEMP, false);
		cache.time_to_empty = bq27x00_battery_read_time(di, BQ27x00_REG_TTE);
		cache.time_to_empty_avg = bq27x00_battery_read_time(di, BQ27x00_REG_TTECP);
		cache.time_to_full = bq27x00_battery_read_time(di, BQ27x00_REG_TTF);
		cache.charge_full = bq27x00_battery_read_lmd(di);
		cache.cycle_count = bq27x00_battery_read_cyct(di);

		if (!is_bq27500)
			cache.current_now = bq27x00_read(di, BQ27x00_REG_AI, false);

		/* We only have to read charge design full once */
		if (di->charge_design_full <= 0)
			di->charge_design_full = bq27x00_battery_read_ilmd(di);
	}

	/* Ignore current_now which is a snapshot of the current battery state
	 * and is likely to be different even between two consecutive reads */
	if (memcmp(&di->cache, &cache, sizeof(cache) - sizeof(int)) != 0) {
		di->cache = cache;
		power_supply_changed(&di->bat);
	}

	di->last_update = jiffies;
}

static void bq27x00_battery_poll(struct work_struct *work)
{
	struct bq27x00_device_info *di =
		container_of(work, struct bq27x00_device_info, work.work);

	bq27x00_update(di);

	if (poll_interval > 0) {
		/* The timer does not have to be accurate. */
		set_timer_slack(&di->work.timer, poll_interval * HZ / 4);
		schedule_delayed_work(&di->work, poll_interval * HZ);
	}
}


/*
 * Return the battery temperature in tenths of degree Celsius
 * Or < 0 if something fails.
 */
static int bq27x00_battery_temperature(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	if (di->cache.temperature < 0)
		return di->cache.temperature;

	if (di->chip == BQ27500)
		val->intval = di->cache.temperature - 2731;
	else
		val->intval = ((di->cache.temperature * 5) - 5463) / 2;

	return 0;
}

/*
 * Return the battery average current in µA
 * Note that current can be negative signed as well
 * Or 0 if something fails.
 */
static int bq27x00_battery_current(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int curr;

	if (di->chip == BQ27500)
	    curr = bq27x00_read(di, BQ27x00_REG_AI, false);
	else
	    curr = di->cache.current_now;

	if (curr < 0)
		return curr;

	if (di->chip == BQ27500) {
		/* bq27500 returns signed value */
		val->intval = (int)((s16)curr) * 1000;
	} else {
		if (di->cache.flags & BQ27000_FLAG_CHGS) {
			dev_dbg(di->dev, "negative current!\n");
			curr = -curr;
		}

		val->intval = curr * 3570 / BQ27000_RS;
	}

	return 0;
}

static int bq27x00_battery_status(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int status;

	if (di->chip == BQ27500) {
		if (di->cache.flags & BQ27500_FLAG_FC)
			status = POWER_SUPPLY_STATUS_FULL;
		else if (di->cache.flags & BQ27500_FLAG_DSC)
			status = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			status = POWER_SUPPLY_STATUS_CHARGING;
	} else {
		if (di->cache.flags & BQ27000_FLAG_FC)
			status = POWER_SUPPLY_STATUS_FULL;
		else if (di->cache.flags & BQ27000_FLAG_CHGS)
			status = POWER_SUPPLY_STATUS_CHARGING;
		else if (power_supply_am_i_supplied(&di->bat))
			status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			status = POWER_SUPPLY_STATUS_DISCHARGING;
	}

	val->intval = status;

	return 0;
}

/*
 * Return the battery Voltage in milivolts
 * Or < 0 if something fails.
 */
static int bq27x00_battery_voltage(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int volt;

	volt = bq27x00_read(di, BQ27x00_REG_VOLT, false);
	if (volt < 0)
		return volt;

	val->intval = volt * 1000;

	return 0;
}

/*
 * Return the battery Available energy in µWh
 * Or < 0 if something fails.
 */
static int bq27x00_battery_energy(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int ae;

	ae = bq27x00_read(di, BQ27x00_REG_AE, false);
	if (ae < 0) {
		dev_err(di->dev, "error reading available energy\n");
		return ae;
	}

	if (di->chip == BQ27500)
		ae *= 1000;
	else
		ae = ae * 29200 / BQ27000_RS;

	val->intval = ae;

	return 0;
}


static int bq27x00_simple_value(int value,
	union power_supply_propval *val)
{
	if (value < 0)
		return value;

	val->intval = value;

	return 0;
}

#define to_bq27x00_device_info(x) container_of((x), \
				struct bq27x00_device_info, bat);

static int bq27x00_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = 0;
	struct bq27x00_device_info *di = to_bq27x00_device_info(psy);

	mutex_lock(&di->lock);
	if (time_is_before_jiffies(di->last_update + 5 * HZ)) {
		cancel_delayed_work_sync(&di->work);
		bq27x00_battery_poll(&di->work.work);
	}
	mutex_unlock(&di->lock);

	if (psp != POWER_SUPPLY_PROP_PRESENT && di->cache.flags < 0)
		return -ENODEV;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = bq27x00_battery_status(di, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = bq27x00_battery_voltage(di, val);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = di->cache.flags < 0 ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = bq27x00_battery_current(di, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = bq27x00_simple_value(di->cache.capacity, val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = bq27x00_battery_temperature(di, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = bq27x00_simple_value(di->cache.time_to_empty, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		ret = bq27x00_simple_value(di->cache.time_to_empty_avg, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		ret = bq27x00_simple_value(di->cache.time_to_full, val);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = bq27x00_simple_value(bq27x00_battery_read_nac(di), val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = bq27x00_simple_value(di->cache.charge_full, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = bq27x00_simple_value(di->charge_design_full, val);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = bq27x00_simple_value(di->cache.cycle_count, val);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		ret = bq27x00_battery_energy(di, val);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static void bq27x00_external_power_changed(struct power_supply *psy)
{
	struct bq27x00_device_info *di = to_bq27x00_device_info(psy);

	cancel_delayed_work_sync(&di->work);
	schedule_delayed_work(&di->work, 0);
}

static int bq27x00_powersupply_init(struct bq27x00_device_info *di)
{
	int ret;

	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = bq27x00_battery_props;
	di->bat.num_properties = ARRAY_SIZE(bq27x00_battery_props);
	di->bat.get_property = bq27x00_battery_get_property;
	di->bat.external_power_changed = bq27x00_external_power_changed;

	INIT_DELAYED_WORK(&di->work, bq27x00_battery_poll);
	mutex_init(&di->lock);

	ret = power_supply_register(di->dev, &di->bat);
	if (ret) {
		dev_err(di->dev, "failed to register battery: %d\n", ret);
		return ret;
	}

	dev_info(di->dev, "support ver. %s enabled\n", DRIVER_VERSION);

	bq27x00_update(di);

	return 0;
}

static void bq27x00_powersupply_unregister(struct bq27x00_device_info *di)
{
	cancel_delayed_work_sync(&di->work);

	power_supply_unregister(&di->bat);

	mutex_destroy(&di->lock);
}


/* i2c specific code */
#ifdef CONFIG_BATTERY_BQ27X00_I2C

/* If the system has several batteries we need a different name for each
 * of them...
 */
static DEFINE_IDR(battery_id);
static DEFINE_MUTEX(battery_mutex);

static int bq27x00_read_i2c(struct bq27x00_device_info *di, u8 reg, bool single)
{
	struct i2c_client *client = to_i2c_client(di->dev);
	struct i2c_msg msg[2];
	unsigned char data[2];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;
	if (single)
		msg[1].len = 1;
	else
		msg[1].len = 2;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		return ret;

	if (!single)
		ret = get_unaligned_le16(data);
	else
		ret = data[0];

	return ret;
}

static int bq27x00_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	char *name;
	struct bq27x00_device_info *di;
	int num;
	int retval = 0;

	/* Get new ID for the new battery device */
	retval = idr_pre_get(&battery_id, GFP_KERNEL);
	if (retval == 0)
		return -ENOMEM;
	mutex_lock(&battery_mutex);
	retval = idr_get_new(&battery_id, client, &num);
	mutex_unlock(&battery_mutex);
	if (retval < 0)
		return retval;

	name = kasprintf(GFP_KERNEL, "%s-%d", id->name, num);
	if (!name) {
		dev_err(&client->dev, "failed to allocate device name\n");
		retval = -ENOMEM;
		goto batt_failed_1;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}

	di->id = num;
	di->dev = &client->dev;
	di->chip = id->driver_data;
	di->bat.name = name;
	di->bus.read = &bq27x00_read_i2c;

	if (bq27x00_powersupply_init(di))
		goto batt_failed_3;

	i2c_set_clientdata(client, di);

	return 0;

batt_failed_3:
	kfree(di);
batt_failed_2:
	kfree(name);
batt_failed_1:
	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, num);
	mutex_unlock(&battery_mutex);

	return retval;
}

static int bq27x00_battery_remove(struct i2c_client *client)
{
	struct bq27x00_device_info *di = i2c_get_clientdata(client);

	bq27x00_powersupply_unregister(di);

	kfree(di->bat.name);

	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, di->id);
	mutex_unlock(&battery_mutex);

	kfree(di);

	return 0;
}

static const struct i2c_device_id bq27x00_id[] = {
	{ "bq27200", BQ27000 },	/* bq27200 is same as bq27000, but with i2c */
	{ "bq27500", BQ27500 },
	{},
};
MODULE_DEVICE_TABLE(i2c, bq27x00_id);

static struct i2c_driver bq27x00_battery_driver = {
	.driver = {
		.name = "bq27x00-battery",
	},
	.probe = bq27x00_battery_probe,
	.remove = bq27x00_battery_remove,
	.id_table = bq27x00_id,
};

static inline int bq27x00_battery_i2c_init(void)
{
	int ret = i2c_add_driver(&bq27x00_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register BQ27x00 i2c driver\n");

	return ret;
}

static inline void bq27x00_battery_i2c_exit(void)
{
	i2c_del_driver(&bq27x00_battery_driver);
}

#else

static inline int bq27x00_battery_i2c_init(void) { return 0; }
static inline void bq27x00_battery_i2c_exit(void) {};

#endif

/* platform specific code */
#ifdef CONFIG_BATTERY_BQ27X00_PLATFORM

static int bq27000_read_platform(struct bq27x00_device_info *di, u8 reg,
			bool single)
{
	struct device *dev = di->dev;
	struct bq27000_platform_data *pdata = dev->platform_data;
	unsigned int timeout = 3;
	int upper, lower;
	int temp;

	if (!single) {
		/* Make sure the value has not changed in between reading the
		 * lower and the upper part */
		upper = pdata->read(dev, reg + 1);
		do {
			temp = upper;
			if (upper < 0)
				return upper;

			lower = pdata->read(dev, reg);
			if (lower < 0)
				return lower;

			upper = pdata->read(dev, reg + 1);
		} while (temp != upper && --timeout);

		if (timeout == 0)
			return -EIO;

		return (upper << 8) | lower;
	}

	return pdata->read(dev, reg);
}

static int bq27000_battery_probe(struct platform_device *pdev)
{
	struct bq27x00_device_info *di;
	struct bq27000_platform_data *pdata = pdev->dev.platform_data;
	int ret;

	if (!pdata) {
		dev_err(&pdev->dev, "no platform_data supplied\n");
		return -EINVAL;
	}

	if (!pdata->read) {
		dev_err(&pdev->dev, "no hdq read callback supplied\n");
		return -EINVAL;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&pdev->dev, "failed to allocate device info data\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, di);

	di->dev = &pdev->dev;
	di->chip = BQ27000;

	di->bat.name = pdata->name ?: dev_name(&pdev->dev);
	di->bus.read = &bq27000_read_platform;

	ret = bq27x00_powersupply_init(di);
	if (ret)
		goto err_free;

	return 0;

err_free:
	platform_set_drvdata(pdev, NULL);
	kfree(di);

	return ret;
}

static int bq27000_battery_remove(struct platform_device *pdev)
{
	struct bq27x00_device_info *di = platform_get_drvdata(pdev);

	bq27x00_powersupply_unregister(di);

	platform_set_drvdata(pdev, NULL);
	kfree(di);

	return 0;
}

static struct platform_driver bq27000_battery_driver = {
	.probe	= bq27000_battery_probe,
	.remove = bq27000_battery_remove,
	.driver = {
		.name = "bq27000-battery",
		.owner = THIS_MODULE,
	},
};

static inline int bq27x00_battery_platform_init(void)
{
	int ret = platform_driver_register(&bq27000_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register BQ27000 platform driver\n");

	return ret;
}

static inline void bq27x00_battery_platform_exit(void)
{
	platform_driver_unregister(&bq27000_battery_driver);
}

#else

static inline int bq27x00_battery_platform_init(void) { return 0; }
static inline void bq27x00_battery_platform_exit(void) {};

#endif

/*
 * Module stuff
 */

static int __init bq27x00_battery_init(void)
{
	int ret;

	ret = bq27x00_battery_i2c_init();
	if (ret)
		return ret;

	ret = bq27x00_battery_platform_init();
	if (ret)
		bq27x00_battery_i2c_exit();

	return ret;
}
module_init(bq27x00_battery_init);

static void __exit bq27x00_battery_exit(void)
{
	bq27x00_battery_platform_exit();
	bq27x00_battery_i2c_exit();
}
module_exit(bq27x00_battery_exit);

MODULE_AUTHOR("Rodolfo Giometti <giometti@linux.it>");
MODULE_DESCRIPTION("BQ27x00 battery monitor driver");
MODULE_LICENSE("GPL");
