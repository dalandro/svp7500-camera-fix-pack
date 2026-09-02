// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2024 Intel Corporation.
 *
 */

#include <linux/acpi.h>
#include <linux/intel_cvs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/vmalloc.h>

#include "cvs_gpio.h"
#include "intel_cvs_update.h"
#include "rgb_hello_init.h"

struct intel_cvs *cvs;

/*
 * cvs_replay_rgb_hello_init - replay the verbatim Windows-Hello RGB sensor
 * init stream via the kernel I2C path.
 *
 * windows_hello_unlock USBPcap shows Windows writes 344 entries to the OV08x40
 * RGB sensor (I2C addr 0x36) during Hello setup, including a ~30-burst-write
 * calibration LUT to registers 0x005b-0x005e that camera-only workflows never
 * touch. We replay it verbatim through i2c_transfer() on the CVS bridge's own
 * adapter — this is the same path our 256-byte HOST_SET_MIPI_CONFIG goes over,
 * which is known not to wedge the bridge. (Direct userspace i2ctransfer to
 * 0x36 *does* wedge it, presumably because of pm_runtime / power coordination
 * the kernel handles for us.)
 *
 * PRECONDITION: RGB sensor must already be powered (e.g. libcamera holding
 * pm_runtime via ov08x40, or some other driver path). If not powered, the
 * very first transfer will time out and propagate.
 *
 * Returns 0 on full success, negative errno on any failure. Logs a count of
 * successful vs failed writes.
 */
/*
 * cvs_send_mipi_ir_config - send the verbatim Windows-trace IR HOST_SET_MIPI_CONFIG
 * (0x0830) payload to the SVP7500 bridge so port-2 forwarding gets configured.
 *
 * Per Windows USBPcap analysis of the Hello-unlock trace, Windows sends 0x830
 * AFTER the IR sensor is already in streaming mode (MODE_SELECT=0x01) by ~182ms.
 * Our prior `mipi-ir` sysfs fired 0x830 BEFORE the sensor was streaming, leaving
 * port 2 silent forever. hm1092's s_stream(1) now calls this immediately after
 * writing MODE_SELECT=0x01, matching the Windows order.
 *
 * Returns 0 on success, negative errno on failure. Safe to call multiple times.
 */
/*
 * cvs_send_2byte_cmd - send a raw 2-byte opcode to the bridge (big-endian).
 * Used for 0x820 / 0x822 — Vision.sys-known short commands we never see
 * Windows use during Hello unlock USBPcap. Per RE of Vision.sys, the bridge
 * accepts these but their effect is undocumented. Long-shot probe.
 */
int cvs_send_2byte_cmd(u16 cmd)
{
	struct i2c_client *i2c;
	u8 buf[2];
	int cnt;

	if (!cvs || !cvs->dev)
		return -ENODEV;
	i2c = container_of(cvs->dev, struct i2c_client, dev);
	buf[0] = (cmd >> 8) & 0xFF;
	buf[1] = cmd & 0xFF;
	cnt = i2c_master_send(i2c, buf, 2);
	dev_info(cvs->dev, "%s: cmd=0x%04x i2c_master_send returned %d\n",
		 __func__, cmd, cnt);
	return cnt == 2 ? 0 : -EIO;
}

int cvs_send_mipi_ir_config(void)
{
	int ret;
	u8 state = 0xff;

	if (!cvs || !cvs->dev) {
		pr_err("intel_cvs: cvs_send_mipi_ir_config: not initialized\n");
		return -ENODEV;
	}
	/* len=1 sentinel selects the IR (port 2) verbatim payload in cvs_write_i2c */
	ret = cvs_write_i2c(HOST_SET_MIPI_CONFIG, NULL, 1);

	/*
	 * TEST 3: probe bridge state immediately after 0x830 returns. Per
	 * Vision.sys RE, Windows calls GET_DEVICE_STATE on failure paths
	 * but not in normal stream-start. We do it here for diagnostic —
	 * if the bridge reports a non-0x06 state byte after our IR 0x830,
	 * that's a smoking gun ("PLL fail", "lane error", etc).
	 */
	if (cvs_read_i2c(GET_DEVICE_STATE, (char *)&state, sizeof(state)) > 0)
		dev_info(cvs->dev,
			 "%s: post-0x830 GET_DEVICE_STATE = 0x%02x\n",
			 __func__, state);
	else
		dev_warn(cvs->dev,
			 "%s: post-0x830 GET_DEVICE_STATE read failed\n",
			 __func__);

	return ret;
}
EXPORT_SYMBOL_GPL(cvs_send_mipi_ir_config);

int cvs_replay_rgb_hello_init(void)
{
	struct i2c_client *i2c;
	int i, ret;
	int ok = 0, fail = 0;

	if (!cvs || !cvs->dev) {
		pr_err("intel_cvs: cvs_replay_rgb_hello_init: cvs not initialized\n");
		return -ENODEV;
	}

	i2c = container_of(cvs->dev, struct i2c_client, dev);
	if (!i2c || !i2c->adapter) {
		dev_err(cvs->dev, "%s: no i2c adapter\n", __func__);
		return -ENODEV;
	}

	dev_info(cvs->dev, "%s: replaying %d verbatim Windows Hello RGB writes...\n",
		 __func__, RGB_HELLO_INIT_COUNT);

	for (i = 0; i < RGB_HELLO_INIT_COUNT; i++) {
		const struct cvs_replay_entry *e = &rgb_hello_init[i];
		struct i2c_msg msg = {
			.addr	= e->addr,
			.flags	= 0,
			.len	= e->len,
			.buf	= (u8 *)e->data,
		};

		ret = i2c_transfer(i2c->adapter, &msg, 1);
		if (ret == 1) {
			ok++;
		} else {
			fail++;
			if (fail <= 5)
				dev_warn(cvs->dev,
					 "%s: entry %d (addr 0x%02x len %d) failed: %d\n",
					 __func__, i, e->addr, e->len, ret);
			if (fail == 6)
				dev_warn(cvs->dev,
					 "%s: ...further failures suppressed\n",
					 __func__);
		}
	}

	dev_info(cvs->dev, "%s: done — %d ok, %d failed (of %d)\n",
		 __func__, ok, fail, RGB_HELLO_INIT_COUNT);

	return fail ? -EIO : 0;
}

static irqreturn_t cvs_irq_handler(int irq, void *devid)
{
	struct intel_cvs *icvs = devid;

	icvs->hostwake_event_arg = 1;
	wake_up_interruptible(&icvs->hostwake_event);
	return IRQ_RETVAL(true);
}

static int cvs_init(struct intel_cvs *icvs)
{
	int ret = -EINVAL;

	if (!icvs || !icvs->dev || !icvs->rst)
		return -EINVAL;

	/*
	 * 2026-05-12: removed IRQF_ONESHOT. devm_request_irq() registers
	 * a hardirq-only handler; IRQF_ONESHOT is only meaningful for
	 * threaded handlers (would be set via devm_request_threaded_irq
	 * with a thread_fn). Kernel emits a WARNING at __setup_irq when
	 * IRQF_ONESHOT is set without thread_fn — and the actual IRQ
	 * delivery to cvs_irq_handler was unreliable as a result,
	 * which appears to cause the SVP7500 bridge to wedge after
	 * brief idle periods (bridge waits for ACK that never arrives).
	 *
	 * cvs_irq_handler only sets a flag and wakes a waitqueue —
	 * both safe in hardirq context, so the plain devm_request_irq
	 * is correct. Drop the bogus flag.
	 */
	ret = devm_request_irq(icvs->dev, icvs->irq, cvs_irq_handler,
						   IRQF_NO_SUSPEND,
						   dev_name(icvs->dev), icvs);
	if (ret)
		dev_err(icvs->dev, "Failed to request irq");
	return ret;
}

static int find_oem_prod_id(acpi_handle handle, const char *method_name,
			    unsigned long long *value)
{
	acpi_status status;

	status = acpi_evaluate_integer(handle, (acpi_string)method_name, NULL,
								   value);

	if (ACPI_FAILURE(status)) {
		dev_err(cvs->dev, "%s: ACPI method %s not found", __func__,
				method_name);
		return status;
	}

	dev_info(cvs->dev, "%s: ACPI method %s returned oem_prod_id:0x%llx",
			 __func__, method_name, *value);
	return 0;
}

static int cvs_common_probe(struct device *dev, bool is_i2c)
{
	struct intel_cvs *icvs;
	acpi_handle handle;
	int ret = -ENODEV;

	icvs = devm_kzalloc(dev, sizeof(struct intel_cvs), GFP_KERNEL);
	if (!icvs)
		return -ENOMEM;
	icvs->dev = dev;
	icvs->has_i2c = is_i2c;
	if (is_i2c) {
		struct i2c_client *i2c = to_i2c_client(dev);
		i2c_set_clientdata(i2c, icvs);
		dev_set_drvdata(dev, icvs);
		dev_info(dev, "%s: probed as i2c device", __func__);
	} else {
		dev_set_drvdata(dev, icvs);
		dev_info(dev, "%s: probed as platform device (GPIO-only)", __func__);
	}
	cvs = icvs;

	ret = gpiod_count(icvs->dev, NULL);
	switch (ret) {
	case ICVS_LIGHT:
		icvs->cap = ICVS_LIGHTCAP;
		break;
	case ICVS_FULL:
		icvs->cap = ICVS_FULLCAP;
		break;
	default:
		dev_err(icvs->dev, "Number of GPIOs not supported: %d", ret);
		ret = -EINVAL;
		goto exit;
	}

	ret = devm_acpi_dev_add_driver_gpios(icvs->dev,
			icvs->cap == ICVS_FULLCAP ?
			icvs_acpi_gpios : icvs_acpi_lgpios);
	if (ret) {
		dev_err(icvs->dev, "Failed to add driver gpios");
		goto exit;
	}

	/* Request GPIO */
	icvs->req = devm_gpiod_get(icvs->dev, "req", GPIOD_OUT_HIGH);
	if (IS_ERR(icvs->req)) {
		dev_err(icvs->dev, "Get request gpiod failed. Do deferred probing");
		return dev_err_probe(icvs->dev, -EPROBE_DEFER,
							 "Do deferred probing as request gpiod failed");
	}

	/* Response GPIO */
	icvs->resp = devm_gpiod_get(icvs->dev, "resp", GPIOD_IN);
	if (IS_ERR(icvs->resp)) {
		dev_err(icvs->dev, "Get response gpiod failed. Do deferred probing");
		return dev_err_probe(icvs->dev, -EPROBE_DEFER,
							 "Do deferred probing as response gpiod failed");
	}

	if (icvs->cap == ICVS_FULLCAP) {
		/* Reset GPIO */
		icvs->rst = devm_gpiod_get(icvs->dev, "rst", GPIOD_OUT_HIGH);
		if (IS_ERR(icvs->rst)) {
			dev_err(icvs->dev, "Get reset gpiod failed. Do deferred probing");
			return dev_err_probe(icvs->dev, -EPROBE_DEFER,
								 "Do deferred probing as reset gpiod failed");
		}

		/* Wake Interrupt */
		ret = acpi_dev_gpio_irq_get_by(ACPI_COMPANION(icvs->dev),
									   "wake-gpio", 0);
		if (ret > 0) {
			icvs->irq = ret;
		} else {
			dev_err(icvs->dev, "Failed to get right wake interrupt:%d", ret);
			ret = -EINVAL;
			goto exit;
		}

		INIT_WORK(&icvs->fw_dl_task, cvs_fw_dl_thread);
		init_waitqueue_head(&icvs->hostwake_event);
		init_waitqueue_head(&icvs->update_complete_event);
		init_waitqueue_head(&icvs->lvfs_fwdl_complete_event);
		icvs->fw_dl_task_finished = false;

		if (icvs->has_i2c) {
			ret = cvs_init(icvs);
			if (ret)
				dev_err(icvs->dev, "Failed to initialize\n");

			find_oem_prod_id(handle, "OPID", &icvs->oem_prod_id);

			ret = cvs_find_magic_num_support(icvs);
			if (ret)
				goto exit;

			if (icvs->magic_num_support) {
				ret = cvs_get_device_cap(&icvs->cv_fw_capability);
				if (ret)
					goto exit;
			}

			ret = cvs_write_i2c(SET_HOST_IDENTIFIER, NULL, 0);
			if (ret) {
				dev_err(cvs->dev, "%s:set_host_identifier cmd failed", __func__);
				goto exit;
			}

			/*
			 * 2026-05-11 v3: probe-time HOST_SET_MIPI_CONFIG RE-ENABLED.
			 *
			 * v2 disabled this to defer config until after sensor load
			 * (Windows-order theory). Result: hm1092 could not enable
			 * dvdd (-19 ENODEV) and could not read chip ID — sensor
			 * digital supply never came up. Concluded probe-time
			 * acquire + ANY valid 0x830 is what triggers the bridge's
			 * internal sensor power chain.
			 *
			 * Probe passes SSDB-style data here; intel_cvs_update.c's
			 * HOST_SET_MIPI_CONFIG case uses that data (legacy) when
			 * len > 0. sysfs 'mipi' passes data=NULL/len=0 which
			 * triggers verbatim Windows payload for post-probe tests.
			 */
			if (icvs->cv_fw_capability.dev_capability &
			    HOST_MIPI_CONFIG_REQUIRED_MASK) {
				/*
				 * MIPI config = SSDB structure starting at offset 0x14.
				 * Ghidra shows Windows reads a 0xC4-byte IOCTL buffer,
				 * passes bytes[0x14:] (176 bytes) to HOST_SET_MIPI_CONFIG.
				 *
				 * SSDB layout from offset 0x14:
				 *   0x00: u32 dphylinkenfuses
				 *   0x04: u32 clockdiv
				 *   0x08: u8  link (CSI-2 port)
				 *   0x09: u8  lanes
				 *   0x0A: u32 csiparams[10] (40 bytes)
				 *   0x32: u32 maxlanespeed
				 *   ... rest is zeros/reserved
				 */
				u8 mipi_cfg[176];
				u8 state;
				int retry;

				memset(mipi_cfg, 0, sizeof(mipi_cfg));
				/* Fields at SSDB-relative offsets (from 0x14) */
				mipi_cfg[0x08] = 2;  /* link/port = CSI-2 port 2 */
				mipi_cfg[0x09] = 1;  /* lanes = 1 */
				/* maxlanespeed at offset 0x32 (SSDB 0x46) */
				*(u32 *)(mipi_cfg + 0x32) = 360960000;
				/* mclkspeed at offset 0x42 (SSDB 0x56) */
				*(u32 *)(mipi_cfg + 0x42) = 19200000;
				/* controllogicid at offset 0x46 (SSDB 0x5A) */
				mipi_cfg[0x46] = 0;
				/* mipilinkdefined at offset 0x41 (SSDB 0x55) */
				mipi_cfg[0x41] = 1;

				ret = cvs_write_i2c(HOST_SET_MIPI_CONFIG,
						    mipi_cfg, sizeof(mipi_cfg));
				if (ret) {
					dev_warn(cvs->dev,
						 "%s:HOST_SET_MIPI_CONFIG failed: %d",
						 __func__, ret);
				} else {
					/* Poll device state — wait for bit 0x40 to clear */
					for (retry = 0; retry < 30; retry++) {
						mdelay(100);
						ret = cvs_read_i2c(GET_DEVICE_STATE,
								   (char *)&state,
								   sizeof(state));
						if (ret <= 0)
							continue;
						dev_dbg(cvs->dev,
							"%s:MIPI poll state=0x%02x retry=%d",
							__func__, state, retry);
						if ((state & 0x40) == 0) {
							dev_info(cvs->dev,
								 "%s:MIPI config accepted (state=0x%02x)",
								 __func__, state);
							break;
						}
					}
					if (retry >= 30)
						dev_warn(cvs->dev,
							 "%s:MIPI config timeout",
							 __func__);

					/*
					 * 0x833 auxiliary MIPI config —
					 * DISABLED 2026-04-24 after testing
					 * three candidate payloads (SSDB[0:44],
					 * SSDB[0x50:0x6B]+pad, mipi_cfg[0x30:0x5B]).
					 * All accepted by CVS without error,
					 * none unblocked frame delivery.
					 * 0x833 appears content-insensitive
					 * (maybe informational). Real frame
					 * blocker is elsewhere — sensor MIPI TX
					 * or CVS internal routing.
					 * Implementation kept in
					 * intel_cvs_update.c switch.
					 */
				}
			}
		}
	}

	mdelay(FW_PREPARE_MS);

	/*
	 * 2026-05-11 v3: probe-time ownership transfer RE-ENABLED.
	 *
	 * Deferring this in v2 broke sensor power chain (sensor's dvdd
	 * regulator failed to enable). The acquire+0x830 sequence at
	 * probe is required for sensor I2C @ 0x24 to be reachable.
	 *
	 * sysfs 'acquire' still works for manual re-triggering.
	 */
	ret = cvs_acquire_camera_sensor_internal();
	if (ret) {
		dev_err(cvs->dev, "%s:Acquire sensor fail", __func__);
		goto exit;
	} else {
		dev_info(cvs->dev, "%s:Transfer of ownership success",
			 __func__);
	}

	icvs->icvs_sensor_state = CV_SENSOR_VISION_ACQUIRED_STATE;
	acpi_dev_clear_dependencies(ACPI_COMPANION(icvs->dev));

exit:
	if (ret)
		devm_kfree(icvs->dev, icvs);

	return ret;
}

static int cvs_i2c_probe(struct i2c_client *i2c)
{
	return cvs_common_probe(&i2c->dev, true);
}

static int cvs_platform_probe(struct platform_device *pdev)
{
	return cvs_common_probe(&pdev->dev, false);
}

static void cvs_exit(struct intel_cvs *icvs)
{
	if (icvs && icvs->dev && icvs->rst)
		devm_free_irq(icvs->dev, icvs->irq, icvs);
}

static void cvs_common_remove(struct device *dev, bool is_i2c)
{
	struct intel_cvs *icvs = dev_get_drvdata(dev);
	if (is_i2c) {
		dev_info(dev, "%s (i2c)\n", __func__);
		icvs = i2c_get_clientdata(to_i2c_client(dev));
	} else {
		dev_info(dev, "%s (platform)\n", __func__);
	}
	if (icvs) {
		cvs->close_fw_dl_task = true;

		if (icvs->cap == ICVS_FULLCAP) {
			if (cvs->fw_dl_task_started && !cvs->fw_dl_task_finished) {
				dev_info(cvs->dev, "%s:signal cvs_fw_dl_thread() to stop",
						 __func__);
				cvs->hostwake_event_arg = 1;
				wake_up_interruptible(&cvs->hostwake_event);

				dev_info(cvs->dev, "%s:Wait for cvs_fw_dl_thread() to stop",
						 __func__);
				wait_event_interruptible(cvs->update_complete_event,
					cvs->update_complete_event_arg == 1);
				dev_info(cvs->dev, "%s:cvs_fw_dl_thread() stopped", __func__);
				mdelay(WAIT_HOST_RELEASE_MS);
			}

			/* reset vision chip */
			if (cvs_reset_cv_device()) {
				dev_err(cvs->dev, "%s:CV reset fail after flash", __func__);
				return;
			}
			cvs->icvs_state = CV_INIT_STATE;

			cvs_exit(icvs);
		}
		if (cvs->fw_buffer)
			vfree(cvs->fw_buffer);

		devm_kfree(dev, icvs);
	}
}

static void cvs_i2c_remove(struct i2c_client *i2c)
{
	cvs_common_remove(&i2c->dev, true);
}

static void cvs_platform_remove(struct platform_device *pdev)
{
	cvs_common_remove(&pdev->dev, false);
}

int cvs_read_i2c(u16 cmd, char *data, int size)
{
	struct intel_cvs *ctx = cvs;
	if (!ctx || !ctx->has_i2c)
		return -EOPNOTSUPP;
	struct i2c_client *i2c = container_of(cvs->dev, struct i2c_client, dev);
	int cnt;
	char *in_data;

	in_data = devm_kzalloc(ctx->dev, (CVMAGICNUMSIZE + size), GFP_KERNEL);
	if (!in_data) {
		dev_err(cvs->dev, "%s:Buffer allocation failed", __func__);
		return -ENOMEM;
	}
	u16 cvs_cmd = (((cmd) >> 8) & 0x00ff) | (((cmd) << 8) & 0xff00);

	if (size < 0 || !data)
		return -EINVAL;

	cnt = i2c_master_send(i2c, (const char *)&cvs_cmd, sizeof(u16));
	if (cnt != sizeof(u16)) {
		dev_err(&i2c->dev, "%s:cmd:%x count:%d (!=2)\n", __func__, cmd,
				cnt);
		return -EIO;
	}

	if (ctx->magic_num_support) {
		u32 magic_num_received;

		cnt = i2c_master_recv(i2c, in_data, size + CVMAGICNUMSIZE);
		magic_num_received = ((u32 *)in_data)[0];
		if (magic_num_received ==  CVMAGICNUM) {
			dev_dbg(&i2c->dev, "%s:Valid magic number", __func__);
			memcpy(data, in_data + CVMAGICNUMSIZE, size);
		} else {
			dev_dbg(&i2c->dev, "%s:Invalid magic number", __func__);
			return -EIO;
		}
	} else {
		cnt = i2c_master_recv(i2c, (char *)data, size);
	}

	return cnt;
}

int cvs_acquire_camera_sensor_internal(void)
{
	int val, retry = 20;

	if (!cvs)
		return -EINVAL;

	if (cvs->icvs_state == CV_FW_DOWNLOADING_STATE ||
		cvs->icvs_state == CV_FW_FLASHING_STATE)
		return -EBUSY;

	if (cvs->owner != CVS_CAMERA_IPU) {
		do {
			gpiod_set_value_cansleep(cvs->req, 0);
			mdelay(GPIO_READ_DELAY_MS);
			val = gpiod_get_value_cansleep(cvs->resp);
		} while (val != 0 && retry--);

		if (val != 0) {
			dev_err(cvs->dev, "%s:error! val %d (!=0)", __func__, val);
			return -EIO;
		}
	}

	cvs->owner = CVS_CAMERA_IPU;
	cvs->int_ref_count++;
	return 0;
}

int cvs_release_camera_sensor_internal(void)
{
	int val, retry = 20;

	if (!cvs)
		return -EINVAL;

	if (cvs->icvs_state == CV_FW_DOWNLOADING_STATE)
		return -EBUSY;

	if (cvs->int_ref_count == 0)
		return 0;

	if (cvs->owner != CVS_CAMERA_CVS && cvs->int_ref_count == 1) {
		do {
			gpiod_set_value_cansleep(cvs->req, 1);
			mdelay(GPIO_READ_DELAY_MS);
			val = gpiod_get_value_cansleep(cvs->resp);
		} while (val != 1 && retry--);

		if (val != 1) {
			dev_err(cvs->dev, "%s:error! val %d (!=1)", __func__, val);
			return -EIO;
		}
	}

	cvs->int_ref_count--;
	cvs->owner = (cvs->int_ref_count == 0) ? CVS_CAMERA_CVS : CVS_CAMERA_IPU;
	return 0;
}

#ifdef DEBUG_CVS
enum cvs_state cvs_state;
int cvs_exec_cmd(enum cvs_command command)
{
	int rc;

	if (!cvs)
		return -EINVAL;

	if (cvs->icvs_state == CV_FW_DOWNLOADING_STATE ||
		cvs->icvs_state == CV_FW_FLASHING_STATE) {
		dev_err(cvs->dev, "%s:Device busy, cmd:0X%x can't be queried now",
				__func__, command);
		cvs_state = cvs->cv_fw_state;
		return -EBUSY;
	}

	switch (command) {
	case GET_DEVICE_STATE:
		rc = cvs_read_i2c(GET_DEVICE_STATE, (char *)&cvs_state,
						  sizeof(char));
		if (rc <= 0)
			goto err_out;
		break;

	case GET_FW_VERSION:
		rc = cvs_read_i2c(GET_FW_VERSION, (char *)&cvs->ver,
						  sizeof(struct cvs_fw));
		if (rc <= 0)
			goto err_out;
		break;

	case GET_VID_PID:
		rc = cvs_read_i2c(GET_VID_PID, (char *)&cvs->id,
						  sizeof(struct cvs_id));
		if (rc <= 0)
			goto err_out;
		break;

	default:
		dev_err(cvs->dev, "%s:command %x not implemented\n", __func__,
			command);
	}

	return 0;

err_out:
	dev_err(cvs->dev, "%s:CVS command 0x%x failed, cvs_read_i2c: %d",
			__func__, command, rc);
	return -EIO;
}

static void cvs_state_str(const enum cvs_state stat, u8 *buf)
{
	int n = 0;

	*buf = 0;
	if (stat == DEVICE_OFF_STATE) {
		sprintf(buf, "%s", "DEVICE_OFF_STATE");
	} else {
		if (stat & PRIVACY_ON_BIT_MASK)
			n += sprintf(buf + n, "%s, ", "PRIVACY_ON_BIT_MASK");
		if (stat & DEVICE_ON_BIT_MASK)
			n += sprintf(buf + n, "%s, ", "DEVICE_ON_BIT_MASK");
		if (stat & SENSOR_OWNER_BIT_MASK)
			n += sprintf(buf + n, "%s, ", "SENSOR_OWNER_BIT_MASK");
		if (stat & DEVICE_DWNLD_STATE_MASK)
			n += sprintf(buf + n, "%s, ",
				     "DEVICE_DWNLD_STATE_MASK");
		if (stat & DEVICE_DWNLD_ERROR_MASK)
			n += sprintf(buf + n, "%s, ",
				     "DEVICE_DWNLD_ERROR_MASK");
		if (stat & DEVICE_DWNLD_BUSY_MASK)
			n += sprintf(buf + n, "%s, ", "DEVICE_DWNLD_BUSY_MASK");
	}
}

static ssize_t coredump_show(struct device *dev,
						struct device_attribute *attr, char *buf)
{
	u8 stat_name[256] = "";

	cvs_state_str(cvs_state, stat_name);
	return sysfs_emit(buf,
		"CVS VID/PID     : 0x%x 0x%x\n"
		"CVS Firmware Ver: %d.%d.%d.%d (0x%x.0x%x.0x%x.0x%x)\n"
		"CVS Device State: 0x%x (%s)\n"
		"Reference Count : %d\n"
		"CVS Owner       : %s\n",
		cvs->id.vid, cvs->id.pid, cvs->ver.major, cvs->ver.minor,
		cvs->ver.hotfix, cvs->ver.build, cvs->ver.major, cvs->ver.minor,
		cvs->ver.hotfix, cvs->ver.build, cvs_state, stat_name,
		cvs->int_ref_count,
		((cvs->owner == CVS_CAMERA_NONE) ? "NONE" :
		 (cvs->owner == CVS_CAMERA_CVS)	 ? "CVS" :
		 (cvs->owner == CVS_CAMERA_IPU)	 ? "HOST" : "UNKNOWN"));
}
static DEVICE_ATTR_RO(coredump);

static ssize_t cmd_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	if (sysfs_streq(buf, "state"))
		cvs_exec_cmd(GET_DEVICE_STATE);
	else if (sysfs_streq(buf, "version"))
		cvs_exec_cmd(GET_FW_VERSION);
	else if (sysfs_streq(buf, "id"))
		cvs_exec_cmd(GET_VID_PID);
	else if (sysfs_streq(buf, "reset")) {
		/*
		 * Pulse the rst GPIO via the existing kernel helper. Required
		 * after firmware update per FWUPDATE_RESET_REQUIRED capability
		 * bit. Without this, CVS state remains 0x56 (post-FW limbo)
		 * and the MIPI bridge stays disabled.
		 */
		int r = cvs_reset_cv_device();
		dev_info(cvs->dev, "%s: cvs_reset_cv_device returned %d", __func__, r);
	}
	else if (sysfs_streq(buf, "acquire")) {
		/*
		 * Manually transfer sensor ownership to CVS bridge. Probe-time
		 * call was deferred so hm1092 can probe via direct sensor I2C.
		 */
		int r = cvs_acquire_camera_sensor_internal();
		if (r == 0)
			cvs->icvs_sensor_state = CV_SENSOR_VISION_ACQUIRED_STATE;
		dev_info(cvs->dev,
			 "%s: manual acquire_camera_sensor returned %d",
			 __func__, r);
	}
	else if (sysfs_streq(buf, "mipi")) {
		/* RGB verbatim Windows payload (port 0 / OV08x40). */
		int r = cvs_write_i2c(HOST_SET_MIPI_CONFIG, NULL, 0);
		dev_info(cvs->dev,
			 "%s: manual HOST_SET_MIPI_CONFIG returned %d (RGB verbatim)",
			 __func__, r);
	}
	else if (sysfs_streq(buf, "mipi-ir")) {
		/*
		 * IR verbatim Windows payload (port 2 / HM1092). Captured from
		 * Windows Hello unlock by @tverhaeghe (intel/vision-drivers#37,
		 * 2026-05-12). Differs from RGB in: byte 0x0d (port idx 0x01 vs
		 * 0x02), bytes 0x10-13 (143 MHz vs 71 MHz), and 0x36-37 (single
		 * port descriptor vs two). The len=1 sentinel tells cvs_write_i2c
		 * to select the IR payload.
		 */
		int r = cvs_write_i2c(HOST_SET_MIPI_CONFIG, NULL, 1);
		dev_info(cvs->dev,
			 "%s: manual HOST_SET_MIPI_CONFIG returned %d (IR verbatim)",
			 __func__, r);
	}
	else if (sysfs_streq(buf, "cmd-820")) {
		int r = cvs_send_2byte_cmd(0x0820);
		dev_info(cvs->dev, "%s: cmd-820 returned %d", __func__, r);
	}
	else if (sysfs_streq(buf, "cmd-822")) {
		int r = cvs_send_2byte_cmd(0x0822);
		dev_info(cvs->dev, "%s: cmd-822 returned %d", __func__, r);
	}
	else if (sysfs_streq(buf, "set-vision")) {
		/*
		 * Send SET_HOST_IDENTIFIER (0x0805) with vision_sensing=1.
		 * Probe-time call sends vision_sensing=0; if the bridge gates
		 * IR/Hello port forwarding on this flag (it IS the "Computer
		 * Vision Sensor" — vision is its purpose), flipping this to 1
		 * may be required to enable port 2.
		 */
		struct i2c_client *i2c = container_of(cvs->dev, struct i2c_client, dev);
		union cv_host_identifiers hi = { .value = 0 };
		u8 buf2[2 + 4];
		int cnt;

		hi.field.vision_sensing = 1;
		hi.field.rgbcamera_pwrup_host = 1;  /* same as probe — preserve */

		buf2[0] = 0x08;
		buf2[1] = 0x05;
		memcpy(&buf2[2], &hi.value, 4);
		cnt = i2c_master_send(i2c, buf2, sizeof(buf2));
		dev_info(cvs->dev,
			 "%s: SET_HOST_IDENTIFIER vision_sensing=1 sent %d/%zu bytes\n",
			 __func__, cnt, sizeof(buf2));
	}
	else if (sysfs_streq(buf, "claim-led") || sysfs_streq(buf, "unclaim-led")) {
		/*
		 * SET_HOST_IDENTIFIER (0x0805) with privacy_led_host toggled.
		 * The probe path hardcodes privacy_led_host=0, leaving the
		 * physical privacy LED under bridge-firmware control — which
		 * latches it ON at first stream until power-off. Claiming LED
		 * control for the host may stop the firmware latch (this is
		 * plausibly what the Windows driver does). unclaim-led reverts
		 * to the probe default.
		 */
		struct i2c_client *i2c = container_of(cvs->dev, struct i2c_client, dev);
		union cv_host_identifiers hi = { .value = 0 };
		u8 buf2[2 + 4];
		int cnt;

		hi.field.privacy_led_host = sysfs_streq(buf, "claim-led") ? 1 : 0;
		hi.field.rgbcamera_pwrup_host = 1;  /* same as probe — preserve */

		buf2[0] = 0x08;
		buf2[1] = 0x05;
		memcpy(&buf2[2], &hi.value, 4);
		cnt = i2c_master_send(i2c, buf2, sizeof(buf2));
		dev_info(cvs->dev,
			 "%s: SET_HOST_IDENTIFIER privacy_led_host=%u sent %d/%zu bytes\n",
			 __func__, hi.field.privacy_led_host, cnt, sizeof(buf2));
	}
	else if (sysfs_streq(buf, "release")) {
		/*
		 * Soft ownership release — the GPIO req/resp inverse of the
		 * existing "acquire" verb, wired to the previously dead
		 * cvs_release_camera_sensor_internal(). Candidate "camera
		 * off" half of a manual LED/ownership toggle; "acquire"
		 * (possibly followed by "mipi") is the "camera on" half.
		 * Risk: probe comments warn the sensor power chain may
		 * collapse when ownership drops — reboot recovers.
		 */
		int r = cvs_release_camera_sensor_internal();
		dev_info(cvs->dev,
			 "%s: release returned %d (owner=%d ref=%d)\n",
			 __func__, r, cvs->owner, cvs->int_ref_count);
	}
	else if (sysfs_streq(buf, "factory-reset")) {
		/*
		 * FACTORY_RESET (0x0831) — untested upstream opcode. May clear
		 * a stuck internal state preventing port-2 forwarding. Safe to
		 * try once at idle (state=0x06).
		 */
		int r = cvs_send_2byte_cmd(0x0831);
		dev_info(cvs->dev, "%s: factory-reset returned %d", __func__, r);
	}
	else if (strncmp(buf, "cmd-", 4) == 0) {
		/*
		 * Generic "send raw 2-byte opcode" path. Accepts
		 *   echo cmd-NNNN > cmd
		 * where NNNN is a hex opcode (e.g. cmd-0823 or cmd-823 or
		 * cmd-83a). Logs pre-state, send result, and post-state to
		 * dmesg so userspace scan scripts can iterate the opcode
		 * space and detect which ones affect bridge state.
		 */
		u16 op = 0;
		const char *hex = buf + 4;
		size_t hlen;
		char tmp[8];
		u8 pre_state = 0xff, post_state = 0xff;
		int rc, len_ok;

		/* Skip optional 0x prefix */
		if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
			hex += 2;

		/* Strip trailing newline + whitespace */
		hlen = strnlen(hex, sizeof(tmp));
		while (hlen && (hex[hlen-1] == '\n' || hex[hlen-1] == ' '))
			hlen--;
		if (hlen == 0 || hlen >= sizeof(tmp)) {
			dev_err(cvs->dev, "%s: cmd-NNNN: bad hex length %zu\n",
				__func__, hlen);
			return count;
		}
		memcpy(tmp, hex, hlen);
		tmp[hlen] = '\0';

		len_ok = kstrtou16(tmp, 16, &op);
		if (len_ok) {
			dev_err(cvs->dev, "%s: cmd-NNNN: parse error '%s'\n",
				__func__, tmp);
			return count;
		}

		/* Read state before */
		if (cvs_read_i2c(GET_DEVICE_STATE, (char *)&pre_state, 1) <= 0)
			pre_state = 0xff;

		/* Fire opcode */
		rc = cvs_send_2byte_cmd(op);

		/* Read state after */
		mdelay(50);
		if (cvs_read_i2c(GET_DEVICE_STATE, (char *)&post_state, 1) <= 0)
			post_state = 0xff;

		dev_info(cvs->dev,
			 "SCAN op=0x%04x pre=0x%02x send_rc=%d post=0x%02x delta=0x%02x\n",
			 op, pre_state, rc, post_state, pre_state ^ post_state);
	}
	else if (sysfs_streq(buf, "replay-rgb-init")) {
		/*
		 * Replay the verbatim 344-write Windows-Hello RGB init stream
		 * via kernel I2C path. Includes the 0x005b-0x005e calibration
		 * LUT that camera-only workflows never touch. Must be called
		 * AFTER ov08x40 has acquired pm_runtime (e.g. libcamera RGB
		 * streaming) — otherwise the sensor is unpowered and the first
		 * transfer will time out and wedge the bridge.
		 */
		int r = cvs_replay_rgb_hello_init();
		dev_info(cvs->dev,
			 "%s: cvs_replay_rgb_hello_init returned %d",
			 __func__, r);
	}
	else
		dev_err(cvs->dev, "%s:invalid %s command\n", __func__, buf);

	return count;
}

static ssize_t cmd_show(struct device *dev, struct device_attribute *attr,
						char *buf)
{
	return sysfs_emit(buf, "command: %s\n",
			  "[coredump, state, version, id, reset, acquire, mipi, mipi-ir, replay-rgb-init, cmd-820, cmd-822, set-vision, factory-reset, cmd-NNNN]");
}
static DEVICE_ATTR_RW(cmd);
#endif //DEBUG_CVS

static ssize_t cvs_ctrl_data_pre_show(struct device *dev,
						struct device_attribute *attr, char *buf)
{
	unsigned long flags;
	int count;

	if (!buf) {
		dev_err(cvs->dev, "%s: buff is null", __func__);
		return -EINVAL;
	}

	count = sizeof(struct cvs_to_plugin_interface);
	memset(&cvs->info_fwupd, 0, sizeof(struct ctrl_data_fwupd));
	cvs->fw_dl_task_finished = false;
	cvs->fw_dl_task_started = false;

	if (cvs_get_fwver_vid_pid()) {
		dev_err(cvs->dev, "%s:Not able to read vid/pid", __func__);
		return -EIO;
	}
	dev_info(cvs->dev, "%s:Device fw version is %d.%d.%d.%d", __func__,
			 cvs->ver.major, cvs->ver.minor, cvs->ver.hotfix, cvs->ver.build);

	/* Device vid,pid */
	cvs->cvs_to_plugin.vid = cvs->id.vid;
	cvs->cvs_to_plugin.pid = cvs->id.pid;
	cvs->cvs_to_plugin.opid = cvs->oem_prod_id;
	cvs->cvs_to_plugin.dev_capabilities =
		cvs->cv_fw_capability.dev_capability;

	/* Device FW version */
	cvs->cvs_to_plugin.major = cvs->ver.major;
	cvs->cvs_to_plugin.minor = cvs->ver.minor;
	cvs->cvs_to_plugin.hotfix = cvs->ver.hotfix;
	cvs->cvs_to_plugin.build = cvs->ver.build;

	spin_lock_irqsave(&cvs->buffer_lock, flags);
	memcpy(buf, &cvs->cvs_to_plugin, sizeof(struct cvs_to_plugin_interface));
	spin_unlock_irqrestore(&cvs->buffer_lock, flags);
	return count;
}

static ssize_t cvs_ctrl_data_pre_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long flags;
	struct file *f;
	int fw_bin_size;
	ssize_t bytes_read = 0;

	if (!buf) {
		dev_err(cvs->dev, "%s:buff is null", __func__);
		return -EINVAL;
	}

	if (count == 0 || count != sizeof(struct plugin_to_cvs_interface)) {
		dev_err(cvs->dev, "%s:Wrong count:%x", __func__, (unsigned int)count);
		return -EINVAL;
	}

	spin_lock_irqsave(&cvs->buffer_lock, flags);
	memcpy(&cvs->plugin_to_cvs, buf, count);
	spin_unlock_irqrestore(&cvs->buffer_lock, flags);

	dev_dbg(cvs->dev, "%s:dl_time:%x, fl_time:%x, retry_cnt:%x, fw_bin_fd:%x",
			__func__, cvs->plugin_to_cvs.max_download_time,
			cvs->plugin_to_cvs.max_flash_time,
			cvs->plugin_to_cvs.max_fwupd_retry_count,
			cvs->plugin_to_cvs.fw_bin_fd);

	/* Get the file structure from the file descriptor */
	f = fget(cvs->plugin_to_cvs.fw_bin_fd);
	if (!f) {
		dev_err(cvs->dev, "%s:Bad file descriptor", __func__);
		return -EBADF;
	}

	fw_bin_size = generic_file_llseek(f, 0, SEEK_END);
	dev_dbg(cvs->dev, "%s:Calculated fw_bin size:%x bytes",
			__func__, fw_bin_size);
	generic_file_llseek(f, -fw_bin_size, SEEK_CUR);

	cvs->fw_buffer_size = fw_bin_size;
	cvs->max_flashtime_ms = cvs->plugin_to_cvs.max_flash_time;
	cvs->fw_update_retries = cvs->plugin_to_cvs.max_fwupd_retry_count;
	cvs->fw_buffer = vmalloc(cvs->fw_buffer_size);

	if (IS_ERR_OR_NULL(cvs->fw_buffer)) {
		dev_err(cvs->dev, "%s:No memory for fw_buffer", __func__);
		fput(f);
		return -ENOMEM;
	}
	dev_dbg(cvs->dev, "%s:fw_buffer allocated at:%p",
			__func__, cvs->fw_buffer);
	/* Read FW binary using file descriptor */
	bytes_read = kernel_read(f, cvs->fw_buffer, cvs->fw_buffer_size,
							 &f->f_pos);
	fput(f);
	if (bytes_read != cvs->fw_buffer_size) {
		dev_err(cvs->dev, "%s:kernel_read failed with bytes_read:%lx",
				__func__, bytes_read);
		return -EIO;
	} else {
		dev_info(cvs->dev, "%s:Full fw_buffer received. Start fw_download",
				 __func__);
		schedule_work(&cvs->fw_dl_task);
		cvs->fw_dl_task_started = true;
	}
	return count;
}

static ssize_t cvs_ctrl_data_fwupd_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	unsigned long flags;
	unsigned int count = sizeof(struct ctrl_data_fwupd);

	if (!buf) {
		dev_err(cvs->dev, "%s:buff is null", __func__);
		return -EINVAL;
	}
	cvs->info_fwupd.fw_dl_finshed = cvs->fw_dl_task_finished;
	cvs->info_fwupd.dev_state     = cvs->cv_fw_state;
	cvs->info_fwupd.fw_upd_retries = cvs->fw_update_retries;

	spin_lock_irqsave(&cvs->buffer_lock, flags);
	memcpy(buf, &cvs->info_fwupd, count);
	spin_unlock_irqrestore(&cvs->buffer_lock, flags);

	if (cvs->info_fwupd.total_packets == cvs->info_fwupd.num_packets_sent) {
		cvs->info_fwupd.fw_dl_finshed = true;
		cvs->lvfs_fwdl_complete_event_arg = 1;
		wake_up_interruptible(&cvs->lvfs_fwdl_complete_event);
	}
	return count;
}

static DEVICE_ATTR_RW(cvs_ctrl_data_pre);
static DEVICE_ATTR_RO(cvs_ctrl_data_fwupd);

static struct attribute *cvs_attrs[] = {
#ifdef DEBUG_CVS
	&dev_attr_coredump.attr,
	&dev_attr_cmd.attr,
#endif
	&dev_attr_cvs_ctrl_data_pre.attr,
	&dev_attr_cvs_ctrl_data_fwupd.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cvs);

#ifdef CONFIG_PM
static int cvs_suspend(struct device *dev)
{
	struct intel_cvs *icvs = dev_get_drvdata(dev);
	int ret = 0;

	if (!icvs)
		return 0; /* Nothing bound */

	/* Platform (GPIO-only) variant: no I2C resources to manage */
	if (!icvs->has_i2c) {
		dev_dbg(dev, "%s: platform(no-i2c) skip suspend", __func__);
		return 0;
	}

	dev_info(icvs->dev, "%s:entered", __func__);
	if (icvs->cap == ICVS_FULLCAP && cvs->fw_dl_task_finished != true) {
		icvs->cv_suspend = true;
		cvs->close_fw_dl_task = true;
		cvs->hostwake_event_arg = 1;
		wake_up_interruptible(&cvs->hostwake_event);
		/* Wait for fw update to cancel */
		dev_info(icvs->dev, "%s:wait for fw update cancel", __func__);
		flush_work(&icvs->fw_dl_task);
		dev_info(icvs->dev, "%s:fw update cancelled", __func__);
	}

	if (icvs->cap == ICVS_FULLCAP && icvs->irq > 0)
		disable_irq(icvs->irq);

	dev_info(icvs->dev, "%s:completed", __func__);
	return ret;
}

static int cvs_resume(struct device *dev)
{
	struct intel_cvs *icvs = dev_get_drvdata(dev);
	int val = -1;

	if (!icvs)
		return 0;

	/* Always check GPIO response even for platform (no-i2c) variant */
	dev_info(dev, "%s entered", __func__);
	val = gpiod_get_value_cansleep(icvs->resp);
	if (val != 0)
		dev_err(dev, "%s:Wrong gpio_response val:%x read via bridge",
			__func__, val);

	if (!icvs->has_i2c)
		return 0; /* Skip I2C / IRQ resume parts */

	if (icvs->cap == ICVS_FULLCAP) {
		icvs->cv_suspend = false;
		icvs->close_fw_dl_task = false;

		if (icvs->irq > 0)
			enable_irq(icvs->irq);
		if (cvs->fw_dl_task_started && !cvs->fw_dl_task_finished) {
			schedule_work(&icvs->fw_dl_task);
			cvs->fw_dl_task_started = true;
		}
	}

	dev_info(icvs->dev, "%s:completed", __func__);
	return 0;
}

static const struct dev_pm_ops icvs_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(cvs_suspend, cvs_resume) };

#define ICVS_DEV_PM_OPS (&icvs_pm_ops)
#else
#define ICVS_DEV_PM_OPS NULL
#endif /* CONFIG_PM */

static struct acpi_device_id acpi_cvs_ids[] = { { "INTC10CF" }, /* MTL */
						{ "INTC10DE" }, /* LNL */
						{ "INTC10E0" }, /* ARL */
						{ "INTC10E1" }, /* PTL */
						{ /* END OF LIST */ } };
MODULE_DEVICE_TABLE(acpi, acpi_cvs_ids);

static struct i2c_driver cvs_i2c_driver = {
	.driver = {
		.name = "Intel CVS driver",
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(acpi_cvs_ids),
		.dev_groups = cvs_groups,
		.pm = pm_ptr(ICVS_DEV_PM_OPS),
	},
	.probe = cvs_i2c_probe,
	.remove = cvs_i2c_remove
};

static struct platform_driver cvs_platform_driver = {
	.driver = {
		.name = "Intel CVS driver",
		.acpi_match_table = ACPI_PTR(acpi_cvs_ids),
		.dev_groups = cvs_groups,
		.pm = pm_ptr(ICVS_DEV_PM_OPS),
	},
	.probe	= cvs_platform_probe,
	.remove	= cvs_platform_remove,
};

static int __init icvs_init(void)
{
	int ret = i2c_add_driver(&cvs_i2c_driver);

	if (ret) {
		pr_err("Failed to register I2C driver: %d\n", ret);
		return ret;
	}

	ret = platform_driver_register(&cvs_platform_driver);
	if (ret) {
		i2c_del_driver(&cvs_i2c_driver);
		pr_err("Failed to register platform driver: %d\n", ret);
	}

	return ret;
}
module_init(icvs_init);

static void __exit icvs_exit(void)
{
	i2c_del_driver(&cvs_i2c_driver);
	platform_driver_unregister(&cvs_platform_driver);
}
module_exit(icvs_exit);

MODULE_AUTHOR("Lifu Wang <lifu.wang@intel.com>");
MODULE_AUTHOR("Israel Cepeda <israel.a.cepeda.lopez@intel.com>");
MODULE_AUTHOR("Hemanth Rachakonda <hemanth.rachakonda@intel.com>");
MODULE_AUTHOR("Srinivas Alla <alla.srinivas@intel.com>");
MODULE_DESCRIPTION("Intel CVS driver");
MODULE_LICENSE("GPL v2");
MODULE_SOFTDEP("pre: usbio gpio-usbio i2c-usbio");
