/*
 * STMicroelectronics st_lsm6dsx sensor driver
 *
 * The ST LSM6DSx IMU MEMS series consists of 3D digital accelerometer
 * and 3D digital gyroscope system-in-package with a digital I2C/SPI serial
 * interface standard output.
 * LSM6DSx IMU MEMS series has a dynamic user-selectable full-scale
 * acceleration range of +-2/+-4/+-8/+-16 g and an angular rate range of
 * +-125/+-245/+-500/+-1000/+-2000 dps
 * LSM6DSx series has an integrated First-In-First-Out (FIFO) buffer
 * allowing dynamic batching of sensor data.
 *
 * Supported sensors:
 * - LSM6DS3:
 *   - Accelerometer/Gyroscope supported ODR [Hz]: 13, 26, 52, 104, 208, 416
 *   - Accelerometer supported full-scale [g]: +-2/+-4/+-8/+-16
 *   - Gyroscope supported full-scale [dps]: +-125/+-245/+-500/+-1000/+-2000
 *   - FIFO size: 8KB
 *
 * - LSM6DS3H/LSM6DSL/LSM6DSM:
 *   - Accelerometer/Gyroscope supported ODR [Hz]: 13, 26, 52, 104, 208, 416
 *   - Accelerometer supported full-scale [g]: +-2/+-4/+-8/+-16
 *   - Gyroscope supported full-scale [dps]: +-125/+-245/+-500/+-1000/+-2000
 *   - FIFO size: 4KB
 *
 * Copyright 2016 STMicroelectronics Inc.
 *
 * Lorenzo Bianconi <lorenzo.bianconi@st.com>
 * Denis Ciocca <denis.ciocca@st.com>
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include <linux/platform_data/st_sensors_pdata.h>

#include "st_lsm6dsx.h"
#include "lsm6dsl/LSM6DSL_ACC_GYRO_driver.h"
#include "lsm6dsl/lsm6dsl_events.h"

#define ST_LSM6DSX_REG_ACC_DEC_MASK		GENMASK(2, 0)
#define ST_LSM6DSX_REG_GYRO_DEC_MASK		GENMASK(5, 3)
#define ST_LSM6DSX_REG_INT1_ADDR		0x0d
#define ST_LSM6DSX_REG_INT2_ADDR		0x0e
#define ST_LSM6DSX_REG_FIFO_FTH_IRQ_MASK	BIT(3)
#define ST_LSM6DSX_REG_WHOAMI_ADDR		0x0f
#define ST_LSM6DSX_REG_RESET_ADDR		0x12
#define ST_LSM6DSX_REG_RESET_MASK		BIT(0)
#define ST_LSM6DSX_REG_BDU_ADDR			0x12
#define ST_LSM6DSX_REG_BDU_MASK			BIT(6)
#define ST_LSM6DSX_REG_INT2_ON_INT1_ADDR	0x13
#define ST_LSM6DSX_REG_INT2_ON_INT1_MASK	BIT(5)
#define ST_LSM6DSX_REG_ROUNDING_ADDR		0x16
#define ST_LSM6DSX_REG_ROUNDING_MASK		BIT(2)
#define ST_LSM6DSX_REG_LIR_ADDR			0x58
#define ST_LSM6DSX_REG_LIR_MASK			BIT(0)

#define ST_LSM6DSX_REG_ACC_ODR_ADDR		0x10
#define ST_LSM6DSX_REG_ACC_ODR_MASK		GENMASK(7, 4)
#define ST_LSM6DSX_REG_ACC_FS_ADDR		0x10
#define ST_LSM6DSX_REG_ACC_FS_MASK		GENMASK(3, 2)
#define ST_LSM6DSX_REG_ACC_OUT_X_L_ADDR		0x28
#define ST_LSM6DSX_REG_ACC_OUT_Y_L_ADDR		0x2a
#define ST_LSM6DSX_REG_ACC_OUT_Z_L_ADDR		0x2c

#define ST_LSM6DSX_REG_GYRO_ODR_ADDR		0x11
#define ST_LSM6DSX_REG_GYRO_ODR_MASK		GENMASK(7, 4)
#define ST_LSM6DSX_REG_GYRO_FS_ADDR		0x11
#define ST_LSM6DSX_REG_GYRO_FS_MASK		GENMASK(3, 2)
#define ST_LSM6DSX_REG_GYRO_OUT_X_L_ADDR	0x22
#define ST_LSM6DSX_REG_GYRO_OUT_Y_L_ADDR	0x24
#define ST_LSM6DSX_REG_GYRO_OUT_Z_L_ADDR	0x26

#define ST_LSM6DSX_ACC_FS_2G_GAIN		IIO_G_TO_M_S_2(61)
#define ST_LSM6DSX_ACC_FS_4G_GAIN		IIO_G_TO_M_S_2(122)
#define ST_LSM6DSX_ACC_FS_8G_GAIN		IIO_G_TO_M_S_2(244)
#define ST_LSM6DSX_ACC_FS_16G_GAIN		IIO_G_TO_M_S_2(488)

#define ST_LSM6DSX_GYRO_FS_245_GAIN		IIO_DEGREE_TO_RAD(8750)
#define ST_LSM6DSX_GYRO_FS_500_GAIN		IIO_DEGREE_TO_RAD(17500)
#define ST_LSM6DSX_GYRO_FS_1000_GAIN		IIO_DEGREE_TO_RAD(35000)
#define ST_LSM6DSX_GYRO_FS_2000_GAIN		IIO_DEGREE_TO_RAD(70000)

enum st_lsm6dsx_event_type {
	ST_LSM6DSX_TAP,
	ST_LSM6DSX_DTAP,
	ST_LSM6DSX_TILT,
	ST_LSM6DSX_FREEFALL,
	ST_LSM6DSX_PEDOMETER,
	ST_LSM6DSX_WAKEUP,
};


struct st_lsm6dsx_odr {
	u16 hz;
	u8 val;
};

#define ST_LSM6DSX_ODR_LIST_SIZE	7
struct st_lsm6dsx_odr_table_entry {
	struct st_lsm6dsx_reg reg;
	struct st_lsm6dsx_odr odr_avl[ST_LSM6DSX_ODR_LIST_SIZE];
};

static const struct st_lsm6dsx_odr_table_entry st_lsm6dsx_odr_table[] = {
	[ST_LSM6DSX_ID_ACC] = {
		.reg = {
			.addr = ST_LSM6DSX_REG_ACC_ODR_ADDR,
			.mask = ST_LSM6DSX_REG_ACC_ODR_MASK,
		},
		.odr_avl[0] = {   0, 0x00},
		.odr_avl[1] = {  13, 0x01 },
		.odr_avl[2] = {  26, 0x02 },
		.odr_avl[3] = {  52, 0x03 },
		.odr_avl[4] = { 104, 0x04 },
		.odr_avl[5] = { 208, 0x05 },
		.odr_avl[6] = { 416, 0x06 },
	},
	[ST_LSM6DSX_ID_GYRO] = {
		.reg = {
			.addr = ST_LSM6DSX_REG_GYRO_ODR_ADDR,
			.mask = ST_LSM6DSX_REG_GYRO_ODR_MASK,
		},
		.odr_avl[0] = {   0,  0x00},
		.odr_avl[1] = {  13, 0x01 },
		.odr_avl[2] = {  26, 0x02 },
		.odr_avl[3] = {  52, 0x03 },
		.odr_avl[4] = { 104, 0x04 },
		.odr_avl[5] = { 208, 0x05 },
		.odr_avl[6] = { 416, 0x06 },
	}
};

struct st_lsm6dsx_fs {
	u32 gain;
	u8 val;
};

#define ST_LSM6DSX_FS_LIST_SIZE		4
struct st_lsm6dsx_fs_table_entry {
	struct st_lsm6dsx_reg reg;
	struct st_lsm6dsx_fs fs_avl[ST_LSM6DSX_FS_LIST_SIZE];
};

static const struct st_lsm6dsx_fs_table_entry st_lsm6dsx_fs_table[] = {
	[ST_LSM6DSX_ID_ACC] = {
		.reg = {
			.addr = ST_LSM6DSX_REG_ACC_FS_ADDR,
			.mask = ST_LSM6DSX_REG_ACC_FS_MASK,
		},
		.fs_avl[0] = {  ST_LSM6DSX_ACC_FS_2G_GAIN, 0x0 },
		.fs_avl[1] = {  ST_LSM6DSX_ACC_FS_4G_GAIN, 0x2 },
		.fs_avl[2] = {  ST_LSM6DSX_ACC_FS_8G_GAIN, 0x3 },
		.fs_avl[3] = { ST_LSM6DSX_ACC_FS_16G_GAIN, 0x1 },
	},
	[ST_LSM6DSX_ID_GYRO] = {
		.reg = {
			.addr = ST_LSM6DSX_REG_GYRO_FS_ADDR,
			.mask = ST_LSM6DSX_REG_GYRO_FS_MASK,
		},
		.fs_avl[0] = {  ST_LSM6DSX_GYRO_FS_245_GAIN, 0x0 },
		.fs_avl[1] = {  ST_LSM6DSX_GYRO_FS_500_GAIN, 0x1 },
		.fs_avl[2] = { ST_LSM6DSX_GYRO_FS_1000_GAIN, 0x2 },
		.fs_avl[3] = { ST_LSM6DSX_GYRO_FS_2000_GAIN, 0x3 },
	}
};

static const struct st_lsm6dsx_settings st_lsm6dsx_sensor_settings[] = {
	{
		.wai = 0x69,
		.max_fifo_size = 8192,
		.id = {
			[0] = ST_LSM6DS3_ID,
		},
	},
	{
		.wai = 0x69,
		.max_fifo_size = 4096,
		.id = {
			[0] = ST_LSM6DS3H_ID,
		},
	},
	{
		.wai = 0x6a,
		.max_fifo_size = 4096,
		.id = {
			[0] = ST_LSM6DSL_ID,
			[1] = ST_LSM6DSM_ID,
		},
	},
};

#define ST_LSM6DSX_CHANNEL(chan_type, addr, mod, scan_idx)		\
{									\
	.type = chan_type,						\
	.address = addr,						\
	.modified = 1,							\
	.channel2 = mod,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
			      BIT(IIO_CHAN_INFO_SCALE),			\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.scan_index = scan_idx,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_LE,					\
	},								\
}

#define ST_LSM6DSX_CHANNEL_EVENT(chan_type, scan_idx)		\
{									\
	.type = chan_type,						\
	.event_spec = st_lsm6dsx_events,				\
	.num_event_specs = ARRAY_SIZE(st_lsm6dsx_events),		\
	.scan_index = scan_idx,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_LE,					\
	},								\
}

static const struct iio_event_spec st_lsm6dsx_events[] = {
	{
		.type = IIO_EV_TYPE_SINGLE_TAP,
		.dir = IIO_EV_DIR_NONE,
		.mask_separate =  BIT(IIO_EV_INFO_VALUE) |
			BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_DOUBLE_TAP,
		.dir = IIO_EV_DIR_NONE,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_PEDOMETER,
		.dir = IIO_EV_DIR_NONE,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_TILT,
		.dir = IIO_EV_DIR_NONE,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_MOTION,
		.dir = IIO_EV_DIR_NONE,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_FREEFALL,
		.dir = IIO_EV_DIR_NONE,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};




static const struct iio_chan_spec st_lsm6dsx_acc_channels[] = {
	ST_LSM6DSX_CHANNEL(IIO_ACCEL, ST_LSM6DSX_REG_ACC_OUT_X_L_ADDR,
			   IIO_MOD_X, 0),
	ST_LSM6DSX_CHANNEL(IIO_ACCEL, ST_LSM6DSX_REG_ACC_OUT_Y_L_ADDR,
			   IIO_MOD_Y, 1),
	ST_LSM6DSX_CHANNEL(IIO_ACCEL, ST_LSM6DSX_REG_ACC_OUT_Z_L_ADDR,
			   IIO_MOD_Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
	//ST_LSM6DSX_CHANNEL_EVENT(IIO_ACCEL, 4),
};

static const struct iio_chan_spec st_lsm6dsx_gyro_channels[] = {
	ST_LSM6DSX_CHANNEL(IIO_ANGL_VEL, ST_LSM6DSX_REG_GYRO_OUT_X_L_ADDR,
			   IIO_MOD_X, 0),
	ST_LSM6DSX_CHANNEL(IIO_ANGL_VEL, ST_LSM6DSX_REG_GYRO_OUT_Y_L_ADDR,
			   IIO_MOD_Y, 1),
	ST_LSM6DSX_CHANNEL(IIO_ANGL_VEL, ST_LSM6DSX_REG_GYRO_OUT_Z_L_ADDR,
			   IIO_MOD_Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3)
};

int st_lsm6dsx_write_with_mask_no_shift(struct st_lsm6dsx_hw *hw, u8 addr, u8 mask,
			       u8 val)
{
	u8 data;
	int err;

	mutex_lock(&hw->lock);

	err = hw->tf->read(hw->dev, addr, sizeof(data), &data);
	if (err < 0) {
		dev_err(hw->dev, "failed to read %02x register\n", addr);
		goto out;
	}

	data &= ~mask;
	data |= val;

	err = hw->tf->write(hw->dev, addr, sizeof(data), &data);
	if (err < 0)
		dev_err(hw->dev, "failed to write %02x register\n", addr);

out:
	mutex_unlock(&hw->lock);

	return err;
}


static int st_lsm6dsx_write_event_value(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 enum iio_event_type type,
				 enum iio_event_direction dir,
				 enum iio_event_info info, int val, int val2)
{
	//printk("POPPO WRITE VALUE ch: %d %d %d %d \r\n",chan->channel,info, val, val2);
	return 0;
}

static int st_lsm6dsx_read_event_value(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir,
				enum iio_event_info info, int *val, int *val2)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(indio_dev);
	int err;
	u8 data;

	err = sensor->hw->tf->read(sensor->hw->dev, LSM6DSL_ACC_GYRO_TAP_SRC,
				   sizeof(u8),
				   &data);
	*val = data;
	printk("POPPO READ VALUE type: %d \r\n",type);
	return 1;
}

static int st_lsm6dsx_read_event_config(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(indio_dev);
	printk("POPPO READ EVENT CONFIG type: %d \r\n", type);
	return (sensor->event_mask & (1 << type)) >> type;
}

static int st_lsm6dsx_write_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     int state)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(indio_dev);
	int err = 0;

	printk("POPPO WRITE EVENT CONFIG type: %d state: %d \r\n", type, state);


	return err;
}

int st_lsm6dsx_write_with_mask(struct st_lsm6dsx_hw *hw, u8 addr, u8 mask,
			       u8 val)
{
	u8 data;
	int err;

	mutex_lock(&hw->lock);

	err = hw->tf->read(hw->dev, addr, sizeof(data), &data);
	if (err < 0) {
		dev_err(hw->dev, "failed to read %02x register\n", addr);
		goto out;
	}

	data = (data & ~mask) | ((val << __ffs(mask)) & mask);

	err = hw->tf->write(hw->dev, addr, sizeof(data), &data);
	if (err < 0)
		dev_err(hw->dev, "failed to write %02x register\n", addr);

out:
	mutex_unlock(&hw->lock);

	return err;
}

static int st_lsm6dsx_check_whoami(struct st_lsm6dsx_hw *hw, int id)
{
	int err, i, j;
	u8 data;

	for (i = 0; i < ARRAY_SIZE(st_lsm6dsx_sensor_settings); i++) {
		for (j = 0; j < ST_LSM6DSX_MAX_ID; j++) {
			if (id == st_lsm6dsx_sensor_settings[i].id[j])
				break;
		}
		if (j < ST_LSM6DSX_MAX_ID)
			break;
	}

	if (i == ARRAY_SIZE(st_lsm6dsx_sensor_settings)) {
		dev_err(hw->dev, "unsupported hw id [%02x]\n", id);
		return -ENODEV;
	}

	err = hw->tf->read(hw->dev, ST_LSM6DSX_REG_WHOAMI_ADDR, sizeof(data),
			   &data);
	if (err < 0) {
		dev_err(hw->dev, "failed to read whoami register\n");
		return err;
	}

	if (data != st_lsm6dsx_sensor_settings[i].wai) {
		dev_err(hw->dev, "unsupported whoami [%02x]\n", data);
		return -ENODEV;
	}

	hw->settings = &st_lsm6dsx_sensor_settings[i];

	return 0;
}

static int st_lsm6dsx_set_full_scale(struct st_lsm6dsx_sensor *sensor,
				     u32 gain)
{
	enum st_lsm6dsx_sensor_id id = sensor->id;
	int i, err;
	u8 val;

	for (i = 0; i < ST_LSM6DSX_FS_LIST_SIZE; i++)
		if (st_lsm6dsx_fs_table[id].fs_avl[i].gain == gain)
			break;

	if (i == ST_LSM6DSX_FS_LIST_SIZE)
		return -EINVAL;

	val = st_lsm6dsx_fs_table[id].fs_avl[i].val;
	err = st_lsm6dsx_write_with_mask(sensor->hw,
					 st_lsm6dsx_fs_table[id].reg.addr,
					 st_lsm6dsx_fs_table[id].reg.mask,
					 val);
	if (err < 0)
		return err;

	sensor->gain = gain;

	return 0;
}

static int st_lsm6dsx_check_odr(struct st_lsm6dsx_sensor *sensor, u16 odr,
				u8 *val)
{
	int i;

	for (i = 0; i < ST_LSM6DSX_ODR_LIST_SIZE; i++)
		if (st_lsm6dsx_odr_table[sensor->id].odr_avl[i].hz == odr)
			break;

	if (i == ST_LSM6DSX_ODR_LIST_SIZE)
		return -EINVAL;

	*val = st_lsm6dsx_odr_table[sensor->id].odr_avl[i].val;
	sensor->odr = odr;

	return 0;
}

static int st_lsm6dsx_set_odr(struct st_lsm6dsx_sensor *sensor, u16 odr)
{
	enum st_lsm6dsx_sensor_id id = sensor->id;
	int err;
	u8 val;

	printk("POPPO SET ODR %d \r\n",odr);

	err = st_lsm6dsx_check_odr(sensor, odr, &val);
	if (err < 0)
		return err;

	err = st_lsm6dsx_write_with_mask(sensor->hw,
					  st_lsm6dsx_odr_table[id].reg.addr,
					  st_lsm6dsx_odr_table[id].reg.mask,
					  val);

	if( err < 0)
		return err;

	if(odr == 0)
		sensor->hw->enable_mask &= ~BIT(id);
	else
		sensor->hw->enable_mask |= BIT(id);

	return err;
}


static int st_lsm6dsx_get_odr(struct st_lsm6dsx_sensor *sensor)
{
	enum st_lsm6dsx_sensor_id id = sensor->id;
	int err;
	int i;
	u8 val;

	err = sensor->hw->tf->read(sensor->hw->dev,
				   st_lsm6dsx_odr_table[id].reg.addr,
				   sizeof(val), &val);

	printk("POPPO GET ODR %d \r\n",val);

	for (i = 0; i < ST_LSM6DSX_ODR_LIST_SIZE; i++)
		if (st_lsm6dsx_odr_table[sensor->id].odr_avl[i].val == (val >> 4)) {
			return st_lsm6dsx_odr_table[sensor->id].odr_avl[i].hz;
		}

	return -EINVAL;
}


int st_lsm6dsx_sensor_enable(struct st_lsm6dsx_sensor *sensor)
{
	int err;

	err = st_lsm6dsx_set_odr(sensor, sensor->odr);
	if (err < 0)
		return err;

	sensor->hw->enable_mask |= BIT(sensor->id);

	return 0;
}

int st_lsm6dsx_sensor_disable(struct st_lsm6dsx_sensor *sensor)
{
	enum st_lsm6dsx_sensor_id id = sensor->id;
	int err;

	err = st_lsm6dsx_write_with_mask(sensor->hw,
					 st_lsm6dsx_odr_table[id].reg.addr,
					 st_lsm6dsx_odr_table[id].reg.mask, 0);
	if (err < 0)
		return err;

	sensor->hw->enable_mask &= ~BIT(id);

	return 0;
}

static int st_lsm6dsx_read_oneshot(struct st_lsm6dsx_sensor *sensor,
				   u8 addr, int *val)
{
	int err, delay;
	__le16 data;

	if(!(sensor->hw->enable_mask & BIT(sensor->id)))
		return -EPERM;

	delay = 1000000 / sensor->odr;
	usleep_range(delay, 2 * delay);

	err = sensor->hw->tf->read(sensor->hw->dev, addr, sizeof(data),
				   (u8 *)&data);
	if (err < 0)
		return err;

	*val = (s16)data;

	return IIO_VAL_INT;
}

static int st_lsm6dsx_read_raw(struct iio_dev *iio_dev,
			       struct iio_chan_spec const *ch,
			       int *val, int *val2, long mask)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(iio_dev);
	int ret = 0;
	u8 data;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		//ret = iio_device_claim_direct_mode(iio_dev);
		mutex_lock(&iio_dev->mlock);
		if (ret)
			break;

		ret = st_lsm6dsx_read_oneshot(sensor, ch->address, val);
		mutex_unlock(&iio_dev->mlock);
		//iio_device_release_direct_mode(iio_dev);
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:

		*val = st_lsm6dsx_get_odr(sensor);
		if(*val == -EINVAL)
			ret = -EINVAL;
		else {
			ret = IIO_VAL_INT;
			sensor->odr = *val;
		}
		break;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = sensor->gain;
		ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int st_lsm6dsx_write_raw(struct iio_dev *iio_dev,
				struct iio_chan_spec const *chan,
				int val, int val2, long mask)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(iio_dev);
	int err = 0;

	//err = iio_device_claim_direct_mode(iio_dev);
	mutex_lock(&iio_dev->mlock);
	//if (err)
	//	return err;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		err = st_lsm6dsx_set_full_scale(sensor, val2);
		break;
	case IIO_CHAN_INFO_SAMP_FREQ: {
		err = st_lsm6dsx_set_odr(sensor, val);
		if(err >= 0)
			err = 0;
		break;
	}
	default:
		err = -EINVAL;
		break;
	}

	//iio_device_release_direct_mode(iio_dev);
	mutex_unlock(&iio_dev->mlock);

	return err;
}

static int st_lsm6dsx_set_watermark(struct iio_dev *iio_dev, unsigned int val)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(iio_dev);
	struct st_lsm6dsx_hw *hw = sensor->hw;
	int err, max_fifo_len;

	max_fifo_len = hw->settings->max_fifo_size / ST_LSM6DSX_SAMPLE_SIZE;
	if (val < 1 || val > max_fifo_len)
		return -EINVAL;

	err = st_lsm6dsx_update_watermark(sensor, val);
	if (err < 0)
		return err;

	sensor->watermark = val;

	return 0;
}

static ssize_t
st_lsm6dsx_sysfs_sampling_frequency_avail(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(dev_get_drvdata(dev));
	enum st_lsm6dsx_sensor_id id = sensor->id;
	int i, len = 0;

	for (i = 0; i < ST_LSM6DSX_ODR_LIST_SIZE; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%d ",
				 st_lsm6dsx_odr_table[id].odr_avl[i].hz);
	buf[len - 1] = '\n';

	return len;
}

static ssize_t st_lsm6dsx_sysfs_scale_avail(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(dev_get_drvdata(dev));
	enum st_lsm6dsx_sensor_id id = sensor->id;
	int i, len = 0;

	for (i = 0; i < ST_LSM6DSX_FS_LIST_SIZE; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "0.%06u ",
				 st_lsm6dsx_fs_table[id].fs_avl[i].gain);
	buf[len - 1] = '\n';

	return len;
}

static ssize_t st_lsm6dsx_event_en_set(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct st_lsm6dsx_sensor *sensor = iio_priv(dev_get_drvdata(dev));
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	enum st_lsm6dsx_event_type type = (u32)this_attr->address;
	bool state;
	int err;

	err = strtobool(buf, &state);
	if (err < 0)
		return err;

	if( (sensor->hw->enable_mask & BIT(sensor->id)) == 0 )
		return -EPERM;


	if(state >= 1)
		sensor->event_mask |= (1 << type);
	else
		sensor->event_mask &= ~(1 << type);

	mutex_lock(&indio_dev->mlock);

	/* Let's disable everything before */

	if( !(sensor->event_mask & (1 << ST_LSM6DSX_TAP)) )
		err |= lsm6dsl_disable_tap(sensor);

	if( !(sensor->event_mask & (1 << ST_LSM6DSX_DTAP)) )
		err |= lsm6dsl_disable_dtap(sensor);

	if( !(sensor->event_mask & (1 << ST_LSM6DSX_PEDOMETER)) )
		err |= lsm6dsl_disable_pedometer(sensor);

	if( !(sensor->event_mask & (1 << ST_LSM6DSX_WAKEUP)) )
		err |= lsm6dsl_disable_motion(sensor);

	if( !(sensor->event_mask & (1 << ST_LSM6DSX_TILT)) )
		err |= lsm6dsl_disable_tilt(sensor);

	if( !(sensor->event_mask & (1 << ST_LSM6DSX_FREEFALL)) )
		err |= lsm6dsl_disable_freefall(sensor);

	/* Enable */

	if( (sensor->event_mask & (1 << ST_LSM6DSX_FREEFALL)) )
		err |= lsm6dsl_enable_freefall(sensor,1);

	if( (sensor->event_mask & (1 << ST_LSM6DSX_TAP)) )
		err |= lsm6dsl_enable_tap(sensor,1);

	if( (sensor->event_mask & (1 << ST_LSM6DSX_DTAP)) )
		err |= lsm6dsl_enable_dtap(sensor,1);

	if( (sensor->event_mask & (1 << ST_LSM6DSX_WAKEUP)) )
		err |= lsm6dsl_enable_motion(sensor,1);

	if( (sensor->event_mask & (1 << ST_LSM6DSX_TILT)) )
		err |= lsm6dsl_enable_tilt(sensor,1);

	if( (sensor->event_mask & (1 << ST_LSM6DSX_PEDOMETER)) )
		err |= lsm6dsl_enable_pedometer(sensor,1);

	mutex_unlock(&indio_dev->mlock);

	return err ? err : len;
}


static ssize_t st_lsm6dsx_event_en_get(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(dev_get_drvdata(dev));
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	enum st_lsm6dsx_event_type type = (u32)this_attr->address;
	int len = 0;

	len += scnprintf(buf,3, "%d\n",(sensor->event_mask & (1 << type)) >> type);

	return len;
}

static IIO_DEV_ATTR_SAMP_FREQ_AVAIL(st_lsm6dsx_sysfs_sampling_frequency_avail);

static IIO_DEVICE_ATTR(in_accel_scale_available, 0444,
		       st_lsm6dsx_sysfs_scale_avail, NULL, 0);

static IIO_DEVICE_ATTR(in_anglvel_scale_available, 0444,
		       st_lsm6dsx_sysfs_scale_avail, NULL, 0);

static IIO_DEVICE_ATTR(event_tap_en, 0664,
		       st_lsm6dsx_event_en_get, st_lsm6dsx_event_en_set, ST_LSM6DSX_TAP);

static IIO_DEVICE_ATTR(event_dtap_en, 0664,
		       st_lsm6dsx_event_en_get, st_lsm6dsx_event_en_set, ST_LSM6DSX_DTAP);

static IIO_DEVICE_ATTR(event_tilt_en, 0664,
		       st_lsm6dsx_event_en_get, st_lsm6dsx_event_en_set, ST_LSM6DSX_TILT);

static IIO_DEVICE_ATTR(event_pedometer_en, 0664,
		       st_lsm6dsx_event_en_get, st_lsm6dsx_event_en_set, ST_LSM6DSX_PEDOMETER);

static IIO_DEVICE_ATTR(event_freefall_en, 0664,
		       st_lsm6dsx_event_en_get, st_lsm6dsx_event_en_set, ST_LSM6DSX_FREEFALL);

static IIO_DEVICE_ATTR(event_wakeup_en, 0664,
		       st_lsm6dsx_event_en_get, st_lsm6dsx_event_en_set, ST_LSM6DSX_WAKEUP);


static struct attribute *st_lsm6dsx_acc_attributes[] = {
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	&iio_dev_attr_in_accel_scale_available.dev_attr.attr,
	&iio_dev_attr_event_tap_en.dev_attr.attr,
	&iio_dev_attr_event_dtap_en.dev_attr.attr,
	&iio_dev_attr_event_tilt_en.dev_attr.attr,
	&iio_dev_attr_event_pedometer_en.dev_attr.attr,
	&iio_dev_attr_event_freefall_en.dev_attr.attr,
	&iio_dev_attr_event_wakeup_en.dev_attr.attr,
	NULL,
};

static const struct attribute_group st_lsm6dsx_acc_attribute_group = {
	.attrs = st_lsm6dsx_acc_attributes,
};

static const struct iio_info st_lsm6dsx_acc_info = {
	.driver_module = THIS_MODULE,
	.attrs = &st_lsm6dsx_acc_attribute_group,
	.read_raw = st_lsm6dsx_read_raw,
	.write_raw = st_lsm6dsx_write_raw,
	.hwfifo_set_watermark = st_lsm6dsx_set_watermark,
	.read_event_config = st_lsm6dsx_read_event_config,
	.write_event_config = st_lsm6dsx_write_event_config,
	.read_event_value = st_lsm6dsx_read_event_value,
	.write_event_value = st_lsm6dsx_write_event_value,
};

static struct attribute *st_lsm6dsx_gyro_attributes[] = {
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	&iio_dev_attr_in_anglvel_scale_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group st_lsm6dsx_gyro_attribute_group = {
	.attrs = st_lsm6dsx_gyro_attributes,
};

static const struct iio_info st_lsm6dsx_gyro_info = {
	.driver_module = THIS_MODULE,
	.attrs = &st_lsm6dsx_gyro_attribute_group,
	.read_raw = st_lsm6dsx_read_raw,
	.write_raw = st_lsm6dsx_write_raw,
	.hwfifo_set_watermark = st_lsm6dsx_set_watermark,
};

static const unsigned long st_lsm6dsx_available_scan_masks[] = {0x7, 0x0};

static int st_lsm6dsx_of_get_drdy_pin(struct st_lsm6dsx_hw *hw, int *drdy_pin)
{
	struct device_node *np = hw->dev->of_node;

	if (!np)
		return -EINVAL;

	return of_property_read_u32(np, "st,drdy-int-pin", drdy_pin);
}

static int st_lsm6dsx_get_drdy_reg(struct st_lsm6dsx_hw *hw, u8 *drdy_reg)
{
	int err = 0, drdy_pin;

	if (st_lsm6dsx_of_get_drdy_pin(hw, &drdy_pin) < 0) {
		struct st_sensors_platform_data *pdata;
		struct device *dev = hw->dev;

		pdata = (struct st_sensors_platform_data *)dev->platform_data;
		drdy_pin = pdata ? pdata->drdy_int_pin : 1;
	}

	switch (drdy_pin) {
	case 1:
		*drdy_reg = ST_LSM6DSX_REG_INT1_ADDR;
		break;
	case 2:
		*drdy_reg = ST_LSM6DSX_REG_INT2_ADDR;
		break;
	default:
		dev_err(hw->dev, "unsupported data ready pin\n");
		err = -EINVAL;
		break;
	}

	return err;
}

static int st_lsm6dsx_init_device(struct st_lsm6dsx_hw *hw)
{
	u8 data, drdy_int_reg;
	int err;

	data = ST_LSM6DSX_REG_RESET_MASK;
	err = hw->tf->write(hw->dev, ST_LSM6DSX_REG_RESET_ADDR, sizeof(data),
			    &data);
	if (err < 0)
		return err;

	msleep(200);

	/* latch interrupts */
	err = st_lsm6dsx_write_with_mask(hw, ST_LSM6DSX_REG_LIR_ADDR,
					 ST_LSM6DSX_REG_LIR_MASK, 1);
	if (err < 0)
		return err;

	/* enable Block Data Update */
	err = st_lsm6dsx_write_with_mask(hw, ST_LSM6DSX_REG_BDU_ADDR,
					 ST_LSM6DSX_REG_BDU_MASK, 1);
	if (err < 0)
		return err;

	err = st_lsm6dsx_write_with_mask(hw, ST_LSM6DSX_REG_ROUNDING_ADDR,
					 ST_LSM6DSX_REG_ROUNDING_MASK, 1);
	if (err < 0)
		return err;

	/* enable FIFO watermak interrupt */
	err = st_lsm6dsx_get_drdy_reg(hw, &drdy_int_reg);
	if (err < 0)
		return err;

	return st_lsm6dsx_write_with_mask(hw, drdy_int_reg,
					  ST_LSM6DSX_REG_FIFO_FTH_IRQ_MASK, 1);
}

static struct iio_dev *st_lsm6dsx_alloc_iiodev(struct st_lsm6dsx_hw *hw,
					       enum st_lsm6dsx_sensor_id id,
					       const char *name)
{
	struct st_lsm6dsx_sensor *sensor;
	struct iio_dev *iio_dev;

	iio_dev = devm_iio_device_alloc(hw->dev, sizeof(*sensor));
	if (!iio_dev)
		return NULL;

	iio_dev->modes = INDIO_DIRECT_MODE;
	iio_dev->dev.parent = hw->dev;
	iio_dev->available_scan_masks = st_lsm6dsx_available_scan_masks;

	sensor = iio_priv(iio_dev);
	sensor->id = id;
	sensor->hw = hw;
	sensor->odr = st_lsm6dsx_odr_table[id].odr_avl[0].hz;
	sensor->gain = st_lsm6dsx_fs_table[id].fs_avl[0].gain;
	sensor->watermark = 1;
	sensor->event_mask = 0;

	switch (id) {
	case ST_LSM6DSX_ID_ACC:
		iio_dev->channels = st_lsm6dsx_acc_channels;
		iio_dev->num_channels = ARRAY_SIZE(st_lsm6dsx_acc_channels);
		iio_dev->info = &st_lsm6dsx_acc_info;

		sensor->decimator_mask = ST_LSM6DSX_REG_ACC_DEC_MASK;
		scnprintf(sensor->name, sizeof(sensor->name), "%s_accel",
			  name);
		break;
	case ST_LSM6DSX_ID_GYRO:
		iio_dev->channels = st_lsm6dsx_gyro_channels;
		iio_dev->num_channels = ARRAY_SIZE(st_lsm6dsx_gyro_channels);
		iio_dev->info = &st_lsm6dsx_gyro_info;

		sensor->decimator_mask = ST_LSM6DSX_REG_GYRO_DEC_MASK;
		scnprintf(sensor->name, sizeof(sensor->name), "%s_gyro",
			  name);
		break;
	default:
		return NULL;
	}
	iio_dev->name = sensor->name;

	return iio_dev;
}

int st_lsm6dsx_probe(struct device *dev, int irq, int hw_id, const char *name,
		     const struct st_lsm6dsx_transfer_function *tf_ops)
{
	struct st_lsm6dsx_hw *hw;
	int i, err;

	hw = devm_kzalloc(dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return -ENOMEM;

	dev_set_drvdata(dev, (void *)hw);

	mutex_init(&hw->lock);
	mutex_init(&hw->fifo_lock);

	hw->dev = dev;
	hw->irq = irq;
	hw->tf = tf_ops;

	err = st_lsm6dsx_check_whoami(hw, hw_id);
	if (err < 0)
		return err;

	for (i = 0; i < ST_LSM6DSX_ID_MAX; i++) {
		hw->iio_devs[i] = st_lsm6dsx_alloc_iiodev(hw, i, name);
		if (!hw->iio_devs[i])
			return -ENOMEM;
	}

	err = st_lsm6dsx_init_device(hw);
	if (err < 0)
		return err;

	if (hw->irq > 0) {
		err = st_lsm6dsx_fifo_setup(hw);
		if (err < 0)
			return err;
	}

	for (i = 0; i < ST_LSM6DSX_ID_MAX; i++) {
		err = devm_iio_device_register(hw->dev, hw->iio_devs[i]);
		if (err)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL(st_lsm6dsx_probe);

MODULE_AUTHOR("Lorenzo Bianconi <lorenzo.bianconi@st.com>");
MODULE_AUTHOR("Denis Ciocca <denis.ciocca@st.com>");
MODULE_DESCRIPTION("STMicroelectronics st_lsm6dsx driver");
MODULE_LICENSE("GPL v2");

