/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2020 Florian Schmidt <schmidt_florian@gmx.de>
 * Copyright (C) 2013 Marcus Comstedt <marcus@mc.pp.se>
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <libsigrok/libsigrok.h>
#include <string.h>

#include "libsigrok-internal.h"
#include "protocol.h"

#define UC_FIRMWARE	"kingst-la-%04x.fw"
#define FPGA_FW_LA2016	"kingst-la2016-fpga.bitstream"
#define FPGA_FW_LA2016A	"kingst-la2016a1-fpga.bitstream"
#define FPGA_FW_LA1016	"kingst-la1016-fpga.bitstream"
#define FPGA_FW_LA1016A	"kingst-la1016a1-fpga.bitstream"

#define MAX_SAMPLE_RATE_LA2016	SR_MHZ(200)
#define MAX_SAMPLE_RATE_LA1016	SR_MHZ(100)
#define MAX_SAMPLE_DEPTH	10e9
#define MAX_PWM_FREQ		SR_MHZ(20)
#define PWM_CLOCK		SR_MHZ(200)	/* 200MHz for both LA2016 and LA1016 */

/* USB vendor class control requests, executed by the Cypress FX2 MCU. */
#define CMD_FPGA_ENABLE	0x10
#define CMD_FPGA_SPI	0x20	/* R/W access to FPGA registers via SPI. */
#define CMD_BULK_START	0x30	/* Start sample data download via USB EP6 IN. */
#define CMD_BULK_RESET	0x38	/* Flush FIFO of FX2 USB EP6 IN. */
#define CMD_FPGA_INIT	0x50	/* Used before and after FPGA bitstream upload. */
#define CMD_KAUTH	0x60	/* Communicate to auth IC (U10). Not used. */
#define CMD_EEPROM	0xa2	/* R/W access to EEPROM content. */

/*
 * FPGA register addresses (base addresses when registers span multiple
 * bytes, in that case data is kept in little endian format). Passed to
 * CMD_FPGA_SPI requests. The FX2 MCU transparently handles the detail
 * of SPI transfers encoding the read (1) or write (0) direction in the
 * MSB of the address field. There are some 60 byte-wide FPGA registers.
 */
#define REG_RUN		0x00	/* Read capture status, write start capture. */
#define REG_PWM_EN	0x02	/* User PWM channels on/off. */
#define REG_CAPT_MODE	0x03	/* Write 0x00 capture to SDRAM, 0x01 streaming. */
#define REG_BULK	0x08	/* Write start addr, byte count to download samples. */
#define REG_SAMPLING	0x10	/* Write capture config, read capture SDRAM location. */
#define REG_TRIGGER	0x20	/* write level and edge trigger config. */
#define REG_THRESHOLD	0x68	/* Write PWM config to setup input threshold DAC. */
#define REG_PWM1	0x70	/* Write config for user PWM1. */
#define REG_PWM2	0x78	/* Write config for user PWM2. */

static int ctrl_in(const struct sr_dev_inst *sdi,
		   uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
		   void *data, uint16_t wLength)
{
	struct sr_usb_dev_inst *usb;
	int ret;

	usb = sdi->conn;

	if ((ret = libusb_control_transfer(
		     usb->devhdl, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
		     bRequest, wValue, wIndex, (unsigned char *)data, wLength,
		     DEFAULT_TIMEOUT_MS)) != wLength) {
		sr_dbg("USB ctrl in: %d bytes, req %d val %#x idx %d: %s.",
			wLength, bRequest, wValue, wIndex,
			libusb_error_name(ret));
		sr_err("Cannot read %d bytes from USB: %s.",
			wLength, libusb_error_name(ret));
		return SR_ERR;
	}

	return SR_OK;
}

static int ctrl_out(const struct sr_dev_inst *sdi,
		    uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
		    void *data, uint16_t wLength)
{
	struct sr_usb_dev_inst *usb;
	int ret;

	usb = sdi->conn;

	if ((ret = libusb_control_transfer(
		     usb->devhdl, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		     bRequest, wValue, wIndex, (unsigned char*)data, wLength,
		     DEFAULT_TIMEOUT_MS)) != wLength) {
		sr_dbg("USB ctrl out: %d bytes, req %d val %#x idx %d: %s.",
			wLength, bRequest, wValue, wIndex,
			libusb_error_name(ret));
		sr_err("Cannot write %d bytes to USB: %s.",
			wLength, libusb_error_name(ret));
		return SR_ERR;
	}

	return SR_OK;
}

static int upload_fpga_bitstream(const struct sr_dev_inst *sdi, const char *bitstream_fname)
{
	struct dev_context *devc;
	struct drv_context *drvc;
	struct sr_usb_dev_inst *usb;
	struct sr_resource bitstream;
	uint8_t buffer[sizeof(uint32_t)];
	uint8_t *wrptr;
	uint8_t cmd_resp;
	uint8_t block[4096];
	int len, act_len;
	unsigned int pos;
	int ret;
	unsigned int zero_pad_to = 0x2c000;

	devc = sdi->priv;
	drvc = sdi->driver->context;
	usb = sdi->conn;

	sr_info("Uploading FPGA bitstream '%s'.", bitstream_fname);

	ret = sr_resource_open(drvc->sr_ctx, &bitstream, SR_RESOURCE_FIRMWARE, bitstream_fname);
	if (ret != SR_OK) {
		sr_err("Cannot find FPGA bitstream %s.", bitstream_fname);
		return ret;
	}

	devc->bitstream_size = (uint32_t)bitstream.size;
	wrptr = buffer;
	write_u32le_inc(&wrptr, devc->bitstream_size);
	if ((ret = ctrl_out(sdi, CMD_FPGA_INIT, 0x00, 0, buffer, wrptr - buffer)) != SR_OK) {
		sr_err("Cannot initiate FPGA bitstream upload.");
		sr_resource_close(drvc->sr_ctx, &bitstream);
		return ret;
	}

	pos = 0;
	while (1) {
		if (pos < bitstream.size) {
			len = (int)sr_resource_read(drvc->sr_ctx, &bitstream, &block, sizeof(block));
			if (len < 0) {
				sr_err("Cannot read FPGA bitstream.");
				sr_resource_close(drvc->sr_ctx, &bitstream);
				return SR_ERR;
			}
		} else {
			/*  Zero-pad until 'zero_pad_to'. */
			len = zero_pad_to - pos;
			if ((unsigned)len > sizeof(block))
				len = sizeof(block);
			memset(&block, 0, len);
		}
		if (len == 0)
			break;

		ret = libusb_bulk_transfer(usb->devhdl, 2, (unsigned char*)&block[0], len, &act_len, DEFAULT_TIMEOUT_MS);
		if (ret != 0) {
			sr_dbg("Cannot write FPGA bitstream, block %#x len %d: %s.",
				pos, (int)len, libusb_error_name(ret));
			ret = SR_ERR;
			break;
		}
		if (act_len != len) {
			sr_dbg("Short write for FPGA bitstream, block %#x len %d: got %d.",
				pos, (int)len, act_len);
			ret = SR_ERR;
			break;
		}
		pos += len;
	}
	sr_resource_close(drvc->sr_ctx, &bitstream);
	if (ret != 0)
		return ret;
	sr_info("FPGA bitstream upload (%" PRIu64 " bytes) done.",
		bitstream.size);

	if ((ret = ctrl_in(sdi, CMD_FPGA_INIT, 0x00, 0, &cmd_resp, sizeof(cmd_resp))) != SR_OK) {
		sr_err("Cannot read response after FPGA bitstream upload.");
		return ret;
	}
	if (cmd_resp != 0) {
		sr_err("Unexpected FPGA bitstream upload response, got 0x%02x, want 0.",
			cmd_resp);
		return SR_ERR;
	}

	g_usleep(30000);

	if ((ret = ctrl_out(sdi, CMD_FPGA_ENABLE, 0x01, 0, NULL, 0)) != SR_OK) {
		sr_err("Cannot enable FPGA after bitstream upload.");
		return ret;
	}

	g_usleep(40000);
	return SR_OK;
}

static int set_threshold_voltage(const struct sr_dev_inst *sdi, float voltage)
{
	struct dev_context *devc;
	int ret;

	devc = sdi->priv;

	uint16_t duty_R79,duty_R56;
	uint8_t buf[2 * sizeof(uint16_t)];
	uint8_t *wrptr;

	/* Clamp threshold setting to valid range for LA2016. */
	if (voltage > 4.0) {
		voltage = 4.0;
	}
	else if (voltage < -4.0) {
		voltage = -4.0;
	}

	/*
	 * Two PWM output channels feed one DAC which generates a bias
	 * voltage, which offsets the input probe's voltage level, and
	 * in combination with the FPGA pins' fixed threshold result in
	 * a programmable input threshold from the user's perspective.
	 * The PWM outputs can be seen on R79 and R56 respectively, the
	 * frequency is 100kHz and the duty cycle varies. The R79 PWM
	 * uses three discrete settings. The R56 PWM varies with desired
	 * thresholds and depends on the R79 PWM configuration. See the
	 * schematics comments which discuss the formulae.
	 */
	if (voltage >= 2.9) {
		duty_R79 = 0;		/* PWM off (0V). */
		duty_R56 = (uint16_t)(302 * voltage - 363);
	}
	else if (voltage <= -0.4) {
		duty_R79 = 0x02d7;	/* 72% duty cycle. */
		duty_R56 = (uint16_t)(302 * voltage + 1090);
	}
	else {
		duty_R79 = 0x00f2;	/* 25% duty cycle. */
		duty_R56 = (uint16_t)(302 * voltage + 121);
	}

	/* Clamp duty register values to sensible limits. */
	if (duty_R56 < 10) {
		duty_R56 = 10;
	}
	else if (duty_R56 > 1100) {
		duty_R56 = 1100;
	}

	sr_dbg("Set threshold voltage %.2fV.", voltage);
	sr_dbg("Duty cycle values: R56 0x%04x, R79 0x%04x.", duty_R56, duty_R79);

	wrptr = buf;
	write_u16le_inc(&wrptr, duty_R56);
	write_u16le_inc(&wrptr, duty_R79);

	ret = ctrl_out(sdi, CMD_FPGA_SPI, REG_THRESHOLD, 0, buf, wrptr - buf);
	if (ret != SR_OK) {
		sr_err("Cannot set threshold voltage %.2fV.", voltage);
		return ret;
	}
	devc->threshold_voltage = voltage;

	return SR_OK;
}

static int enable_pwm(const struct sr_dev_inst *sdi, uint8_t p1, uint8_t p2)
{
	struct dev_context *devc;
	uint8_t cfg;
	int ret;

	devc = sdi->priv;
	cfg = 0;

	if (p1) cfg |= 1 << 0;
	if (p2) cfg |= 1 << 1;

	sr_dbg("Set PWM enable %d %d. Config 0x%02x.", p1, p2, cfg);
	ret = ctrl_out(sdi, CMD_FPGA_SPI, REG_PWM_EN, 0, &cfg, sizeof(cfg));
	if (ret != SR_OK) {
		sr_err("Cannot setup PWM enabled state.");
		return ret;
	}
	devc->pwm_setting[0].enabled = (p1) ? 1 : 0;
	devc->pwm_setting[1].enabled = (p2) ? 1 : 0;

	return SR_OK;
}

static int set_pwm(const struct sr_dev_inst *sdi, uint8_t which, float freq, float duty)
{
	int CTRL_PWM[] = { REG_PWM1, REG_PWM2 };
	struct dev_context *devc;
	pwm_setting_dev_t cfg;
	pwm_setting_t *setting;
	int ret;
	uint8_t buf[2 * sizeof(uint32_t)];
	uint8_t *wrptr;

	devc = sdi->priv;

	if (which < 1 || which > 2) {
		sr_err("Invalid PWM channel: %d.", which);
		return SR_ERR;
	}
	if (freq > MAX_PWM_FREQ) {
		sr_err("Too high a PWM frequency: %.1f.", freq);
		return SR_ERR;
	}
	if (duty > 100 || duty < 0) {
		sr_err("Invalid PWM duty cycle: %f.", duty);
		return SR_ERR;
	}

	cfg.period = (uint32_t)(PWM_CLOCK / freq);
	cfg.duty = (uint32_t)(0.5f + (cfg.period * duty / 100.));
	sr_dbg("Set PWM%d period %d, duty %d.", which, cfg.period, cfg.duty);

	wrptr = buf;
	write_u32le_inc(&wrptr, cfg.period);
	write_u32le_inc(&wrptr, cfg.duty);
	ret = ctrl_out(sdi, CMD_FPGA_SPI, CTRL_PWM[which - 1], 0, buf, wrptr - buf);
	if (ret != SR_OK) {
		sr_err("Cannot setup PWM%d configuration %d %d.",
			which, cfg.period, cfg.duty);
		return ret;
	}
	setting = &devc->pwm_setting[which - 1];
	setting->freq = freq;
	setting->duty = duty;

	return SR_OK;
}

static int set_defaults(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	devc = sdi->priv;

	devc->capture_ratio = 5; /* percent */
	devc->cur_channels = 0xffff;
	devc->limit_samples = 5000000;
	devc->cur_samplerate = SR_MHZ(100);

	ret = set_threshold_voltage(sdi, devc->threshold_voltage);
	if (ret)
		return ret;

	ret = enable_pwm(sdi, 0, 0);
	if (ret)
		return ret;

	ret = set_pwm(sdi, 1, 1e3, 50);
	if (ret)
		return ret;

	ret = set_pwm(sdi, 2, 100e3, 50);
	if (ret)
		return ret;

	ret = enable_pwm(sdi, 1, 1);
	if (ret)
		return ret;

	return SR_OK;
}

static int set_trigger_config(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_trigger *trigger;
	trigger_cfg_t cfg;
	GSList *stages;
	GSList *channel;
	struct sr_trigger_stage *stage1;
	struct sr_trigger_match *match;
	uint16_t ch_mask;
	int ret;
	uint8_t buf[4 * sizeof(uint32_t)];
	uint8_t *wrptr;

	devc = sdi->priv;
	trigger = sr_session_trigger_get(sdi->session);

	memset(&cfg, 0, sizeof(cfg));

	cfg.channels = devc->cur_channels;

	if (trigger && trigger->stages) {
		stages = trigger->stages;
		stage1 = stages->data;
		if (stages->next) {
			sr_err("Only one trigger stage supported for now.");
			return SR_ERR;
		}
		channel = stage1->matches;
		while (channel) {
			match = channel->data;
			ch_mask = 1 << match->channel->index;

			switch (match->match) {
			case SR_TRIGGER_ZERO:
				cfg.level |= ch_mask;
				cfg.high_or_falling &= ~ch_mask;
				break;
			case SR_TRIGGER_ONE:
				cfg.level |= ch_mask;
				cfg.high_or_falling |= ch_mask;
				break;
			case SR_TRIGGER_RISING:
				if ((cfg.enabled & ~cfg.level)) {
					sr_err("Device only supports one edge trigger.");
					return SR_ERR;
				}
				cfg.level &= ~ch_mask;
				cfg.high_or_falling &= ~ch_mask;
				break;
			case SR_TRIGGER_FALLING:
				if ((cfg.enabled & ~cfg.level)) {
					sr_err("Device only supports one edge trigger.");
					return SR_ERR;
				}
				cfg.level &= ~ch_mask;
				cfg.high_or_falling |= ch_mask;
				break;
			default:
				sr_err("Unknown trigger condition.");
				return SR_ERR;
			}
			cfg.enabled |= ch_mask;
			channel = channel->next;
		}
	}
	sr_dbg("Set trigger config: "
		"channels 0x%04x, trigger-enabled 0x%04x, "
		"level-triggered 0x%04x, high/falling 0x%04x.",
		cfg.channels, cfg.enabled, cfg.level, cfg.high_or_falling);

	devc->had_triggers_configured = cfg.enabled != 0;

	wrptr = buf;
	write_u32le_inc(&wrptr, cfg.channels);
	write_u32le_inc(&wrptr, cfg.enabled);
	write_u32le_inc(&wrptr, cfg.level);
	write_u32le_inc(&wrptr, cfg.high_or_falling);
	ret = ctrl_out(sdi, CMD_FPGA_SPI, REG_TRIGGER, 16, buf, wrptr - buf);
	if (ret != SR_OK) {
		sr_err("Cannot setup trigger configuration.");
		return ret;
	}

	return SR_OK;
}

static int set_sample_config(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	double clock_divisor;
	uint64_t total;
	int ret;
	uint16_t divisor;
	uint8_t buf[2 * sizeof(uint32_t) + 48 / 8 + sizeof(uint16_t)];
	uint8_t *wrptr;

	devc = sdi->priv;
	total = 128 * 1024 * 1024;

	if (devc->cur_samplerate > devc->max_samplerate) {
		sr_err("Too high a sample rate: %" PRIu64 ".",
			devc->cur_samplerate);
		return SR_ERR;
	}

	clock_divisor = devc->max_samplerate / (double)devc->cur_samplerate;
	if (clock_divisor > 0xffff)
		clock_divisor = 0xffff;
	divisor = (uint16_t)(clock_divisor + 0.5);
	devc->cur_samplerate = devc->max_samplerate / divisor;

	if (devc->limit_samples > MAX_SAMPLE_DEPTH) {
		sr_err("Too high a sample depth: %" PRIu64 ".",
			devc->limit_samples);
		return SR_ERR;
	}

	devc->pre_trigger_size = (devc->capture_ratio * devc->limit_samples) / 100;

	sr_dbg("Set sample config: %" PRIu64 "kHz, %" PRIu64 " samples, trigger-pos %" PRIu64 "%%.",
		devc->cur_samplerate / 1000,
		devc->limit_samples,
		devc->capture_ratio);

	wrptr = buf;
	write_u32le_inc(&wrptr, devc->limit_samples);
	write_u8_inc(&wrptr, 0);
	write_u32le_inc(&wrptr, devc->pre_trigger_size);
	write_u32le_inc(&wrptr, ((total * devc->capture_ratio) / 100) & 0xffffff00);
	write_u16le_inc(&wrptr, divisor);
	write_u8_inc(&wrptr, 0);

	ret = ctrl_out(sdi, CMD_FPGA_SPI, REG_SAMPLING, 0, buf, wrptr - buf);
	if (ret != SR_OK) {
		sr_err("Cannot setup acquisition configuration.");
		return ret;
	}

	return SR_OK;
}

/*
 * FPGA register REG_RUN holds the run state (u16le format). Bit fields
 * of interest:
 *   bit 0: value 1 = idle
 *   bit 1: value 1 = writing to SDRAM
 *   bit 2: value 0 = waiting for trigger, 1 = trigger seen
 *   bit 3: value 0 = pretrigger sampling, 1 = posttrigger sampling
 * The meaning of other bit fields is unknown.
 *
 * Typical values in order of appearance during execution:
 *   0x85e2: pre-sampling, samples before the trigger position,
 *     when capture ratio > 0%
 *   0x85ea: pre-sampling complete, now waiting for the trigger
 *     (whilst sampling continuously)
 *   0x85ee: trigger seen, capturing post-trigger samples, running
 *   0x85ed: idle
 */
static uint16_t run_state(const struct sr_dev_inst *sdi)
{
	uint16_t state;
	static uint16_t previous_state = 0;
	int ret;

	if ((ret = ctrl_in(sdi, CMD_FPGA_SPI, REG_RUN, 0, &state, sizeof(state))) != SR_OK) {
		sr_err("Cannot read run state.");
		return ret;
	}

	/*
	 * Avoid flooding the log, only dump values as they change.
	 * The routine is called about every 50ms.
	 */
	if (state != previous_state) {
		previous_state = state;
		if ((state & 0x0003) == 0x01) {
			sr_dbg("Run state: 0x%04x (%s).", state, "idle");
		}
		else if ((state & 0x000f) == 0x02) {
			sr_dbg("Run state: 0x%04x (%s).", state,
				"pre-trigger sampling");
		}
		else if ((state & 0x000f) == 0x0a) {
			sr_dbg("Run state: 0x%04x (%s).", state,
				"sampling, waiting for trigger");
		}
		else if ((state & 0x000f) == 0x0e) {
			sr_dbg("Run state: 0x%04x (%s).", state,
				"post-trigger sampling");
		}
		else {
			sr_dbg("Run state: 0x%04x.", state);
		}
	}

	return state;
}

static int set_run_mode(const struct sr_dev_inst *sdi, uint8_t fast_blinking)
{
	int ret;

	if ((ret = ctrl_out(sdi, CMD_FPGA_SPI, REG_RUN, 0, &fast_blinking, sizeof(fast_blinking))) != SR_OK) {
		sr_err("Cannot configure run mode %d.", fast_blinking);
		return ret;
	}

	return SR_OK;
}

static int get_capture_info(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;
	uint8_t buf[3 * sizeof(uint32_t)];
	const uint8_t *rdptr;

	devc = sdi->priv;

	if ((ret = ctrl_in(sdi, CMD_FPGA_SPI, REG_SAMPLING, 0, buf, sizeof(buf))) != SR_OK) {
		sr_err("Cannot read capture info.");
		return ret;
	}

	rdptr = buf;
	devc->info.n_rep_packets = read_u32le_inc(&rdptr);
	devc->info.n_rep_packets_before_trigger = read_u32le_inc(&rdptr);
	devc->info.write_pos = read_u32le_inc(&rdptr);

	sr_dbg("Capture info: n_rep_packets: 0x%08x/%d, before_trigger: 0x%08x/%d, write_pos: 0x%08x%d.",
	       devc->info.n_rep_packets, devc->info.n_rep_packets,
	       devc->info.n_rep_packets_before_trigger,
	       devc->info.n_rep_packets_before_trigger,
	       devc->info.write_pos, devc->info.write_pos);

	if (devc->info.n_rep_packets % 5) {
		sr_warn("Unexpected packets count %lu, not a multiple of 5.",
			(unsigned long)devc->info.n_rep_packets);
	}

	return SR_OK;
}

SR_PRIV int la2016_upload_firmware(struct sr_context *sr_ctx, libusb_device *dev, uint16_t product_id)
{
	char fw_file[1024];
	snprintf(fw_file, sizeof(fw_file) - 1, UC_FIRMWARE, product_id);
	return ezusb_upload_firmware(sr_ctx, dev, USB_CONFIGURATION, fw_file);
}

SR_PRIV int la2016_setup_acquisition(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;
	uint8_t cmd;

	devc = sdi->priv;

	ret = set_threshold_voltage(sdi, devc->threshold_voltage);
	if (ret != SR_OK)
		return ret;

	cmd = 0;
	if ((ret = ctrl_out(sdi, CMD_FPGA_SPI, REG_CAPT_MODE, 0, &cmd, sizeof(cmd))) != SR_OK) {
		sr_err("Cannot send command to stop sampling.");
		return ret;
	}

	ret = set_trigger_config(sdi);
	if (ret != SR_OK)
		return ret;

	ret = set_sample_config(sdi);
	if (ret != SR_OK)
		return ret;

	return SR_OK;
}

SR_PRIV int la2016_start_acquisition(const struct sr_dev_inst *sdi)
{
	int ret;

	ret = set_run_mode(sdi, 3);
	if (ret != SR_OK)
		return ret;

	return SR_OK;
}

static int la2016_stop_acquisition(const struct sr_dev_inst *sdi)
{
	int ret;

	ret = set_run_mode(sdi, 0);
	if (ret != SR_OK)
		return ret;

	return SR_OK;
}

SR_PRIV int la2016_abort_acquisition(const struct sr_dev_inst *sdi)
{
	int ret;
	struct dev_context *devc;

	ret = la2016_stop_acquisition(sdi);
	if (ret != SR_OK)
		return ret;

	devc = sdi ? sdi->priv : NULL;
	if (devc && devc->transfer)
		libusb_cancel_transfer(devc->transfer);

	return SR_OK;
}

static int la2016_has_triggered(const struct sr_dev_inst *sdi)
{
	uint16_t state;

	state = run_state(sdi);

	return (state & 0x3) == 1;
}

static int la2016_start_retrieval(const struct sr_dev_inst *sdi, libusb_transfer_cb_fn cb)
{
	struct dev_context *devc;
	struct sr_usb_dev_inst *usb;
	int ret;
	uint8_t wrbuf[2 * sizeof(uint32_t)];
	uint8_t *wrptr;
	uint32_t to_read;
	uint8_t *buffer;

	devc = sdi->priv;
	usb = sdi->conn;

	if ((ret = get_capture_info(sdi)) != SR_OK)
		return ret;

	devc->n_transfer_packets_to_read = devc->info.n_rep_packets / NUM_PACKETS_IN_CHUNK;
	devc->n_bytes_to_read = devc->n_transfer_packets_to_read * TRANSFER_PACKET_LENGTH;
	devc->read_pos = devc->info.write_pos - devc->n_bytes_to_read;
	devc->n_reps_until_trigger = devc->info.n_rep_packets_before_trigger;

	sr_dbg("Want to read %u xfer-packets starting from pos %" PRIu32 ".",
	       devc->n_transfer_packets_to_read, devc->read_pos);

	if ((ret = ctrl_out(sdi, CMD_BULK_RESET, 0x00, 0, NULL, 0)) != SR_OK) {
		sr_err("Cannot reset USB bulk state.");
		return ret;
	}
	sr_dbg("Will read from 0x%08lx, 0x%08x bytes.",
		(unsigned long)devc->read_pos, devc->n_bytes_to_read);
	wrptr = wrbuf;
	write_u32le_inc(&wrptr, devc->read_pos);
	write_u32le_inc(&wrptr, devc->n_bytes_to_read);
	if ((ret = ctrl_out(sdi, CMD_FPGA_SPI, REG_BULK, 0, wrbuf, wrptr - wrbuf)) != SR_OK) {
		sr_err("Cannot send USB bulk config.");
		return ret;
	}
	if ((ret = ctrl_out(sdi, CMD_BULK_START, 0x00, 0, NULL, 0)) != SR_OK) {
		sr_err("Cannot unblock USB bulk transfers.");
		return ret;
	}

	/*
	 * Pick a buffer size for all USB transfers. The buffer size
	 * must be a multiple of the endpoint packet size. And cannot
	 * exceed a maximum value.
	 */
	to_read = devc->n_bytes_to_read;
	if (to_read >= LA2016_USB_BUFSZ) /* Multiple transfers. */
		to_read = LA2016_USB_BUFSZ;
	else /* One transfer. */
		to_read = (to_read + (LA2016_EP6_PKTSZ-1)) & ~(LA2016_EP6_PKTSZ-1);
	buffer = g_try_malloc(to_read);
	if (!buffer) {
		sr_dbg("USB bulk transfer size %d bytes.", (int)to_read);
		sr_err("Cannot allocate buffer for USB bulk transfer.");
		return SR_ERR_MALLOC;
	}

	devc->transfer = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(
		devc->transfer, usb->devhdl,
		0x86, buffer, to_read,
		cb, (void *)sdi, DEFAULT_TIMEOUT_MS);

	if ((ret = libusb_submit_transfer(devc->transfer)) != 0) {
		sr_err("Cannot submit USB transfer: %s.", libusb_error_name(ret));
		libusb_free_transfer(devc->transfer);
		devc->transfer = NULL;
		g_free(buffer);
		return SR_ERR;
	}

	return SR_OK;
}

static void send_chunk(struct sr_dev_inst *sdi,
	const uint8_t *packets, unsigned int num_tfers)
{
	struct dev_context *devc;
	struct sr_datafeed_logic logic;
	struct sr_datafeed_packet sr_packet;
	unsigned int max_samples, n_samples, total_samples, free_n_samples;
	unsigned int i, j, k;
	int do_signal_trigger;
	uint16_t *wp;
	const uint8_t *rp;
	uint16_t state;
	uint8_t repetitions;

	devc = sdi->priv;

	logic.unitsize = 2;
	logic.data = devc->convbuffer;

	sr_packet.type = SR_DF_LOGIC;
	sr_packet.payload = &logic;

	max_samples = devc->convbuffer_size / 2;
	n_samples = 0;
	wp = (uint16_t *)devc->convbuffer;
	total_samples = 0;
	do_signal_trigger = 0;

	if (devc->had_triggers_configured && devc->reading_behind_trigger == 0 && devc->info.n_rep_packets_before_trigger == 0) {
		std_session_send_df_trigger(sdi);
		devc->reading_behind_trigger = 1;
	}

	rp = packets;
	for (i = 0; i < num_tfers; i++) {
		for (k = 0; k < NUM_PACKETS_IN_CHUNK; k++) {
			free_n_samples = max_samples - n_samples;
			if (free_n_samples < 256 || do_signal_trigger) {
				logic.length = n_samples * 2;
				sr_session_send(sdi, &sr_packet);
				n_samples = 0;
				wp = (uint16_t *)devc->convbuffer;
				if (do_signal_trigger) {
					std_session_send_df_trigger(sdi);
					do_signal_trigger = 0;
				}
			}

			state = read_u16le_inc(&rp);
			repetitions = read_u8_inc(&rp);
			for (j = 0; j < repetitions; j++)
				*wp++ = state;

			n_samples += repetitions;
			total_samples += repetitions;
			devc->total_samples += repetitions;
			if (!devc->reading_behind_trigger) {
				devc->n_reps_until_trigger--;
				if (devc->n_reps_until_trigger == 0) {
					devc->reading_behind_trigger = 1;
					do_signal_trigger = 1;
					sr_dbg("Trigger position after %" PRIu64 " samples, %.6fms.",
					       devc->total_samples,
					       (double)devc->total_samples / devc->cur_samplerate * 1e3);
				}
			}
		}
		(void)read_u8_inc(&rp); /* Skip sequence number. */
	}
	if (n_samples) {
		logic.length = n_samples * 2;
		sr_session_send(sdi, &sr_packet);
		if (do_signal_trigger) {
			std_session_send_df_trigger(sdi);
		}
	}
	sr_dbg("Send_chunk done after %u samples.", total_samples);
}

static void LIBUSB_CALL receive_transfer(struct libusb_transfer *transfer)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_usb_dev_inst *usb;
	int ret;

	sdi = transfer->user_data;
	devc = sdi->priv;
	usb = sdi->conn;

	sr_dbg("receive_transfer(): status %s received %d bytes.",
	       libusb_error_name(transfer->status), transfer->actual_length);

	if (transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
		sr_err("USB bulk transfer timeout.");
		devc->transfer_finished = 1;
	}
	send_chunk(sdi, transfer->buffer, transfer->actual_length / TRANSFER_PACKET_LENGTH);

	devc->n_bytes_to_read -= transfer->actual_length;
	if (devc->n_bytes_to_read) {
		uint32_t to_read = devc->n_bytes_to_read;
		/*
		 * Determine read size for the next USB transfer. Make
		 * the buffer size a multiple of the endpoint packet
		 * size. Don't exceed a maximum value.
		 */
		if (to_read >= LA2016_USB_BUFSZ)
			to_read = LA2016_USB_BUFSZ;
		else
			to_read = (to_read + (LA2016_EP6_PKTSZ-1)) & ~(LA2016_EP6_PKTSZ-1);
		libusb_fill_bulk_transfer(
			transfer, usb->devhdl,
			0x86, transfer->buffer, to_read,
			receive_transfer, (void *)sdi, DEFAULT_TIMEOUT_MS);

		if ((ret = libusb_submit_transfer(transfer)) == 0)
			return;
		sr_err("Cannot submit another USB transfer: %s.",
			libusb_error_name(ret));
	}

	g_free(transfer->buffer);
	libusb_free_transfer(transfer);
	devc->transfer_finished = 1;
}

SR_PRIV int la2016_receive_data(int fd, int revents, void *cb_data)
{
	const struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct drv_context *drvc;
	struct timeval tv;

	(void)fd;
	(void)revents;

	sdi = cb_data;
	devc = sdi->priv;
	drvc = sdi->driver->context;

	if (devc->have_trigger == 0) {
		if (la2016_has_triggered(sdi) == 0) {
			/* Not yet ready for sample data download. */
			return TRUE;
		}
		devc->have_trigger = 1;
		devc->transfer_finished = 0;
		devc->reading_behind_trigger = 0;
		devc->total_samples = 0;
		/* We can start downloading sample data. */
		if (la2016_start_retrieval(sdi, receive_transfer) != SR_OK) {
			sr_err("Cannot start acquisition data download.");
			return FALSE;
		}
		sr_dbg("Acquisition data download started.");
		std_session_send_df_frame_begin(sdi);

		return TRUE;
	}

	tv.tv_sec = tv.tv_usec = 0;
	libusb_handle_events_timeout(drvc->sr_ctx->libusb_ctx, &tv);

	if (devc->transfer_finished) {
		sr_dbg("Download finished, post processing.");
		std_session_send_df_frame_end(sdi);

		usb_source_remove(sdi->session, drvc->sr_ctx);
		std_session_send_df_end(sdi);

		la2016_stop_acquisition(sdi);

		g_free(devc->convbuffer);
		devc->convbuffer = NULL;

		devc->transfer = NULL;

		sr_dbg("Download finished, done post processing.");
	}

	return TRUE;
}

SR_PRIV int la2016_init_device(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	uint16_t state;
	uint8_t buf[8];
	int16_t purchase_date_bcd[2];
	uint8_t magic;
	int ret;

	devc = sdi->priv;

	/*
	 * Four EEPROM bytes at offset 0x20 are purchase year and month
	 * in BCD format, with 16bit complemented checksum. For example
	 * 20 04 df fb translates to 2020-04. This can help identify the
	 * age of devices when unknown magic numbers are seen.
	 */
	if ((ret = ctrl_in(sdi, CMD_EEPROM, 0x20, 0, purchase_date_bcd, sizeof(purchase_date_bcd))) != SR_OK) {
		sr_err("Cannot read purchase date in EEPROM.");
	}
	else {
		sr_dbg("Purchase date: 20%02hx-%02hx.",
			(purchase_date_bcd[0]) & 0xff,
			(purchase_date_bcd[0] >> 8) & 0xff);
		if (purchase_date_bcd[0] != (0x0ffff & ~purchase_date_bcd[1])) {
			sr_err("Purchase date fails checksum test.");
		}
	}

	/*
	 * Several Kingst logic analyzer devices share the same USB VID
	 * and PID. The product ID determines which MCU firmware to load.
	 * The MCU firmware provides access to EEPROM content which then
	 * allows to identify the device model. Which in turn determines
	 * which FPGA bitstream to load. Eight bytes at offset 0x08 are
	 * to get inspected.
	 *
	 * EEPROM content for model identification is kept redundantly
	 * in memory. The values are stored in verbatim and in inverted
	 * form, multiple copies are kept at different offsets. Example
	 * data:
	 *
	 *   magic 0x08
	 *    | ~magic 0xf7
	 *    | |
	 *   08f7000008f710ef
	 *            | |
	 *            | ~magic backup
	 *            magic backup
	 *
	 * Exclusively inspecting the magic byte appears to be sufficient,
	 * other fields seem to be 'don't care'.
	 *
	 *   magic 2 == LA2016 using "kingst-la2016-fpga.bitstream"
	 *   magic 3 == LA1016 using "kingst-la1016-fpga.bitstream"
	 *   magic 8 == LA2016a using "kingst-la2016a1-fpga.bitstream"
	 *              (latest v1.3.0 PCB, perhaps others)
	 *   magic 9 == LA1016a using "kingst-la1016a1-fpga.bitstream"
	 *              (latest v1.3.0 PCB, perhaps others)
	 *
	 * When EEPROM content does not match the hardware configuration
	 * (the board layout), the software may load but yield incorrect
	 * results (like swapped channels). The FPGA bitstream itself
	 * will authenticate with IC U10 and fail when its capabilities
	 * do not match the hardware model. An LA1016 won't become a
	 * LA2016 by faking its EEPROM content.
	 */
	if ((ret = ctrl_in(sdi, CMD_EEPROM, 0x08, 0, &buf, sizeof(buf))) != SR_OK) {
		sr_err("Cannot read EEPROM device identifier bytes.");
		return ret;
	}

	magic = 0;
	if (buf[0] == (0xff & ~buf[1])) {
		/* Primary copy of magic passes complement check. */
		magic = buf[0];
	}
	else if (buf[4] == (0x0ff & ~buf[5])) {
		/* Backup copy of magic passes complement check. */
		sr_dbg("Using backup copy of device type magic number.");
		magic = buf[4];
	}

	sr_dbg("Device type: magic number is %hhu.", magic);

	/* Select the FPGA bitstream depending on the model. */
	switch (magic) {
	case 2:
		ret = upload_fpga_bitstream(sdi, FPGA_FW_LA2016);
		devc->max_samplerate = MAX_SAMPLE_RATE_LA2016;
		break;
	case 3:
		ret = upload_fpga_bitstream(sdi, FPGA_FW_LA1016);
		devc->max_samplerate = MAX_SAMPLE_RATE_LA1016;
		break;
	case 8:
		ret = upload_fpga_bitstream(sdi, FPGA_FW_LA2016A);
		devc->max_samplerate = MAX_SAMPLE_RATE_LA2016;
		break;
	case 9:
		ret = upload_fpga_bitstream(sdi, FPGA_FW_LA1016A);
		devc->max_samplerate = MAX_SAMPLE_RATE_LA1016;
		break;
	default:
		sr_err("Cannot identify as one of the supported models.");
		return SR_ERR;
	}

	if (ret != SR_OK) {
		sr_err("Cannot upload FPGA bitstream.");
		return ret;
	}

	state = run_state(sdi);
	if (state != 0x85e9) {
		sr_warn("Unexpected run state, want 0x85e9, got 0x%04x.", state);
	}

	if ((ret = ctrl_out(sdi, CMD_BULK_RESET, 0x00, 0, NULL, 0)) != SR_OK) {
		sr_err("Cannot reset USB bulk transfer.");
		return ret;
	}

	sr_dbg("Device should be initialized.");

	return set_defaults(sdi);
}

SR_PRIV int la2016_deinit_device(const struct sr_dev_inst *sdi)
{
	int ret;

	if ((ret = ctrl_out(sdi, CMD_FPGA_ENABLE, 0x00, 0, NULL, 0)) != SR_OK) {
		sr_err("Cannot deinitialize device's FPGA.");
		return ret;
	}

	return SR_OK;
}
