// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for Digibest ISDB2056 device (isdb2056_device.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "isdb2056_device.h"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "px4_device_params.h"
#include "firmware.h"

#define ISDB2056_DEVICE_TS_SYNC_COUNT	4
#define ISDB2056_DEVICE_TS_SYNC_SIZE	(188 * ISDB2056_DEVICE_TS_SYNC_COUNT)

struct isdb2056_stream_context {
	struct ptx_chrdev *chrdev;
	u8 remain_buf[ISDB2056_DEVICE_TS_SYNC_SIZE];
	size_t remain_len;
};

static void isdb2056_device_release(struct kref *kref);

static int isdb2056_backend_set_power(struct isdb2056_device *isdb2056,
				      bool state)
{
	int ret = 0;
	struct it930x_bridge *it930x = &isdb2056->it930x;

	dev_dbg(isdb2056->dev,
		"isdb2056_backend_set_power: %s\n", (state) ? "true" : "false");

	if (!state && !atomic_read(&isdb2056->available))
		return 0;

	if (state) {
		ret = it930x_write_gpio(it930x, 3, false);
		if (ret)
			return ret;

		msleep(100);

		ret = it930x_write_gpio(it930x, 2, true);
		if (ret)
			return ret;

		msleep(20);
	} else {
		it930x_write_gpio(it930x, 2, false);
		it930x_write_gpio(it930x, 3, true);
	}

	return 0;
}

static int isdb2056_backend_init(struct isdb2056_device *isdb2056)
{
	int ret = 0;
	struct isdb2056_chrdev *chrdev2056 = &isdb2056->chrdev2056;

	ret = tc90522_init(&chrdev2056->tc90522_t);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_backend_init: tc90522_init() (t) failed. (ret: %d)\n",
			ret);
		return ret;
	}

	ret = tc90522_init(&chrdev2056->tc90522_s);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_backend_init: tc90522_init() (s) failed. (ret: %d)\n",
			ret);
		return ret;
	}

	if (isdb2056->isdb2056_model == ISDB2056N_MODEL) {
		ret = tc90522_init(&chrdev2056->tc90522_s0);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_backend_init: tc90522_init() (s0) failed. (ret: %d)\n",
				ret);
			return ret;
		}
	}

	ret = r850_init(&chrdev2056->r850);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_backend_init: r850_init() failed. (ret: %d)\n",
			ret);
		return ret;
	}

	ret = rt710_init(&chrdev2056->rt710);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_backend_init: rt710_init() failed. (ret: %d)\n",
			ret);
		return ret;
	}

	return 0;
}

static int isdb2056_backend_term(struct isdb2056_device *isdb2056)
{
	struct isdb2056_chrdev *chrdev2056 = &isdb2056->chrdev2056;

	r850_term(&chrdev2056->r850);
	rt710_term(&chrdev2056->rt710);

	tc90522_term(&chrdev2056->tc90522_t);
	tc90522_term(&chrdev2056->tc90522_s);
	if (isdb2056->isdb2056_model == ISDB2056N_MODEL)
		tc90522_term(&chrdev2056->tc90522_s0);

	return 0;
}

static void isdb2056_device_stream_process(struct ptx_chrdev *chrdev,
					   u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (likely(remain)) {
		u32 i = 0;
		bool sync_remain = false;

		while (true) {
			if (likely(((i + 1) * 188) <= remain)) {
				if (unlikely(p[i * 188] != 0x47))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (unlikely(i < ISDB2056_DEVICE_TS_SYNC_COUNT)) {
			p++;
			remain--;
			continue;
		}

		ptx_chrdev_put_stream(chrdev, p, 188 * i);

		p += 188 * i;
		remain -= 188 * i;

		if (unlikely(sync_remain))
			break;
	}

	*buf = p;
	*len = remain;

	return;
}

static int isdb2056_device_stream_handler(void *context, void *buf, u32 len)
{
	struct isdb2056_stream_context *stream_ctx = context;
	u8 *ctx_remain_buf = stream_ctx->remain_buf;
	u32 ctx_remain_len = stream_ctx->remain_len;
	u8 *p = buf;
	u32 remain = len;

	if (unlikely(ctx_remain_len)) {
		if (likely((ctx_remain_len + len) >= ISDB2056_DEVICE_TS_SYNC_SIZE)) {
			u32 t = ISDB2056_DEVICE_TS_SYNC_SIZE - ctx_remain_len;

			memcpy(ctx_remain_buf + ctx_remain_len, p, t);
			ctx_remain_len = ISDB2056_DEVICE_TS_SYNC_SIZE;

			isdb2056_device_stream_process(stream_ctx->chrdev,
						  &ctx_remain_buf,
						  &ctx_remain_len);
			if (likely(!ctx_remain_len)) {
				p += t;
				remain -= t;
			}

			stream_ctx->remain_len = 0;
		} else {
			memcpy(ctx_remain_buf + ctx_remain_len, p, len);
			stream_ctx->remain_len += len;

			return 0;
		}
	}

	isdb2056_device_stream_process(stream_ctx->chrdev, &p, &remain);

	if (unlikely(remain)) {
		memcpy(stream_ctx->remain_buf, p, remain);
		stream_ctx->remain_len = remain;
	}

	return 0;
}

static int isdb2056_chrdev_init(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "isdb2056_chrdev_init\n");

	chrdev->params.system = PTX_UNSPECIFIED_SYSTEM;
	return 0;
}

static int isdb2056_chrdev_term(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "isdb2056_chrdev_term\n");
	return 0;
}

static struct tc90522_regbuf tc_init_t[] = {
	{ 0xb0, NULL, { 0xa0 } },
	{ 0xb2, NULL, { 0x3d } },
	{ 0xb3, NULL, { 0x25 } },
	{ 0xb4, NULL, { 0x8b } },
	{ 0xb5, NULL, { 0x4b } },
	{ 0xb6, NULL, { 0x3f } },
	{ 0xb7, NULL, { 0xff } },
	{ 0xb8, NULL, { 0xc0 } },
};

static struct tc90522_regbuf tc_init_s[] = {
	{ 0x15, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x00 } },
};

static int isdb2056_chrdev_open(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;
	struct isdb2056_device *isdb2056 = container_of(chrdev2056,
							struct isdb2056_device,
							chrdev2056);
	struct r850_system_config sys;

	dev_dbg(isdb2056->dev,
		"isdb2056_chrdev_open %u\n", chrdev_group->id);

	ret = isdb2056_backend_set_power(isdb2056, true);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: isdb2056_backend_set_power(true) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend_power;
	}

	ret = isdb2056_backend_init(isdb2056);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: isdb2056_backend_init() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend_init;
	}

	/* Initialization for ISDB-T */

	ret = tc90522_write_multiple_regs(&chrdev2056->tc90522_t,
					  tc_init_t, ARRAY_SIZE(tc_init_t));
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: tc90522_write_multiple_regs(tc_init_t) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_backend;
	}

	/* disable ts pins */
	ret = tc90522_enable_ts_pins_t(&chrdev2056->tc90522_t, false);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: tc90522_enable_ts_pins_t(false) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* sleep */
	ret = tc90522_sleep_t(&chrdev2056->tc90522_t, true);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: tc90522_sleep_t(true) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	sys.system = R850_SYSTEM_ISDB_T;
	sys.bandwidth = R850_BANDWIDTH_6M;
	sys.if_freq = 4063;

	ret = r850_set_system(&chrdev2056->r850, &sys);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: r850_set_system() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* Initialization for ISDB-S */

	ret = tc90522_write_multiple_regs(&chrdev2056->tc90522_s,
					  tc_init_s, ARRAY_SIZE(tc_init_s));
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: tc90522_write_multiple_regs(tc_init_s) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* disable ts pins */
	ret = tc90522_enable_ts_pins_s(&chrdev2056->tc90522_s, false);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: tc90522_enable_ts_pins_s(false) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	/* sleep */
	ret = tc90522_sleep_s(&chrdev2056->tc90522_s, true);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_open %u: tc90522_sleep_s(true) failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	kref_get(&isdb2056->kref);
	return 0;

fail_backend:
	isdb2056_backend_term(isdb2056);

fail_backend_init:
	isdb2056_backend_set_power(isdb2056, false);

fail_backend_power:
	return ret;
}

static int isdb2056_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;
	struct isdb2056_device *isdb2056 = container_of(chrdev2056,
							struct isdb2056_device,
							chrdev2056);

	dev_dbg(isdb2056->dev,
		"isdb2056_chrdev_release %u: kref count: %u\n",
		chrdev_group->id, kref_read(&isdb2056->kref));

	isdb2056_backend_term(isdb2056);
	isdb2056_backend_set_power(isdb2056, false);

	kref_put(&isdb2056->kref, isdb2056_device_release);
	return 0;
}

static int isdb2056_chrdev_tune(struct ptx_chrdev *chrdev,
				struct ptx_tune_params *params)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;
	struct isdb2056_device *isdb2056 = container_of(chrdev2056,
							struct isdb2056_device,
							chrdev2056);
	bool tuner_locked;
	s32 ss;

	dev_dbg(isdb2056->dev,
		"isdb2056_chrdev_tune %u\n", chrdev_group->id);

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x47, 0x30);
		if (ret)
			break;

		ret = tc90522_set_agc_t(&chrdev2056->tc90522_t, false);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_set_agc_t(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_sleep_s(&chrdev2056->tc90522_s, true);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_sleep_s(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x0e, 0x77);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x0f, 0x10);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x71, 0x20);
		if (ret)
			break;

		ret = tc90522_sleep_t(&chrdev2056->tc90522_t, false);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_sleep_t(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x76, 0x0c);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x1f, 0x30);
		if (ret)
			break;

		ret = r850_wakeup(&chrdev2056->r850);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: r850_wakeup() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = r850_set_frequency(&chrdev2056->r850, params->freq);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: r850_set_frequency(%u) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			ret = r850_is_pll_locked(&chrdev2056->r850,
						 &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: r850_is_pll_locked() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		} else if (!tuner_locked) {
			/* PLL error */
			dev_dbg(isdb2056->dev,
				"isdb2056_chrdev_tune %u: PLL is NOT locked.\n",
				chrdev_group->id);
			ret = -EAGAIN;
			break;
		}

		dev_dbg(isdb2056->dev,
			"isdb2056_chrdev_tune %u: PLL is locked. count: %d\n",
			chrdev_group->id, i);

		ret = tc90522_set_agc_t(&chrdev2056->tc90522_t, true);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_set_agc_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x71, 0x01);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x72, 0x25);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x75, 0x00);
		if (ret)
			break;

		msleep(100);

		break;

	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_set_agc_s(&chrdev2056->tc90522_s, false);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_set_agc_s(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x0e, 0x11);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x0f, 0x70);
		if (ret)
			break;

		ret = tc90522_sleep_t(&chrdev2056->tc90522_t, true);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_sleep_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		switch (isdb2056->isdb2056_model) {
		case ISDB2056_MODEL:
			ret = tc90522_write_reg(&chrdev2056->tc90522_s, 0x07, 0x77);
			break;
		case ISDB2056N_MODEL:
			ret = tc90522_write_reg(&chrdev2056->tc90522_s0, 0x07, 0x77);
			break;
		default:
			dev_err(isdb2056->dev, "isdb2056_chrdev_tune: undefined model (model: %d)\n", isdb2056->isdb2056_model);
			ret = -EINVAL;
			break;
		}
		if (ret)
			break;

		switch (isdb2056->isdb2056_model) {
		case ISDB2056_MODEL:
			ret = tc90522_write_reg(&chrdev2056->tc90522_s, 0x08, 0x10);
			break;
		case ISDB2056N_MODEL:
			ret = tc90522_write_reg(&chrdev2056->tc90522_s0, 0x08, 0x37);
			break;
		default:
			dev_err(isdb2056->dev, "isdb2056_chrdev_tune: undefined model (model: %d)\n", isdb2056->isdb2056_model);
			ret = -EINVAL;
			break;
		}
		if (ret)
			break;

		ret = tc90522_sleep_s(&chrdev2056->tc90522_s, false);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_sleep_s(false) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		ret = tc90522_write_reg(&chrdev2056->tc90522_s, 0x04, 0x02);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_s, 0x8e, 0x02);
		if (ret)
			break;

		ret = tc90522_write_reg(&chrdev2056->tc90522_t, 0x1f, 0x20);
		if (ret)
			break;

		ret = rt710_set_params(&chrdev2056->rt710,
				       params->freq, 28860, 4);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: rt710_set_params(%u, 28860, 4) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
			break;
		}

		i = 50;
		while (i--) {
			ret = rt710_is_pll_locked(&chrdev2056->rt710,
						  &tuner_locked);
			if (!ret && tuner_locked)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: rt710_is_pll_locked() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		} else if (!tuner_locked) {
			/* PLL error */
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: PLL is NOT locked.\n",
				chrdev_group->id);
			ret = -EAGAIN;
			break;
		}

		rt710_get_rf_signal_strength(&chrdev2056->rt710, &ss);
		dev_dbg(isdb2056->dev,
			"isdb2056_chrdev_tune %u: PLL is locked. count: %d, signal strength: %d.%03ddBm\n",
			chrdev_group->id, i, ss / 1000, -ss % 1000);

		ret = tc90522_set_agc_s(&chrdev2056->tc90522_s, true);
		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_tune %u: tc90522_set_agc_s(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			break;
		}

		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int isdb2056_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	int ret = 0;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_is_signal_locked_t(&chrdev2056->tc90522_t,
						locked);
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_is_signal_locked_s(&chrdev2056->tc90522_s,
						locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int isdb2056_chrdev_set_stream_id(struct ptx_chrdev *chrdev,
					 u16 stream_id)
{
	int ret = 0, i;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;
	struct isdb2056_device *isdb2056 = container_of(chrdev2056,
							struct isdb2056_device,
							chrdev2056);
	struct tc90522_demod *tc90522_s = &chrdev2056->tc90522_s;
	u16 tsid, tsid2;

	dev_dbg(isdb2056->dev,
		"isdb2056_chrdev_set_stream_id %u\n", chrdev_group->id);

	if (chrdev->current_system != PTX_ISDB_S_SYSTEM)
		return -EINVAL;

	if (stream_id < 12) {
		i = 100;
		while (i--) {
			ret = tc90522_tmcc_get_tsid_s(tc90522_s,
						      stream_id, &tsid);
			if ((!ret && tsid) || ret == -EINVAL)
				break;

			msleep(10);
		}

		if (ret) {
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_set_stream_id_s %u: tc90522_tmcc_get_tsid_s() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			return ret;
		}

		if (!tsid) {
			ret = -EAGAIN;
			return ret;
		}
	} else {
		tsid = stream_id;
	}

	ret = tc90522_set_tsid_s(tc90522_s, tsid);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_set_stream_id_s %u: tc90522_set_tsid_s(0x%x) failed. (ret: %d)\n",
			chrdev_group->id, tsid, ret);
		return ret;
	}

	/* check slot */

	i = 100;
	while(i--) {
		ret = tc90522_get_tsid_s(tc90522_s, &tsid2);
		if (!ret && tsid2 == tsid)
			break;

		msleep(10);
	}

	if (tsid2 != tsid)
		ret = -EAGAIN;

	return ret;
}

static int isdb2056_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;
	struct isdb2056_device *isdb2056 = container_of(chrdev2056,
							struct isdb2056_device,
							chrdev2056);
	struct isdb2056_stream_context *stream_ctx = isdb2056->stream_ctx;


	dev_dbg(isdb2056->dev,
		"isdb2056_chrdev_start_capture %u\n", chrdev_group->id);

	ret = it930x_purge_psb(&isdb2056->it930x,
			       px4_device_params.psb_purge_timeout);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_start_capture %u: it930x_purge_psb() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail;
	}

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_enable_ts_pins_t(&chrdev2056->tc90522_t, true);
		if (ret)
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_start_capture %u: tc90522_enable_ts_pins_t(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);

		break;

	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_enable_ts_pins_s(&chrdev2056->tc90522_s, true);
		if (ret)
			dev_err(isdb2056->dev,
				"isdb2056_chrdev_start_capture %u: tc90522_enable_ts_pins_s(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);

		break;

	default:
		break;
	}

	if (ret)
		goto fail_tc;

	stream_ctx->remain_len = 0;

	ret = itedtv_bus_start_streaming(&isdb2056->it930x.bus,
					 isdb2056_device_stream_handler,
					 stream_ctx);
	if (ret) {
		dev_err(isdb2056->dev,
			"isdb2056_chrdev_start_capture %u: itedtv_bus_start_streaming() failed. (ret: %d)\n",
			chrdev_group->id, ret);
			goto fail_bus;
	}

	return 0;

fail_bus:

fail_tc:
	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(&chrdev2056->tc90522_t, false);
		break;

	case PTX_ISDB_S_SYSTEM:
		tc90522_enable_ts_pins_s(&chrdev2056->tc90522_s, false);
		break;

	default:
		break;
	}

fail:
	return ret;
}

static int isdb2056_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;
	struct isdb2056_device *isdb2056 = container_of(chrdev2056,
							struct isdb2056_device,
							chrdev2056);

	dev_dbg(isdb2056->dev,
		"isdb2056_chrdev_stop_capture %u\n", chrdev_group->id);

	itedtv_bus_stop_streaming(&isdb2056->it930x.bus);

	if (!atomic_read(&isdb2056->available))
		return 0;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		tc90522_enable_ts_pins_t(&chrdev2056->tc90522_t, false);
		break;

	case PTX_ISDB_S_SYSTEM:
		tc90522_enable_ts_pins_s(&chrdev2056->tc90522_s, false);
		break;

	default:
		break;
	}

	return 0;
}

static int isdb2056_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return (status) ? isdb2056_chrdev_start_capture(chrdev)
			: isdb2056_chrdev_stop_capture(chrdev);
}

static int isdb2056_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	int ret = 0;
	struct isdb2056_chrdev *chrdev2056 = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = tc90522_get_cndat_t(&chrdev2056->tc90522_t, value);
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = tc90522_get_cn_s(&chrdev2056->tc90522_s, (u16 *)value);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct ptx_chrdev_operations isdb2056_chrdev_ops = {
	.init = isdb2056_chrdev_init,
	.term = isdb2056_chrdev_term,
	.open = isdb2056_chrdev_open,
	.release = isdb2056_chrdev_release,
	.tune = isdb2056_chrdev_tune,
	.check_lock = isdb2056_chrdev_check_lock,
	.set_stream_id = isdb2056_chrdev_set_stream_id,
	.set_lnb_voltage = NULL,
	.set_capture = isdb2056_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = isdb2056_chrdev_read_cnr_raw
};

static int isdb2056_device_load_config(struct isdb2056_device *isdb2056,
				       struct ptx_chrdev_config *chrdev_config)
{
	int ret = 0, i;
	struct device *dev = isdb2056->dev;
	struct it930x_bridge *it930x = &isdb2056->it930x;
	struct it930x_stream_input *input = &it930x->config.input[0];
	struct isdb2056_chrdev *chrdev2056 = &isdb2056->chrdev2056;
	u8 tmp;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev,
			"isdb2056_load_config: it930x_read_reg(0x4979) failed.\n");
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM error.\n");
		return ret;
	}

	chrdev_config->system_cap = PTX_ISDB_T_SYSTEM | PTX_ISDB_S_SYSTEM;

	input->enable = true;
	input->is_parallel = false;
	input->port_number = 0;
	input->slave_number = 0;
	input->i2c_bus = 3;
	input->i2c_addr = 0x10;
	input->packet_len = 188;
	input->sync_byte = 0x47;

	chrdev2056->tc90522_t.dev = dev;
	chrdev2056->tc90522_t.i2c = &it930x->i2c_master[2];
	chrdev2056->tc90522_t.i2c_addr = 0x10;
	chrdev2056->tc90522_t.is_secondary = false;

	// This structure (tc90522_s0) is only required by ISDB2056N, but it is initialized in common just to be safe
	chrdev2056->tc90522_s0.dev = dev;
	chrdev2056->tc90522_s0.i2c = &it930x->i2c_master[2];
	chrdev2056->tc90522_s0.i2c_addr = 0x11;
	chrdev2056->tc90522_s0.is_secondary = false;

	chrdev2056->tc90522_s.dev = dev;
	chrdev2056->tc90522_s.i2c = &it930x->i2c_master[2];
	switch (isdb2056->isdb2056_model) {
	case ISDB2056_MODEL:
		// ISDB2056 TS demodulator: Toshiba TC90532XBG
		chrdev2056->tc90522_s.i2c_addr = 0x11;
		chrdev2056->tc90522_s.is_secondary = false;
		break;
	case ISDB2056N_MODEL:
		// ISDB2056N TS demodulator: Toshiba TC90522XBG
		chrdev2056->tc90522_s.i2c_addr = 0x13;
		chrdev2056->tc90522_s.is_secondary = true;
		break;
	default:
		dev_err(dev, "isdb2056_device_load_config: undefined model (model: %d)\n", isdb2056->isdb2056_model);
		return -EINVAL;
	}

	chrdev2056->r850.dev = dev;
	chrdev2056->r850.i2c = &chrdev2056->tc90522_t.i2c_master;
	chrdev2056->r850.i2c_addr = 0x7c;
	chrdev2056->r850.config.xtal = 24000;
	chrdev2056->r850.config.loop_through = false;
	chrdev2056->r850.config.clock_out = false;
	chrdev2056->r850.config.no_imr_calibration = true;
	chrdev2056->r850.config.no_lpf_calibration = true;

	chrdev2056->rt710.dev = dev;
	chrdev2056->rt710.i2c = &chrdev2056->tc90522_s.i2c_master;
	chrdev2056->rt710.i2c_addr = 0x7a;
	chrdev2056->rt710.config.xtal = 24000;
	chrdev2056->rt710.config.loop_through = false;
	chrdev2056->rt710.config.clock_out = false;
	chrdev2056->rt710.config.signal_output_mode = RT710_SIGNAL_OUTPUT_DIFFERENTIAL;
	chrdev2056->rt710.config.agc_mode = RT710_AGC_POSITIVE;
	chrdev2056->rt710.config.vga_atten_mode = RT710_VGA_ATTEN_OFF;
	chrdev2056->rt710.config.fine_gain = RT710_FINE_GAIN_3DB;
	chrdev2056->rt710.config.scan_mode = RT710_SCAN_MANUAL;

	for (i = 1; i < 5; i++) {
		it930x->config.input[i].enable = false;
		it930x->config.input[i].port_number = i;
	}

	return 0;
}

int isdb2056_device_init(struct isdb2056_device *isdb2056, struct device *dev,
			 enum isdb2056_model isdb2056_model,
			 struct ptx_chrdev_context *chrdev_ctx,
			 struct completion *quit_completion)
{
	int ret = 0;
	struct it930x_bridge *it930x;
	struct itedtv_bus *bus;
	struct ptx_chrdev_config chrdev_config;
	struct ptx_chrdev_group_config chrdev_group_config;
	struct ptx_chrdev_group *chrdev_group;
	struct isdb2056_stream_context *stream_ctx;

	if (!isdb2056 || !dev || !chrdev_ctx || !quit_completion)
		return -EINVAL;

	dev_dbg(dev, "isdb2056_device_init\n");

	get_device(dev);

	kref_init(&isdb2056->kref);
	isdb2056->dev = dev;
	isdb2056->isdb2056_model = isdb2056_model;
	isdb2056->quit_completion = quit_completion;

	stream_ctx = kzalloc(sizeof(*stream_ctx), GFP_KERNEL);
	if (!stream_ctx) {
		dev_err(isdb2056->dev,
			"isdb2056_device_init: kzalloc(sizeof(*stream_ctx), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail;
	}
	isdb2056->stream_ctx = stream_ctx;

	it930x = &isdb2056->it930x;
	bus = &it930x->bus;

	ret = itedtv_bus_init(bus);
	if (ret)
		goto fail_bus;

	ret = it930x_init(it930x);
	if (ret)
		goto fail_bridge;

	ret = it930x_raise(it930x);
	if (ret)
		goto fail_device;

	ret = isdb2056_device_load_config(isdb2056, &chrdev_config);
	if (ret)
		goto fail_device;

	chrdev_config.ops = &isdb2056_chrdev_ops;
	chrdev_config.options = PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = &isdb2056->chrdev2056;

	ret = it930x_load_firmware(it930x, IT930X_FIRMWARE_FILENAME);
	if (ret)
		goto fail_device;

	ret = it930x_init_warm(it930x);
	if (ret)
		goto fail_device;

	/* GPIO */
	ret = it930x_set_gpio_mode(it930x, 3, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 3, true);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 2, false);
	if (ret)
		goto fail_device;

#if 0
	ret = it930x_set_gpio_mode(it930x, 11, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	/* LNB power supply: off */
	ret = it930x_write_gpio(it930x, 11, false);
	if (ret)
		goto fail_device;
#endif

	if (px4_device_params.discard_null_packets) {
		struct it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		ret = it930x_set_pid_filter(it930x, 0, &filter);
		if (ret)
			goto fail_device;
	}

	chrdev_group_config.owner_kref = &isdb2056->kref;
	chrdev_group_config.owner_kref_release = isdb2056_device_release;
	chrdev_group_config.reserved = false;
	chrdev_group_config.minor_base = 0;	/* unused */
	chrdev_group_config.chrdev_num = 1;
	chrdev_group_config.chrdev_config = &chrdev_config;

	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev,
					   &chrdev_group_config, &chrdev_group);
	if (ret)
		goto fail_chrdev;

	isdb2056->chrdev_group = chrdev_group;
	isdb2056->chrdev2056.chrdev = &chrdev_group->chrdev[0];
	stream_ctx->chrdev = &chrdev_group->chrdev[0];

	atomic_set(&isdb2056->available, 1);
	return 0;

fail_chrdev:

fail_device:
	it930x_term(it930x);

fail_bridge:
	itedtv_bus_term(bus);

fail_bus:
	kfree(isdb2056->stream_ctx);

fail:
	put_device(dev);
	return ret;
}

static void isdb2056_device_release(struct kref *kref)
{
	struct isdb2056_device *isdb2056 = container_of(kref,
							struct isdb2056_device,
							kref);

	dev_dbg(isdb2056->dev, "isdb2056_device_release\n");

	it930x_term(&isdb2056->it930x);
	itedtv_bus_term(&isdb2056->it930x.bus);

	kfree(isdb2056->stream_ctx);
	put_device(isdb2056->dev);

	complete(isdb2056->quit_completion);
	return;
}

void isdb2056_device_term(struct isdb2056_device *isdb2056)
{
	dev_dbg(isdb2056->dev,
		"isdb2056_device_term: kref count: %u\n",
		kref_read(&isdb2056->kref));

	atomic_xchg(&isdb2056->available, 0);
	ptx_chrdev_group_destroy(isdb2056->chrdev_group);

	kref_put(&isdb2056->kref, isdb2056_device_release);
	return;
}
