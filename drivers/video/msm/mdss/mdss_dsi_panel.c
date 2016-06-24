/* Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/qpnp/pin.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/qpnp/pwm.h>
#include <linux/err.h>
#include <linux/string.h>

#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
#include <soc/qcom/lge/board_lge.h>
#endif

#if defined(CONFIG_LGE_PP_AD_SUPPORTED)
extern void qpnp_wled_dimming(int dst_lvl,int current_lvl);
#endif

#include "mdss_dsi.h"

#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
#include "oem_mdss_aod.h"
#endif
#include <linux/input/lge_touch_notify.h>
#endif

#if defined(CONFIG_LGE_LCD_TUNING)
extern int tun_lcd[128];
extern char read_cmd[128];
extern int reg_num;
int cmd_num;
#endif

#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
unsigned int cabc_ctrl;
#endif

#if defined(CONFIG_MACH_LGE)
struct mdss_panel_data *pdata_base;
#endif

#define DT_CMD_HDR 6
#define MIN_REFRESH_RATE 30
#define DEFAULT_MDP_TRANSFER_TIME 14000

#define CEIL(x, y)		(((x) + ((y)-1)) / (y))

#define VSYNC_DELAY msecs_to_jiffies(17)

DEFINE_LED_TRIGGER(bl_led_trigger);

/*
 * rc_buf_thresh = {896, 1792, 2688, 3548, 4480, 5376, 6272, 6720,
 *		7168, 7616, 7744, 7872, 8000, 8064, 8192};
 *	(x >> 6) & 0x0ff)
 */
static u32 dsc_rc_buf_thresh[] = {0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54,
		0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e};
static char dsc_rc_range_min_qp[] = {0, 0, 1, 1, 3, 3, 3, 3, 3, 3, 5,
				5, 5, 7, 13};
static char dsc_rc_range_max_qp[] = {4, 4, 5, 6, 7, 7, 7, 8, 9, 10, 11,
			 12, 13, 13, 15};
static char dsc_rc_range_bpg_offset[] = {2, 0, 0, -2, -4, -6, -8, -8,
			-8, -10, -10, -12, -12, -12, -12};

void mdss_dsi_panel_pwm_cfg(struct mdss_dsi_ctrl_pdata *ctrl)
{
	if (ctrl->pwm_pmi)
		return;

	ctrl->pwm_bl = pwm_request(ctrl->pwm_lpg_chan, "lcd-bklt");
	if (ctrl->pwm_bl == NULL || IS_ERR(ctrl->pwm_bl)) {
		pr_err("%s: Error: lpg_chan=%d pwm request failed",
				__func__, ctrl->pwm_lpg_chan);
	}
	ctrl->pwm_enabled = 0;
}

bool mdss_dsi_panel_pwm_enable(struct mdss_dsi_ctrl_pdata *ctrl)
{
	bool status = true;
	if (!ctrl->pwm_enabled)
		goto end;

	if (pwm_enable(ctrl->pwm_bl)) {
		pr_err("%s: pwm_enable() failed\n", __func__);
		status = false;
	}

	ctrl->pwm_enabled = 1;

end:
	return status;
}

static void mdss_dsi_panel_bklt_pwm(struct mdss_dsi_ctrl_pdata *ctrl, int level)
{
	int ret;
	u32 duty;
	u32 period_ns;

	if (ctrl->pwm_bl == NULL) {
		pr_err("%s: no PWM\n", __func__);
		return;
	}

	if (level == 0) {
		if (ctrl->pwm_enabled) {
			ret = pwm_config_us(ctrl->pwm_bl, level,
					ctrl->pwm_period);
			if (ret)
				pr_err("%s: pwm_config_us() failed err=%d.\n",
						__func__, ret);
			pwm_disable(ctrl->pwm_bl);
		}
		ctrl->pwm_enabled = 0;
		return;
	}

	duty = level * ctrl->pwm_period;
	duty /= ctrl->bklt_max;

	pr_debug("%s: bklt_ctrl=%d pwm_period=%d pwm_gpio=%d pwm_lpg_chan=%d\n",
			__func__, ctrl->bklt_ctrl, ctrl->pwm_period,
				ctrl->pwm_pmic_gpio, ctrl->pwm_lpg_chan);

	pr_debug("%s: ndx=%d level=%d duty=%d\n", __func__,
					ctrl->ndx, level, duty);

	if (ctrl->pwm_period >= USEC_PER_SEC) {
		ret = pwm_config_us(ctrl->pwm_bl, duty, ctrl->pwm_period);
		if (ret) {
			pr_err("%s: pwm_config_us() failed err=%d.\n",
					__func__, ret);
			return;
		}
	} else {
		period_ns = ctrl->pwm_period * NSEC_PER_USEC;
		ret = pwm_config(ctrl->pwm_bl,
				level * period_ns / ctrl->bklt_max,
				period_ns);
		if (ret) {
			pr_err("%s: pwm_config() failed err=%d.\n",
					__func__, ret);
			return;
		}
	}

	if (!ctrl->pwm_enabled) {
		ret = pwm_enable(ctrl->pwm_bl);
		if (ret)
			pr_err("%s: pwm_enable() failed err=%d\n", __func__,
				ret);
		ctrl->pwm_enabled = 1;
	}
}

static char dcs_cmd[2] = {0x54, 0x00}; /* DTYPE_DCS_READ */
static struct dsi_cmd_desc dcs_read_cmd = {
	{DTYPE_DCS_READ, 1, 0, 1, 5, sizeof(dcs_cmd)},
	dcs_cmd
};

u32 mdss_dsi_panel_cmd_read(struct mdss_dsi_ctrl_pdata *ctrl, char cmd0,
		char cmd1, void (*fxn)(int), char *rbuf, int len)
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return -EINVAL;
	}

	dcs_cmd[0] = cmd0;
	dcs_cmd[1] = cmd1;
	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = &dcs_read_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_RX | CMD_REQ_COMMIT;
	cmdreq.rlen = len;
	cmdreq.rbuf = rbuf;
	cmdreq.cb = fxn; /* call back */
	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
	/*
	 * blocked here, until call back called
	 */

	return 0;
}

#if defined (CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
#if defined (CONFIG_LGE_LCD_TUNING)
char lge_dcs_cmd[2] = {0x0A, 0x00}; /* DTYPE_DCS_READ */
struct dsi_cmd_desc lge_dcs_read_cmd = {
	{DTYPE_DCS_READ, 1, 0, 1, 5, sizeof(lge_dcs_cmd)},
	lge_dcs_cmd
};

void lge_force_mdss_dsi_panel_cmd_read(char cmd0, int cnt)
{
  struct dcs_cmd_req cmdreq;
  struct mdss_dsi_ctrl_pdata *ctrl;
  char rx_buf[128] = {0x0};
  int i = 0;

  ctrl = container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
				panel_data);
  lge_dcs_cmd[0] = cmd0;
  memset(&cmdreq, 0, sizeof(cmdreq));
  cmdreq.cmds = &lge_dcs_read_cmd;
  cmdreq.cmds_cnt = 1;
  cmdreq.flags = CMD_REQ_RX | CMD_REQ_COMMIT;
  cmdreq.rlen = cnt;
  cmdreq.cb = NULL;
  cmdreq.rbuf = rx_buf;

  if (ctrl->status_cmds.link_state == DSI_LP_MODE)
         cmdreq.flags |= CMD_REQ_LP_MODE;
  else if (ctrl->status_cmds.link_state == DSI_HS_MODE)
         cmdreq.flags |= CMD_REQ_HS_MODE;

  mdelay(1);

  mdss_dsi_cmdlist_put(ctrl, &cmdreq);

  for(i=0;i<cnt;i++)
	pr_info("[Display]Reg[0x%x],buf[%d]=0x%x,mode[%d]\n",cmd0, i, rx_buf[i], ctrl->status_cmds.link_state);
}
#endif
#endif

#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
void mdss_dsi_panel_cmds_send(struct mdss_dsi_ctrl_pdata *ctrl,
			struct dsi_panel_cmds *pcmds, u32 flags)
#else
static void mdss_dsi_panel_cmds_send(struct mdss_dsi_ctrl_pdata *ctrl,
			struct dsi_panel_cmds *pcmds, u32 flags)
#endif
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return;
	}

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = pcmds->cmds;
	cmdreq.cmds_cnt = pcmds->cmd_cnt;
	cmdreq.flags = flags;

	/*Panel ON/Off commands should be sent in DSI Low Power Mode*/
	if (pcmds->link_state == DSI_LP_MODE)
		cmdreq.flags  |= CMD_REQ_LP_MODE;
	else if (pcmds->link_state == DSI_HS_MODE)
		cmdreq.flags |= CMD_REQ_HS_MODE;

	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}

static char led_pwm1[2] = {0x51, 0x0};	/* DTYPE_DCS_WRITE1 */
static struct dsi_cmd_desc backlight_cmd = {
	{DTYPE_DCS_WRITE1, 1, 0, 0, 1, sizeof(led_pwm1)},
	led_pwm1
};

static void mdss_dsi_panel_bklt_dcs(struct mdss_dsi_ctrl_pdata *ctrl, int level)
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return;
	}

	pr_debug("%s: level=%d\n", __func__, level);

	led_pwm1[1] = (unsigned char)level;

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = &backlight_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}

static int mdss_dsi_request_gpios(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	int rc = 0;

	if (gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
		rc = gpio_request(ctrl_pdata->disp_en_gpio,
						"disp_enable");
		if (rc) {
			pr_err("request disp_en gpio failed, rc=%d\n",
				       rc);
			goto disp_en_gpio_err;
		}
	}
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	/* Reset GPIO already requested when parse gpio */
#else
	rc = gpio_request(ctrl_pdata->rst_gpio, "disp_rst_n");
	if (rc) {
		pr_err("request reset gpio failed, rc=%d\n",
			rc);
		goto rst_gpio_err;
	}
#endif
	if (gpio_is_valid(ctrl_pdata->bklt_en_gpio)) {
		rc = gpio_request(ctrl_pdata->bklt_en_gpio,
						"bklt_enable");
		if (rc) {
			pr_err("request bklt gpio failed, rc=%d\n",
				       rc);
			goto bklt_en_gpio_err;
		}
	}
	if (gpio_is_valid(ctrl_pdata->mode_gpio)) {
		rc = gpio_request(ctrl_pdata->mode_gpio, "panel_mode");
		if (rc) {
			pr_err("request panel mode gpio failed,rc=%d\n",
								rc);
			goto mode_gpio_err;
		}
	}

	return rc;

mode_gpio_err:
	if (gpio_is_valid(ctrl_pdata->bklt_en_gpio))
		gpio_free(ctrl_pdata->bklt_en_gpio);
bklt_en_gpio_err:
	gpio_free(ctrl_pdata->rst_gpio);
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
#else
rst_gpio_err:
#endif
	if (gpio_is_valid(ctrl_pdata->disp_en_gpio))
		gpio_free(ctrl_pdata->disp_en_gpio);
disp_en_gpio_err:
	return rc;
}

int mdss_dsi_panel_reset(struct mdss_panel_data *pdata, int enable)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_panel_info *pinfo = NULL;
	int i, rc = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	if (mdss_dsi_is_right_ctrl(ctrl_pdata) &&
		mdss_dsi_is_hw_config_split(ctrl_pdata->shared_data)) {
		pr_debug("%s:%d, right ctrl gpio configuration not needed\n",
			__func__, __LINE__);
		return rc;
	}

	if (!gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
		pr_debug("%s:%d, reset line not configured\n",
			   __func__, __LINE__);
	}

	if (!gpio_is_valid(ctrl_pdata->rst_gpio)) {
		pr_debug("%s:%d, reset line not configured\n",
			   __func__, __LINE__);
		return rc;
	}

	pr_debug("%s: enable = %d\n", __func__, enable);
	pinfo = &(ctrl_pdata->panel_data.panel_info);

	if (enable) {
		rc = mdss_dsi_request_gpios(ctrl_pdata);
		if (rc) {
			pr_err("gpio request failed\n");
			return rc;
		}
		if (!pinfo->cont_splash_enabled) {
			if (gpio_is_valid(ctrl_pdata->disp_en_gpio))
				gpio_set_value((ctrl_pdata->disp_en_gpio), 1);
#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
			/* Only panel reset when U0 -> U3 or U0 -> U2 Unblank*/
			if (pinfo->aod_cmd_mode != ON_CMD && pinfo->aod_cmd_mode != ON_AND_AOD && !pinfo->panel_dead)
				return rc;
#endif
			/* Notify to Touch when panel reset */
#if defined (CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
			if (touch_notifier_call_chain(NOTIFY_TOUCH_RESET, NULL))
				pr_err("Failt to send notify to touch\n");
#endif
			for (i = 0; i < pdata->panel_info.rst_seq_len; ++i) {
				gpio_set_value((ctrl_pdata->rst_gpio),
					pdata->panel_info.rst_seq[i]);
				if (pdata->panel_info.rst_seq[++i])
					usleep_range(pinfo->rst_seq[i] * 1000, pinfo->rst_seq[i] * 1000);
			}

			if (gpio_is_valid(ctrl_pdata->bklt_en_gpio))
				gpio_set_value((ctrl_pdata->bklt_en_gpio), 1);
		}

		if (gpio_is_valid(ctrl_pdata->mode_gpio)) {
			if (pinfo->mode_gpio_state == MODE_GPIO_HIGH)
				gpio_set_value((ctrl_pdata->mode_gpio), 1);
			else if (pinfo->mode_gpio_state == MODE_GPIO_LOW)
				gpio_set_value((ctrl_pdata->mode_gpio), 0);
		}
		if (ctrl_pdata->ctrl_state & CTRL_STATE_PANEL_INIT) {
			pr_debug("%s: Panel Not properly turned OFF\n",
						__func__);
			ctrl_pdata->ctrl_state &= ~CTRL_STATE_PANEL_INIT;
			pr_debug("%s: Reset panel done\n", __func__);
		}
	} else {
		if (gpio_is_valid(ctrl_pdata->bklt_en_gpio)) {
			gpio_set_value((ctrl_pdata->bklt_en_gpio), 0);
			gpio_free(ctrl_pdata->bklt_en_gpio);
		}
		if (gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
			gpio_set_value((ctrl_pdata->disp_en_gpio), 0);
			gpio_free(ctrl_pdata->disp_en_gpio);
		}
		gpio_set_value((ctrl_pdata->rst_gpio), 0);
		gpio_free(ctrl_pdata->rst_gpio);
		if (gpio_is_valid(ctrl_pdata->mode_gpio))
			gpio_free(ctrl_pdata->mode_gpio);
	}
	return rc;
}

/**
 * mdss_dsi_roi_merge() -  merge two roi into single roi
 *
 * Function used by partial update with only one dsi intf take 2A/2B
 * (column/page) dcs commands.
 */
static int mdss_dsi_roi_merge(struct mdss_dsi_ctrl_pdata *ctrl,
					struct mdss_rect *roi)
{
	struct mdss_panel_info *l_pinfo;
	struct mdss_rect *l_roi;
	struct mdss_rect *r_roi;
	struct mdss_dsi_ctrl_pdata *other = NULL;
	int ans = 0;

	if (ctrl->ndx == DSI_CTRL_LEFT) {
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_RIGHT);
		if (!other)
			return ans;
		l_pinfo = &(ctrl->panel_data.panel_info);
		l_roi = &(ctrl->panel_data.panel_info.roi);
		r_roi = &(other->panel_data.panel_info.roi);
	} else  {
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_LEFT);
		if (!other)
			return ans;
		l_pinfo = &(other->panel_data.panel_info);
		l_roi = &(other->panel_data.panel_info.roi);
		r_roi = &(ctrl->panel_data.panel_info.roi);
	}

	if (l_roi->w == 0 && l_roi->h == 0) {
		/* right only */
		*roi = *r_roi;
		roi->x += l_pinfo->xres;/* add left full width to x-offset */
	} else {
		/* left only and left+righ */
		*roi = *l_roi;
		roi->w +=  r_roi->w; /* add right width */
		ans = 1;
	}

	return ans;
}

static char caset[] = {0x2a, 0x00, 0x00, 0x03, 0x00};	/* DTYPE_DCS_LWRITE */
static char paset[] = {0x2b, 0x00, 0x00, 0x05, 0x00};	/* DTYPE_DCS_LWRITE */

/* pack into one frame before sent */
static struct dsi_cmd_desc set_col_page_addr_cmd[] = {
	{{DTYPE_DCS_LWRITE, 0, 0, 0, 1, sizeof(caset)}, caset},	/* packed */
	{{DTYPE_DCS_LWRITE, 1, 0, 0, 1, sizeof(paset)}, paset},
};

static void mdss_dsi_send_col_page_addr(struct mdss_dsi_ctrl_pdata *ctrl,
				struct mdss_rect *roi, int unicast)
{
	struct dcs_cmd_req cmdreq;

	caset[1] = (((roi->x) & 0xFF00) >> 8);
	caset[2] = (((roi->x) & 0xFF));
	caset[3] = (((roi->x - 1 + roi->w) & 0xFF00) >> 8);
	caset[4] = (((roi->x - 1 + roi->w) & 0xFF));
	set_col_page_addr_cmd[0].payload = caset;

	paset[1] = (((roi->y) & 0xFF00) >> 8);
	paset[2] = (((roi->y) & 0xFF));
	paset[3] = (((roi->y - 1 + roi->h) & 0xFF00) >> 8);
	paset[4] = (((roi->y - 1 + roi->h) & 0xFF));
	set_col_page_addr_cmd[1].payload = paset;

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds_cnt = 2;
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	if (unicast)
		cmdreq.flags |= CMD_REQ_UNICAST;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	cmdreq.cmds = set_col_page_addr_cmd;
	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}

static int mdss_dsi_set_col_page_addr(struct mdss_panel_data *pdata,
		bool force_send)
{
	struct mdss_panel_info *pinfo;
	struct mdss_rect roi = {0};
	struct mdss_rect *p_roi;
	struct mdss_rect *c_roi;
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_dsi_ctrl_pdata *other = NULL;
	int left_or_both = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pinfo = &pdata->panel_info;
	p_roi = &pinfo->roi;

	/*
	 * to avoid keep sending same col_page info to panel,
	 * if roi_merge enabled, the roi of left ctrl is used
	 * to compare against new merged roi and saved new
	 * merged roi to it after comparing.
	 * if roi_merge disabled, then the calling ctrl's roi
	 * and pinfo's roi are used to compare.
	 */
	if (pinfo->partial_update_roi_merge) {
		left_or_both = mdss_dsi_roi_merge(ctrl, &roi);
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_LEFT);
		c_roi = &other->roi;
	} else {
		c_roi = &ctrl->roi;
		roi = *p_roi;
	}

	/* roi had changed, do col_page update */
	if (force_send || !mdss_rect_cmp(c_roi, &roi)) {
		pr_debug("%s: ndx=%d x=%d y=%d w=%d h=%d\n",
				__func__, ctrl->ndx, p_roi->x,
				p_roi->y, p_roi->w, p_roi->h);

		*c_roi = roi; /* keep to ctrl */
		if (c_roi->w == 0 || c_roi->h == 0) {
			/* no new frame update */
			pr_debug("%s: ctrl=%d, no partial roi set\n",
						__func__, ctrl->ndx);
			return 0;
		}

		if (pinfo->dcs_cmd_by_left) {
			if (left_or_both && ctrl->ndx == DSI_CTRL_RIGHT) {
				/* 2A/2B sent by left already */
				return 0;
			}
		}

		if (!mdss_dsi_sync_wait_enable(ctrl)) {
			if (pinfo->dcs_cmd_by_left)
				ctrl = mdss_dsi_get_ctrl_by_index(
							DSI_CTRL_LEFT);
			mdss_dsi_send_col_page_addr(ctrl, &roi, 0);
		} else {
			/*
			 * when sync_wait_broadcast enabled,
			 * need trigger at right ctrl to
			 * start both dcs cmd transmission
			 */
			other = mdss_dsi_get_other_ctrl(ctrl);
			if (!other)
				goto end;

			if (mdss_dsi_is_left_ctrl(ctrl)) {
				if (pinfo->partial_update_roi_merge) {
					/*
					 * roi is the one after merged
					 * to dsi-1 only
					 */
					mdss_dsi_send_col_page_addr(other,
							&roi, 0);
				} else {
					mdss_dsi_send_col_page_addr(ctrl,
							&ctrl->roi, 1);
					mdss_dsi_send_col_page_addr(other,
							&other->roi, 1);
				}
			} else {
				if (pinfo->partial_update_roi_merge) {
					/*
					 * roi is the one after merged
					 * to dsi-1 only
					 */
					mdss_dsi_send_col_page_addr(ctrl,
							&roi, 0);
				} else {
					mdss_dsi_send_col_page_addr(other,
							&other->roi, 1);
					mdss_dsi_send_col_page_addr(ctrl,
							&ctrl->roi, 1);
				}
			}
		}
	}

end:
	return 0;
}
#if defined(CONFIG_LGE_ENHANCE_GALLERY_SHARPNESS)
static ssize_t sharpness_get(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;

	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);

	return sprintf(buf, "%x\n", ctrl->sharpness_on_cmds.cmds[2].payload[3]);

}

static ssize_t sharpness_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	unsigned int param;
	ssize_t ret = strnlen(buf, PAGE_SIZE);

	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	if(pdata_base->panel_info.panel_power_state == 0){
		pr_err("%s: Panel off state. Ignore sharpness enhancement cmd\n", __func__);
		return -EINVAL;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);

	sscanf(buf, "%x", &param);
	ctrl->sharpness_on_cmds.cmds[2].payload[3] = param;

	mdss_dsi_panel_cmds_send(ctrl, &ctrl->sharpness_on_cmds, CMD_REQ_COMMIT);

	pr_info("%s: sent sharpness enhancement cmd : 0x%02X\n",
			__func__, ctrl->sharpness_on_cmds.cmds[2].payload[3]);

	return ret;
}

static ssize_t color_enhancement_get(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int i=0;
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;

	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);

	sprintf(buf, "%s 0x%02X 0x%02X\n", buf, ctrl->ce_on_cmds.cmds[1].payload[0]
		,ctrl->ce_on_cmds.cmds[1].payload[1]);

	for(i=0; i<24; i++){
		sprintf(buf, "%s 0x%02X", buf, ctrl->ce_on_cmds.cmds[2].payload[i]);
		if(((i+1)%6) == 0)
			sprintf(buf, "%s \n", buf);
	}

	return sprintf(buf, "%s\n", buf);
}

static ssize_t color_enhancement_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	ssize_t ret = strnlen(buf, PAGE_SIZE);
	int set_color_enhancement[128];
	int i, ie_function;

	struct mdss_dsi_ctrl_pdata *ctrl = NULL;

	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	if(pdata_base->panel_info.panel_power_state == 0){
		pr_err("%s: Panel off state. Ignore color enhancement cmd\n", __func__);
		return -EINVAL;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);

	memset(set_color_enhancement,0,24*sizeof(int));
	set_color_enhancement[0] = ctrl->ce_on_cmds.cmds[2].payload[0];
	sscanf(buf, "%x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
		&ie_function,&set_color_enhancement[1],&set_color_enhancement[2],&set_color_enhancement[3],
		&set_color_enhancement[4],&set_color_enhancement[5],&set_color_enhancement[6],&set_color_enhancement[7],
		&set_color_enhancement[8],&set_color_enhancement[9],&set_color_enhancement[10],&set_color_enhancement[11],
		&set_color_enhancement[12],&set_color_enhancement[13],&set_color_enhancement[14],&set_color_enhancement[15],
		&set_color_enhancement[16],&set_color_enhancement[17],&set_color_enhancement[18],&set_color_enhancement[19],
		&set_color_enhancement[20],&set_color_enhancement[21],&set_color_enhancement[22],&set_color_enhancement[23]);

	ctrl->ce_on_cmds.cmds[1].payload[1] = ie_function;
	pr_debug("%s: 0xF0 0x%02X ", __func__, ctrl->ce_on_cmds.cmds[1].payload[1]);

	for(i=0; i<24; i++){
		ctrl->ce_on_cmds.cmds[2].payload[i] = set_color_enhancement[i];
		pr_debug("0x%02X ", ctrl->ce_on_cmds.cmds[2].payload[i]);
	}
	pr_debug("\n");

	mdss_dsi_panel_cmds_send(ctrl, &ctrl->ce_on_cmds, CMD_REQ_COMMIT);
	return ret;
}

static DEVICE_ATTR(sharpness, S_IWUSR|S_IRUGO, sharpness_get, sharpness_set);
static DEVICE_ATTR(color_enhance, S_IWUSR|S_IRUGO, color_enhancement_get, color_enhancement_set);
#endif

#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
static ssize_t image_enhance_get(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;

	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}
	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);

	return sprintf(buf, "%d\n", ctrl->ie_on);
}

static ssize_t image_enhance_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	ssize_t ret = strnlen(buf, PAGE_SIZE);

	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	if(pdata_base->panel_info.panel_power_state == 0){
		pr_err("%s: Panel off state. Ignore image enhancement cmd\n", __func__);
		return -EINVAL;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);

	sscanf(buf, "%d", &ctrl->ie_on);

	if(ctrl->ie_on == 1){
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->ie_on_cmds, CMD_REQ_COMMIT);
		pr_info("%s: set = %d, image enhance function on\n", __func__, ctrl->ie_on);
	}
	else if(ctrl->ie_on == 0){
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->ie_off_cmds, CMD_REQ_COMMIT);
		pr_info("%s: set = %d, image enhance funtion off\n", __func__, ctrl->ie_on);
	}
	else
		pr_info("%s: set = %d, wrong set value\n", __func__, ctrl->ie_on);

	return ret;
}

static ssize_t cabc_get(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", cabc_ctrl);
}

static ssize_t cabc_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	ssize_t ret = strnlen(buf, PAGE_SIZE);

	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	if(pdata_base->panel_info.panel_power_state == 0){
		pr_err("%s: Panel off state. Ignore cabc set cmd\n", __func__);
		return -EINVAL;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);

	sscanf(buf, "%d", &cabc_ctrl);

	if(cabc_ctrl == 20){
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->cabc_20_cmds, CMD_REQ_COMMIT);
		pr_info("%s: set = %d, cabc function on at 20%%\n", __func__, cabc_ctrl);
	}
	else if(cabc_ctrl == 30){
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->cabc_30_cmds, CMD_REQ_COMMIT);
		pr_info("%s: set = %d, cabc funtion on at 30%%\n", __func__, cabc_ctrl);
	}
	else
		pr_info("%s: set = %d, wrong set value\n", __func__, cabc_ctrl);

	return ret;
}

static DEVICE_ATTR(image_enhance_set, S_IWUSR|S_IRUGO, image_enhance_get, image_enhance_set);
static DEVICE_ATTR(cabc, S_IWUSR|S_IRUGO, cabc_get, cabc_set);
#endif

#if defined(CONFIG_LGE_LCD_TUNING)
int find_lcd_cmd(void)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	int i,j;
	char cmd[128]={0, };
	memset(read_cmd,0,128*sizeof(char));
	pr_info("reg_num=%x",reg_num);
	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);
	for(i=0;i<ctrl->on_cmds.cmd_cnt;i++)
	{
		pr_info("%s:cmd_cnt(find_lcd_cmd)[%d]=%x",__func__,i,ctrl->on_cmds.cmds[i].payload[0]);
		if(ctrl->on_cmds.cmds[i].payload[0]==reg_num)
		{
			for(j=0;j<ctrl->on_cmds.cmds[i].dchdr.dlen;j++)
			{
				cmd[j] = ctrl->on_cmds.cmds[i].payload[j];
			}
			memcpy(read_cmd,cmd,128*sizeof(char));
			cmd_num=ctrl->on_cmds.cmds[i].dchdr.dlen;
			return cmd_num;
		}
	}
	return 0;
}
void put_lcd_cmd(void)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	int i,j;
	pr_info("reg_num=%x",reg_num);
	if(pdata_base == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	ctrl =  container_of(pdata_base, struct mdss_dsi_ctrl_pdata,
			panel_data);
	for(i=0;i<ctrl->on_cmds.cmd_cnt;i++)
	{
		pr_info("%s:cmd_cnt[%d]=%x",__func__,i,ctrl->on_cmds.cmds[i].payload[0]);
		if(ctrl->on_cmds.cmds[i].payload[0]==reg_num)
		{
			for(j=1;j<ctrl->on_cmds.cmds[i].dchdr.dlen;j++)
			{
				ctrl->on_cmds.cmds[i].payload[j]=tun_lcd[j];
				pr_info("%s: cmds[%d].payload[%d]: %x",__func__,i,j,ctrl->on_cmds.cmds[i].payload[j]);
			}
			//mdss_dsi_panel_cmds_send(ctrl, &ctrl->on_cmds, CMD_REQ_COMMIT);
		}
	}
}

int get_backlight_map_size(int *bl_size)
{
	int ret = 0;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	if (pdata_base) {
		*bl_size = pdata_base->panel_info.blmap_size;
		pr_info("Current bl map size : %d\n", *bl_size);
	}
#else
	ret = -1;
#endif
	return ret;
}

int get_backlight_map(int *bl_map)
{
	int ret = 0;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	if (pdata_base) {
		memcpy(bl_map, pdata_base->panel_info.blmap, sizeof(int) * pdata_base->panel_info.blmap_size);
	}
#else
	ret = -1;
#endif
	return ret;
}
int set_backlight_map(int bl_size, int *bl_map)
{
	int ret = 0;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	if (pdata_base) {
		pdata_base->panel_info.blmap_size = bl_size;
		memcpy(pdata_base->panel_info.blmap, bl_map, sizeof(int) * bl_size);
		pr_info("BL map updated with size %d\n", bl_size);
	}
#else
	ret = -1;
#endif
	return ret;
}

#endif

static void mdss_dsi_panel_switch_mode(struct mdss_panel_data *pdata,
							int mode)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mipi_panel_info *mipi;
	struct dsi_panel_cmds *pcmds;
	u32 flags = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	mipi  = &pdata->panel_info.mipi;

	if (!mipi->dms_mode)
		return;

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	if (mipi->dms_mode != DYNAMIC_MODE_RESOLUTION_SWITCH_IMMEDIATE) {
		flags |= CMD_REQ_COMMIT;
		if (mode == SWITCH_TO_CMD_MODE)
			pcmds = &ctrl_pdata->video2cmd;
		else
			pcmds = &ctrl_pdata->cmd2video;
	} else if ((mipi->dms_mode ==
				DYNAMIC_MODE_RESOLUTION_SWITCH_IMMEDIATE)
			&& pdata->current_timing
			&& !list_empty(&pdata->timings_list)) {
		struct dsi_panel_timing *pt;

		pt = container_of(pdata->current_timing,
				struct dsi_panel_timing, timing);

		pr_debug("%s: sending switch commands\n", __func__);
		pcmds = &pt->switch_cmds;
		flags |= CMD_REQ_DMA_TPG;
	} else {
		pr_warn("%s: Invalid mode switch attempted\n", __func__);
		return;
	}

	mdss_dsi_panel_cmds_send(ctrl_pdata, pcmds, flags);
}

static void mdss_dsi_panel_bl_ctrl(struct mdss_panel_data *pdata,
							u32 bl_level)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_dsi_ctrl_pdata *sctrl = NULL;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	/*
	 * Some backlight controllers specify a minimum duty cycle
	 * for the backlight brightness. If the brightness is less
	 * than it, the controller can malfunction.
	 */

	if ((bl_level < pdata->panel_info.bl_min) && (bl_level != 0))
		bl_level = pdata->panel_info.bl_min;

	switch (ctrl_pdata->bklt_ctrl) {
	case BL_WLED:
		led_trigger_event(bl_led_trigger, bl_level);
		break;
	case BL_PWM:
		mdss_dsi_panel_bklt_pwm(ctrl_pdata, bl_level);
		break;
	case BL_DCS_CMD:
		if (!mdss_dsi_sync_wait_enable(ctrl_pdata)) {
			mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
			break;
		}
		/*
		 * DCS commands to update backlight are usually sent at
		 * the same time to both the controllers. However, if
		 * sync_wait is enabled, we need to ensure that the
		 * dcs commands are first sent to the non-trigger
		 * controller so that when the commands are triggered,
		 * both controllers receive it at the same time.
		 */
		sctrl = mdss_dsi_get_other_ctrl(ctrl_pdata);
		if (mdss_dsi_sync_wait_trigger(ctrl_pdata)) {
			if (sctrl)
				mdss_dsi_panel_bklt_dcs(sctrl, bl_level);
			mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
		} else {
			mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
			if (sctrl)
				mdss_dsi_panel_bklt_dcs(sctrl, bl_level);
		}
		break;
	default:
		pr_err("%s: Unknown bl_ctrl configuration\n",
			__func__);
		break;
	}
}

static int mdss_dsi_panel_on(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;
	struct dsi_panel_cmds *on_cmds;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	struct dsi_panel_cmds *vcom_cmds;
#endif

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pr_debug("%s: ndx=%d\n", __func__, ctrl->ndx);

	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			goto end;
	}

	on_cmds = &ctrl->on_cmds;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	vcom_cmds = &ctrl->vcom_cmds;
#endif
	if ((pinfo->mipi.dms_mode == DYNAMIC_MODE_SWITCH_IMMEDIATE) &&
			(pinfo->mipi.boot_mode != pinfo->mipi.mode))
		on_cmds = &ctrl->post_dms_on_cmds;

	pr_debug("%s: ndx=%d cmd_cnt=%d\n", __func__,
				ctrl->ndx, on_cmds->cmd_cnt);
#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
	switch (pinfo->aod_cmd_mode) {
	case AOD_CMD_ENABLE:
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->aod_cmds[AOD_PANEL_CMD_U3_TO_U2], CMD_REQ_COMMIT);
		goto notify;
	case AOD_CMD_DISABLE:
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->aod_cmds[AOD_PANEL_CMD_U2_TO_U3], CMD_REQ_COMMIT);
		goto notify;
	case ON_AND_AOD:
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
		if (lge_get_panel_maker_id() == LGD_LG4946 &&
				lge_get_panel_revision_id() == LGD_LG4946_REV0 &&
				vcom_cmds->cmd_cnt != 0) {
			pr_err("[Display] send LG4946 REV0 Vcom Setting \n");
			mdss_dsi_panel_cmds_send(ctrl, vcom_cmds, CMD_REQ_COMMIT);
		}
#endif
		if (on_cmds->cmd_cnt)
			mdss_dsi_panel_cmds_send(ctrl, on_cmds, CMD_REQ_COMMIT);

#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
		if (ctrl->ie_on == 0)
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->ie_off_cmds, CMD_REQ_COMMIT);
#endif
#if defined(CONFIG_LGE_ENHANCE_GALLERY_SHARPNESS)
		if (ctrl->sharpness_on_cmds.cmds[2].payload[3] == 0x23)
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->sharpness_on_cmds, CMD_REQ_COMMIT);
#endif
		goto end;
	case ON_CMD:
		break;
	case CMD_SKIP:
		goto notify;
	default:
		pr_err("[AOD] Unknown Mode : %d\n", pinfo->aod_cmd_mode);
		break;
	}
#endif
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	if (lge_get_panel_maker_id() == LGD_LG4946 &&
			lge_get_panel_revision_id() == LGD_LG4946_REV0 &&
			vcom_cmds->cmd_cnt != 0) {
		pr_err("[Display] send LG4946 REV0 Vcom Setting \n");
		mdss_dsi_panel_cmds_send(ctrl, vcom_cmds, CMD_REQ_COMMIT);
	}
#endif
	if (on_cmds->cmd_cnt)
		mdss_dsi_panel_cmds_send(ctrl, on_cmds, CMD_REQ_COMMIT);
	if (pinfo->compression_mode == COMPRESSION_DSC)
		mdss_dsi_panel_dsc_pps_send(ctrl, pinfo);

#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
	if (ctrl->ie_on == 0)
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->ie_off_cmds, CMD_REQ_COMMIT);
#endif
#if defined(CONFIG_LGE_ENHANCE_GALLERY_SHARPNESS)
	if (ctrl->sharpness_on_cmds.cmds[2].payload[3] == 0x23)
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->sharpness_on_cmds, CMD_REQ_COMMIT);
#endif

#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
notify:
	{
		int param;
		param = pinfo->aod_cur_mode;
		if(touch_notifier_call_chain(LCD_EVENT_LCD_MODE, (void *)&param))
			pr_err("[AOD] Failt to send notify to touch\n");
	}
#endif

end:
	pinfo->blank_state = MDSS_PANEL_BLANK_UNBLANK;
	pr_debug("%s:-\n", __func__);
	return 0;
}

static int mdss_dsi_post_panel_on(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;
	struct dsi_panel_cmds *cmds;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pr_debug("%s: ctrl=%p ndx=%d\n", __func__, ctrl, ctrl->ndx);

	pinfo = &pdata->panel_info;
	if (pinfo->dcs_cmd_by_left && ctrl->ndx != DSI_CTRL_LEFT)
			goto end;

	cmds = &ctrl->post_panel_on_cmds;
	if (cmds->cmd_cnt) {
		msleep(VSYNC_DELAY);	/* wait for a vsync passed */
		mdss_dsi_panel_cmds_send(ctrl, cmds, CMD_REQ_COMMIT);
	}

end:
	pr_debug("%s:-\n", __func__);
	return 0;
}

static int mdss_dsi_panel_off(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pr_debug("%s: ctrl=%p ndx=%d\n", __func__, ctrl, ctrl->ndx);

	if (pinfo->dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			goto end;
	}

#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
		switch (pinfo->aod_cmd_mode) {
		case AOD_CMD_ENABLE:
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->aod_cmds[AOD_PANEL_CMD_U3_TO_U2], CMD_REQ_COMMIT);
			goto notify;
		case AOD_CMD_DISABLE:
			mdss_dsi_panel_cmds_send(ctrl, &ctrl->aod_cmds[AOD_PANEL_CMD_U2_TO_U3], CMD_REQ_COMMIT);
			goto notify;
		case OFF_CMD:
			break;
		case CMD_SKIP:
			goto notify;
		default:
			pr_err("[AOD] Unknown Mode : %d\n", pinfo->aod_cmd_mode);
			break;
		}
#endif
	if (ctrl->off_cmds.cmd_cnt)
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->off_cmds, CMD_REQ_COMMIT);

#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
notify:
	{
		int param;
		param = pinfo->aod_cur_mode;
		if(touch_notifier_call_chain(LCD_EVENT_LCD_MODE, (void *)&param))
			pr_err("[AOD] Failt to send notify to touch\n");
	}
#endif

end:
	pinfo->blank_state = MDSS_PANEL_BLANK_BLANK;
	pr_debug("%s:-\n", __func__);
	return 0;
}

static int mdss_dsi_panel_low_power_config(struct mdss_panel_data *pdata,
	int enable)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	pr_debug("%s: ctrl=%p ndx=%d enable=%d\n", __func__, ctrl, ctrl->ndx,
		enable);

	/* Any panel specific low power commands/config */
	if (enable)
		pinfo->blank_state = MDSS_PANEL_BLANK_LOW_POWER;
	else
		pinfo->blank_state = MDSS_PANEL_BLANK_UNBLANK;

	pr_debug("%s:-\n", __func__);
	return 0;
}

static void mdss_dsi_parse_trigger(struct device_node *np, char *trigger,
		char *trigger_key)
{
	const char *data;

	*trigger = DSI_CMD_TRIGGER_SW;
	data = of_get_property(np, trigger_key, NULL);
	if (data) {
		if (!strcmp(data, "none"))
			*trigger = DSI_CMD_TRIGGER_NONE;
		else if (!strcmp(data, "trigger_te"))
			*trigger = DSI_CMD_TRIGGER_TE;
		else if (!strcmp(data, "trigger_sw_seof"))
			*trigger = DSI_CMD_TRIGGER_SW_SEOF;
		else if (!strcmp(data, "trigger_sw_te"))
			*trigger = DSI_CMD_TRIGGER_SW_TE;
	}
}

#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
int mdss_dsi_parse_dcs_cmds(struct device_node *np,
		struct dsi_panel_cmds *pcmds, char *cmd_key, char *link_key)
#else
static int mdss_dsi_parse_dcs_cmds(struct device_node *np,
		struct dsi_panel_cmds *pcmds, char *cmd_key, char *link_key)
#endif
{
	const char *data;
	int blen = 0, len;
	char *buf, *bp;
	struct dsi_ctrl_hdr *dchdr;
	int i, cnt;

	data = of_get_property(np, cmd_key, &blen);
	if (!data) {
		pr_err("%s: failed, key=%s\n", __func__, cmd_key);
		return -ENOMEM;
	}

	buf = kzalloc(sizeof(char) * blen, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, data, blen);

	/* scan dcs commands */
	bp = buf;
	len = blen;
	cnt = 0;
	while (len >= sizeof(*dchdr)) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		dchdr->dlen = ntohs(dchdr->dlen);
		if (dchdr->dlen > len) {
			pr_err("%s: dtsi cmd=%x error, len=%d",
				__func__, dchdr->dtype, dchdr->dlen);
			goto exit_free;
		}
		bp += sizeof(*dchdr);
		len -= sizeof(*dchdr);
		bp += dchdr->dlen;
		len -= dchdr->dlen;
		cnt++;
	}

	if (len != 0) {
		pr_err("%s: dcs_cmd=%x len=%d error!",
				__func__, buf[0], blen);
		goto exit_free;
	}

	pcmds->cmds = kzalloc(cnt * sizeof(struct dsi_cmd_desc),
						GFP_KERNEL);
	if (!pcmds->cmds)
		goto exit_free;

	pcmds->cmd_cnt = cnt;
	pcmds->buf = buf;
	pcmds->blen = blen;

	bp = buf;
	len = blen;
	for (i = 0; i < cnt; i++) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		len -= sizeof(*dchdr);
		bp += sizeof(*dchdr);
		pcmds->cmds[i].dchdr = *dchdr;
		pcmds->cmds[i].payload = bp;
		bp += dchdr->dlen;
		len -= dchdr->dlen;
	}

	/*Set default link state to LP Mode*/
	pcmds->link_state = DSI_LP_MODE;

	if (link_key) {
		data = of_get_property(np, link_key, NULL);
		if (data && !strcmp(data, "dsi_hs_mode"))
			pcmds->link_state = DSI_HS_MODE;
		else
			pcmds->link_state = DSI_LP_MODE;
	}

	pr_debug("%s: dcs_cmd=%x len=%d, cmd_cnt=%d link_state=%d\n", __func__,
		pcmds->buf[0], pcmds->blen, pcmds->cmd_cnt, pcmds->link_state);

	return 0;

exit_free:
	kfree(buf);
	return -ENOMEM;
}


int mdss_panel_get_dst_fmt(u32 bpp, char mipi_mode, u32 pixel_packing,
				char *dst_format)
{
	int rc = 0;
	switch (bpp) {
	case 3:
		*dst_format = DSI_CMD_DST_FORMAT_RGB111;
		break;
	case 8:
		*dst_format = DSI_CMD_DST_FORMAT_RGB332;
		break;
	case 12:
		*dst_format = DSI_CMD_DST_FORMAT_RGB444;
		break;
	case 16:
		switch (mipi_mode) {
		case DSI_VIDEO_MODE:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB565;
			break;
		case DSI_CMD_MODE:
			*dst_format = DSI_CMD_DST_FORMAT_RGB565;
			break;
		default:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB565;
			break;
		}
		break;
	case 18:
		switch (mipi_mode) {
		case DSI_VIDEO_MODE:
			if (pixel_packing == 0)
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666;
			else
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666_LOOSE;
			break;
		case DSI_CMD_MODE:
			*dst_format = DSI_CMD_DST_FORMAT_RGB666;
			break;
		default:
			if (pixel_packing == 0)
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666;
			else
				*dst_format = DSI_VIDEO_DST_FORMAT_RGB666_LOOSE;
			break;
		}
		break;
	case 24:
		switch (mipi_mode) {
		case DSI_VIDEO_MODE:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
			break;
		case DSI_CMD_MODE:
			*dst_format = DSI_CMD_DST_FORMAT_RGB888;
			break;
		default:
			*dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
			break;
		}
		break;
	default:
		rc = -EINVAL;
		break;
	}
	return rc;
}

static int mdss_dsi_parse_fbc_params(struct device_node *np,
			struct mdss_panel_timing *timing)
{
	int rc, fbc_enabled = 0;
	u32 tmp;
	struct fbc_panel_info *fbc = &timing->fbc;

	fbc_enabled = of_property_read_bool(np,	"qcom,mdss-dsi-fbc-enable");
	if (fbc_enabled) {
		pr_debug("%s:%d FBC panel enabled.\n", __func__, __LINE__);
		fbc->enabled = 1;
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-bpp", &tmp);
		fbc->target_bpp = (!rc ? tmp : 24);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-packing",
				&tmp);
		fbc->comp_mode = (!rc ? tmp : 0);
		fbc->qerr_enable = of_property_read_bool(np,
			"qcom,mdss-dsi-fbc-quant-error");
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-bias", &tmp);
		fbc->cd_bias = (!rc ? tmp : 0);
		fbc->pat_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-pat-mode");
		fbc->vlc_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-vlc-mode");
		fbc->bflc_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-bflc-mode");
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-h-line-budget",
				&tmp);
		fbc->line_x_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-budget-ctrl",
				&tmp);
		fbc->block_x_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-block-budget",
				&tmp);
		fbc->block_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossless-threshold", &tmp);
		fbc->lossless_mode_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossy-threshold", &tmp);
		fbc->lossy_mode_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-rgb-threshold",
				&tmp);
		fbc->lossy_rgb_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossy-mode-idx", &tmp);
		fbc->lossy_mode_idx = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-slice-height", &tmp);
		fbc->slice_height = (!rc ? tmp : 0);
		fbc->pred_mode = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-2d-pred-mode");
		fbc->enc_mode = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-ver2-mode");
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-max-pred-err", &tmp);
		fbc->max_pred_err = (!rc ? tmp : 0);

		timing->compression_mode = COMPRESSION_FBC;
	} else {
		pr_debug("%s:%d Panel does not support FBC.\n",
				__func__, __LINE__);
		fbc->enabled = 0;
		fbc->target_bpp = 24;
	}
	return 0;
}

static int mdss_dsc_to_buf(struct dsc_desc *dsc, char *buf,
			int pps_id, int major, int minor)
{
	char *bp;
	char data;
	int i, bpp;

	bp = buf;
	*bp++ = ((major << 4) | minor);		/* pps0 */
	*bp++ = pps_id;				/* pps1 */
	bp++;					/* pps2, reserved */

	data = dsc->line_buf_depth & 0x0f;
	data |= (dsc->bpc << 4);
	*bp++ = data;				 /* pps3 */

	bpp = dsc->bpp;
	bpp <<= 4;	/* 4 fraction bits */
	data = (bpp >> 8);
	data &= 0x03;		/* upper two bits */
	data |= (dsc->block_pred_enable << 5);
	data |= (dsc->convert_rgb << 4);
	data |= (dsc->enable_422 << 3);
	data |= (dsc->vbr_enable << 2);
	*bp++ = data;				/* pps4 */
	*bp++ = bpp;				/* pps5 */

	*bp++ = (dsc->pic_height >> 8);		/* pps6 */
	*bp++ = (dsc->pic_height & 0x0ff);	/* pps7 */
	*bp++ = (dsc->pic_width >> 8);		/* pps8 */
	*bp++ = (dsc->pic_width & 0x0ff);	/* pps9 */

	*bp++ = (dsc->slice_height >> 8);	/* pps10 */
	*bp++ = (dsc->slice_height & 0x0ff);	/* pps11 */
	*bp++ = (dsc->slice_width >> 8);	/* pps12 */
	*bp++ = (dsc->slice_width & 0x0ff);	/* pps13 */

	*bp++ = (dsc->chunk_size >> 8);		/* pps14 */
	*bp++ = (dsc->chunk_size & 0x0ff);	/* pps15 */

	data = dsc->initial_xmit_delay >> 8;
	data &= 0x03;
	*bp++ = data;				/* pps16, bit 0, 1 */
	*bp++ = dsc->initial_xmit_delay;	/* pps17 */

	*bp++ = (dsc->initial_dec_delay >> 8);	/* pps18 */
	*bp++ = dsc->initial_dec_delay;		/* pps19 */

	bp++;					/* pps20, reserved */

	*bp++ = (dsc->initial_scale_value & 0x3f); /* pps21 */

	data = (dsc->scale_increment_interval >> 8);
	data &= 0x0f;
	*bp++ =  data;				/* pps22 */
	*bp++ = dsc->scale_increment_interval;	/* pps23 */

	data = (dsc->scale_decrement_interval >> 8);
	data &= 0x0f;
	*bp++ = data;				/* pps24 */
	*bp++ = (dsc->scale_decrement_interval & 0x0ff);/* pps25 */

	bp++;			/* pps26, reserved */

	*bp++ = (dsc->first_line_bpg_offset & 0x1f);/* pps27 */

	*bp++ = (dsc->nfl_bpg_offset >> 8);	/* pps28 */
	*bp++ = (dsc->nfl_bpg_offset & 0x0ff);	/* pps29 */
	*bp++ = (dsc->slice_bpg_offset >> 8);	/* pps30 */
	*bp++ = (dsc->slice_bpg_offset & 0x0ff);/* pps31 */

	*bp++ = (dsc->initial_offset >> 8);	/* pps32 */
	*bp++ = (dsc->initial_offset & 0x0ff);	/* pps33 */

	*bp++ = (dsc->final_offset >> 8);	/* pps34 */
	*bp++ = (dsc->final_offset & 0x0ff);	/* pps35 */

	*bp++ = (dsc->min_qp_flatness & 0x1f);	/* pps36 */
	*bp++ = (dsc->max_qp_flatness & 0x1f);	/* pps37 */

	*bp++ = (dsc->rc_model_size >> 8);	/* pps38 */
	*bp++ = (dsc->rc_model_size & 0x0ff);	/* pps39 */

	*bp++ = (dsc->edge_factor & 0x0f);	/* pps40 */

	*bp++ = (dsc->quant_incr_limit0 & 0x1f);	/* pps41 */
	*bp++ = (dsc->quant_incr_limit1 & 0x1f);	/* pps42 */

	data = (dsc->tgt_offset_hi << 4);
	data |= (dsc->tgt_offset_lo & 0x0f);
	*bp++ = data;				/* pps43 */

	for (i = 0; i < 14; i++)
		*bp++ = dsc->buf_thresh[i];	/* pps44 - pps57 */

	for (i = 0; i < 15; i++) {		/* pps58 - pps87 */
		data = (dsc->range_min_qp[i] & 0x1f); /* 5 bits */
		data <<= 3;
		data |= ((dsc->range_max_qp[i] >> 2) & 0x07); /* 3 bits */
		*bp++ = data;
		data = (dsc->range_max_qp[i] & 0x03); /* 2 bits */
		data <<= 6;
		data |= (dsc->range_bpg_offset[i] & 0x3f); /* 6 bits */
		*bp++ = data;
	}

	/* pps88 to pps127 are reserved */

	return DSC_PPS_LEN;	/* 128 */
}

void mdss_dsi_panel_dsc_pps_send(struct mdss_dsi_ctrl_pdata *ctrl,
				struct mdss_panel_info *pinfo)
{
	struct dsc_desc *dsc;
	struct dsi_panel_cmds pcmds;
	struct dsi_cmd_desc cmd;

	if (!pinfo || (pinfo->compression_mode != COMPRESSION_DSC))
		return;

	memset(&pcmds, 0, sizeof(pcmds));
	memset(&cmd, 0, sizeof(cmd));

	dsc = &pinfo->dsc;
	cmd.dchdr.dlen = mdss_dsc_to_buf(dsc, ctrl->pps_buf, 0 , 1, 0);
	cmd.dchdr.dtype = DTYPE_PPS;
	cmd.dchdr.last = 1;
	cmd.dchdr.wait = 10;
	cmd.dchdr.vc = 0;
	cmd.dchdr.ack = 0;
	cmd.payload = ctrl->pps_buf;

	pcmds.cmd_cnt = 1;
	pcmds.cmds = &cmd;
	pcmds.link_state = DSI_LP_MODE;

	mdss_dsi_panel_cmds_send(ctrl, &pcmds, CMD_REQ_COMMIT);
}

int mdss_dsc_initial_line_calc(int bpc, int xmit_delay,
			int slice_width, int slice_per_line)
{
	int ssm_delay;
	int total_pixels;

	ssm_delay = ((bpc < 10) ? 83 : 91);
	total_pixels = ssm_delay * 3 + xmit_delay + 47;
	total_pixels += ((slice_per_line > 1) ? (ssm_delay * 3) : 0);

	return CEIL(total_pixels, slice_width);
}

void mdss_dsc_parameters_calc(struct dsc_desc *dsc, int width, int height)
{
	int bpp, bpc;
	int mux_words_size;
	int groups_per_line, groups_total;
	int min_rate_buffer_size;
	int hrd_delay;
	int pre_num_extra_mux_bits, num_extra_mux_bits;
	int slice_bits;
	int target_bpp_x16;
	int data;
	int final_value, final_scale;
	int slice_per_line, bytes_in_slice, total_bytes;

	if (!dsc || !width || !height)
		return;

	dsc->pic_width = width;
	dsc->pic_height = height;

	if ((dsc->pic_width % dsc->slice_width) ||
	    (dsc->pic_height % dsc->slice_height)) {
		pr_err("Error: pic_dim=%dx%d has to be multiple of slice_dim=%dx%d\n",
			dsc->pic_width, dsc->pic_height,
			dsc->slice_width, dsc->slice_height);
		return;
	}

	dsc->rc_model_size = 8192;	/* rate_buffer_size */
	dsc->first_line_bpg_offset = 12;
	dsc->min_qp_flatness = 3;
	dsc->max_qp_flatness = 12;
	dsc->line_buf_depth = 9;

	dsc->edge_factor = 6;
	dsc->quant_incr_limit0 = 11;
	dsc->quant_incr_limit1 = 11;
	dsc->tgt_offset_hi = 3;
	dsc->tgt_offset_lo = 3;

	dsc->buf_thresh = dsc_rc_buf_thresh;
	dsc->range_min_qp = dsc_rc_range_min_qp;
	dsc->range_max_qp = dsc_rc_range_max_qp;
	dsc->range_bpg_offset = dsc_rc_range_bpg_offset;

	bpp = dsc->bpp;
	bpc = dsc->bpc;

	if (bpp == 8)
		dsc->initial_offset = 6144;
	else
		dsc->initial_offset = 2048;	/* bpp = 12 */

	if (bpc == 8)
		mux_words_size = 48;
	else
		mux_words_size = 64;	/* bpc == 12 */

	slice_per_line = CEIL(dsc->pic_width, dsc->slice_width);

	dsc->pkt_per_line = slice_per_line / dsc->slice_per_pkt;
	if (slice_per_line % dsc->slice_per_pkt)
		dsc->pkt_per_line = 1;		/* default*/

	bytes_in_slice = CEIL(dsc->pic_width, slice_per_line);

	bytes_in_slice *= dsc->bpp;	/* bites per compressed pixel */
	bytes_in_slice = CEIL(bytes_in_slice, 8);

	dsc->bytes_in_slice = bytes_in_slice;

	pr_debug("%s: slice_per_line=%d pkt_per_line=%d bytes_in_slice=%d\n",
		__func__, slice_per_line, dsc->pkt_per_line, bytes_in_slice);

	total_bytes = bytes_in_slice * slice_per_line;
	dsc->eol_byte_num = total_bytes % 3;
	dsc->pclk_per_line =  CEIL(total_bytes, 3);

	dsc->slice_last_group_size = 3 - dsc->eol_byte_num;

	pr_debug("%s: pclk_per_line=%d total_bytes=%d eol_byte_num=%d\n",
		__func__, dsc->pclk_per_line, total_bytes, dsc->eol_byte_num);

	dsc->bytes_per_pkt = bytes_in_slice * dsc->slice_per_pkt;

	dsc->det_thresh_flatness = 7 + 2*(bpc - 8);

	dsc->initial_xmit_delay = dsc->rc_model_size / (2 * bpp);

	dsc->initial_lines = mdss_dsc_initial_line_calc(bpc,
		dsc->initial_xmit_delay, dsc->slice_width, slice_per_line);

	groups_per_line = CEIL(dsc->slice_width, 3);

	dsc->chunk_size = dsc->slice_width * bpp / 8;
	if ((dsc->slice_width * bpp) % 8)
		dsc->chunk_size++;

	/* rbs-min */
	min_rate_buffer_size =  dsc->rc_model_size - dsc->initial_offset +
			dsc->initial_xmit_delay * bpp +
			groups_per_line * dsc->first_line_bpg_offset;

	hrd_delay = CEIL(min_rate_buffer_size, bpp);

	dsc->initial_dec_delay = hrd_delay - dsc->initial_xmit_delay;

	dsc->initial_scale_value = 8 * dsc->rc_model_size /
			(dsc->rc_model_size - dsc->initial_offset);

	slice_bits = 8 * dsc->chunk_size * dsc->slice_height;

	groups_total = groups_per_line * dsc->slice_height;

	data = dsc->first_line_bpg_offset * 2048;

	dsc->nfl_bpg_offset = CEIL(data, (dsc->slice_height - 1));

	pre_num_extra_mux_bits = 3 * (mux_words_size + (4 * bpc + 4) - 2);

	num_extra_mux_bits = pre_num_extra_mux_bits - (mux_words_size -
		((slice_bits - pre_num_extra_mux_bits) % mux_words_size));

	data = 2048 * (dsc->rc_model_size - dsc->initial_offset
		+ num_extra_mux_bits);
	dsc->slice_bpg_offset = CEIL(data, groups_total);

	/* bpp * 16 + 0.5 */
	data = bpp * 16;
	data *= 2;
	data++;
	data /= 2;
	target_bpp_x16 = data;

	data = (dsc->initial_xmit_delay * target_bpp_x16) / 16;
	final_value =  dsc->rc_model_size - data + num_extra_mux_bits;

	final_scale = 8 * dsc->rc_model_size /
		(dsc->rc_model_size - final_value);

	dsc->final_offset = final_value;

	data = (final_scale - 9) * (dsc->nfl_bpg_offset +
		dsc->slice_bpg_offset);
	dsc->scale_increment_interval = (2048 * dsc->final_offset) / data;

	dsc->scale_decrement_interval = groups_per_line /
		(dsc->initial_scale_value - 8);

	pr_debug("%s: initial_xmit_delay=%d\n", __func__,
		dsc->initial_xmit_delay);

	pr_debug("%s: bpg_offset, nfl=%d slice=%d\n", __func__,
		dsc->nfl_bpg_offset, dsc->slice_bpg_offset);

	pr_debug("%s: groups_per_line=%d chunk_size=%d\n", __func__,
		groups_per_line, dsc->chunk_size);
	pr_debug("%s:min_rate_buffer_size=%d hrd_delay=%d\n", __func__,
		min_rate_buffer_size, hrd_delay);
	pr_debug("%s:initial_dec_delay=%d initial_scale_value=%d\n", __func__,
		dsc->initial_dec_delay, dsc->initial_scale_value);
	pr_debug("%s:slice_bits=%d, groups_total=%d\n", __func__,
		slice_bits, groups_total);
	pr_debug("%s: first_line_bgp_offset=%d slice_height=%d\n", __func__,
		dsc->first_line_bpg_offset, dsc->slice_height);
	pr_debug("%s:final_value=%d final_scale=%d\n", __func__,
		final_value, final_scale);
	pr_debug("%s: sacle_increment_interval=%d scale_decrement_interval=%d\n",
		__func__, dsc->scale_increment_interval,
		dsc->scale_decrement_interval);
}

static int mdss_dsi_parse_dsc_params(struct device_node *np,
		struct mdss_panel_timing *timing, bool is_split_display)
{
	u32 data;
	int rc = 0;
	struct dsc_desc *dsc = &timing->dsc;

	if (!np) {
		pr_err("%s: device node pointer is NULL\n", __func__);
		return -EINVAL;
	}

	rc = of_property_read_u32(np, "qcom,mdss-dsc-encoders", &data);
	if (rc) {
		if (!of_find_property(np, "qcom,mdss-dsc-encoders", NULL)) {
			/* property is not defined, default to 1 */
			data = 1;
		} else {
			pr_err("%s: Error parsing qcom,mdss-dsc-encoders\n",
				__func__);
			goto end;
		}
	}

	timing->dsc_enc_total = data;

	if (is_split_display && (timing->dsc_enc_total > 1)) {
		pr_err("%s: Error: for split displays, more than 1 dsc encoder per panel is not allowed.\n",
			__func__);
		goto end;
	}

	rc = of_property_read_u32(np, "qcom,mdss-dsc-slice-height", &data);
	if (rc)
		goto end;
	dsc->slice_height = data;

	rc = of_property_read_u32(np, "qcom,mdss-dsc-slice-width", &data);
	if (rc)
		goto end;
	dsc->slice_width = data;

	if (timing->xres % dsc->slice_width) {
		pr_err("%s: Error: multiple of slice-width:%d should match panel-width:%d\n",
			__func__, dsc->slice_width, timing->xres);
		goto end;
	}

	data = timing->xres / dsc->slice_width;
	if (((timing->dsc_enc_total > 1) && ((data != 2) && (data != 4))) ||
	    ((timing->dsc_enc_total == 1) && (data > 2))) {
		pr_err("%s: Error: max 2 slice per encoder. slice-width:%d should match panel-width:%d dsc_enc_total:%d\n",
			__func__, dsc->slice_width,
			timing->xres, timing->dsc_enc_total);
		goto end;
	}

	rc = of_property_read_u32(np, "qcom,mdss-dsc-slice-per-pkt", &data);
	if (rc)
		goto end;
	dsc->slice_per_pkt = data;

	pr_debug("%s: num_enc:%d :slice h=%d w=%d s_pkt=%d\n", __func__,
		timing->dsc_enc_total, dsc->slice_height,
		dsc->slice_width, dsc->slice_per_pkt);

	rc = of_property_read_u32(np, "qcom,mdss-dsc-bit-per-component", &data);
	if (rc)
		goto end;
	dsc->bpc = data;

	rc = of_property_read_u32(np, "qcom,mdss-dsc-bit-per-pixel", &data);
	if (rc)
		goto end;
	dsc->bpp = data;

	pr_debug("%s: bpc=%d bpp=%d\n", __func__,
		dsc->bpc, dsc->bpp);

	dsc->block_pred_enable = of_property_read_bool(np,
			"qcom,mdss-dsc-block-prediction-enable");

	dsc->enable_422 = 0;
	dsc->convert_rgb = 1;
	dsc->vbr_enable = 0;

	dsc->config_by_manufacture_cmd = of_property_read_bool(np,
		"qcom,mdss-dsc-config-by-manufacture-cmd");

	mdss_dsc_parameters_calc(&timing->dsc, timing->xres, timing->yres);

	timing->dsc.full_frame_slices =
		CEIL(timing->dsc.pic_width, timing->dsc.slice_width);

	timing->compression_mode = COMPRESSION_DSC;

end:
	return rc;
}

static int mdss_dsi_parse_topology_config(struct device_node *np,
	struct dsi_panel_timing *pt, struct mdss_panel_data *panel_data)
{
	int rc = 0;
	bool is_split_display = panel_data->panel_info.is_split_display;
	const char *data;
	struct mdss_panel_timing *timing = &pt->timing;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_panel_info *pinfo;
	struct device_node *cfg_np;

	ctrl_pdata = container_of(panel_data, struct mdss_dsi_ctrl_pdata,
							panel_data);
	cfg_np = ctrl_pdata->panel_data.cfg_np;
	pinfo = &ctrl_pdata->panel_data.panel_info;

	if (!cfg_np && of_find_property(np, "qcom,config-select", NULL)) {
		cfg_np = of_parse_phandle(np, "qcom,config-select", 0);
		if (!cfg_np)
			pr_err("%s:err parsing qcom,config-select\n", __func__);
		ctrl_pdata->panel_data.cfg_np = cfg_np;
	}

	if (cfg_np) {
		if (!of_property_read_u32_array(cfg_np, "qcom,lm-split",
		    timing->lm_widths, 2)) {
			if (mdss_dsi_is_hw_config_split(ctrl_pdata->shared_data)
			    && (timing->lm_widths[1] != 0)) {
				pr_err("%s: lm-split not allowed with split display\n",
					__func__);
				rc = -EINVAL;
				goto end;
			}
		}
		rc = of_property_read_string(cfg_np, "qcom,split-mode", &data);
		if (!rc && !strcmp(data, "pingpong-split"))
			pinfo->use_pingpong_split = true;

		if (((timing->lm_widths[0]) || (timing->lm_widths[1])) &&
		    pinfo->use_pingpong_split) {
			pr_err("%s: pingpong_split cannot be used when lm-split[%d,%d] is specified\n",
				__func__,
				timing->lm_widths[0], timing->lm_widths[1]);
			return -EINVAL;
		}

		pr_info("%s: cfg_node name %s lm_split:%dx%d pp_split:%s\n",
			__func__, cfg_np->name,
			timing->lm_widths[0], timing->lm_widths[1],
			pinfo->use_pingpong_split ? "yes" : "no");
	}

	if (!pinfo->use_pingpong_split &&
	    (timing->lm_widths[0] == 0) && (timing->lm_widths[1] == 0))
		timing->lm_widths[0] = pt->timing.xres;

	data = of_get_property(np, "qcom,compression-mode", NULL);
	if (data) {
		if (cfg_np && !strcmp(data, "dsc"))
			rc = mdss_dsi_parse_dsc_params(cfg_np, &pt->timing,
					is_split_display);
		else if (!strcmp(data, "fbc"))
			rc = mdss_dsi_parse_fbc_params(np, &pt->timing);
	}

end:
	of_node_put(cfg_np);
	return rc;
}

static void mdss_panel_parse_te_params(struct device_node *np,
		struct mdss_panel_timing *timing)
{
	struct mdss_mdp_pp_tear_check *te = &timing->te;
	u32 tmp;
	int rc = 0;
	/*
	 * TE default: dsi byte clock calculated base on 70 fps;
	 * around 14 ms to complete a kickoff cycle if te disabled;
	 * vclk_line base on 60 fps; write is faster than read;
	 * init == start == rdptr;
	 */
	te->tear_check_en =
		!of_property_read_bool(np, "qcom,mdss-tear-check-disable");
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-cfg-height", &tmp);
	te->sync_cfg_height = (!rc ? tmp : 0xfff0);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-init-val", &tmp);
	te->vsync_init_val = (!rc ? tmp : timing->yres);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-threshold-start", &tmp);
	te->sync_threshold_start = (!rc ? tmp : 4);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-threshold-continue", &tmp);
	te->sync_threshold_continue = (!rc ? tmp : 4);
	rc = of_property_read_u32(np, "qcom,mdss-tear-check-frame-rate", &tmp);
	te->refx100 = (!rc ? tmp : 6000);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-start-pos", &tmp);
	te->start_pos = (!rc ? tmp : timing->yres);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-rd-ptr-trigger-intr", &tmp);
	te->rd_ptr_irq = (!rc ? tmp : timing->yres + 1);
}


static int mdss_dsi_parse_reset_seq(struct device_node *np,
		u32 rst_seq[MDSS_DSI_RST_SEQ_LEN], u32 *rst_len,
		const char *name)
{
	int num = 0, i;
	int rc;
	struct property *data;
	u32 tmp[MDSS_DSI_RST_SEQ_LEN];
	*rst_len = 0;
	data = of_find_property(np, name, &num);
	num /= sizeof(u32);
	if (!data || !num || num > MDSS_DSI_RST_SEQ_LEN || num % 2) {
		pr_debug("%s:%d, error reading %s, length found = %d\n",
			__func__, __LINE__, name, num);
	} else {
		rc = of_property_read_u32_array(np, name, tmp, num);
		if (rc)
			pr_debug("%s:%d, error reading %s, rc = %d\n",
				__func__, __LINE__, name, rc);
		else {
			for (i = 0; i < num; ++i)
				rst_seq[i] = tmp[i];
			*rst_len = num;
		}
	}
	return 0;
}

static int mdss_dsi_gen_read_status(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	if (!mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
		ctrl_pdata->status_value, 0)) {
		pr_err("%s: Read back value from panel is incorrect\n",
							__func__);
		return -EINVAL;
	} else {
		return 1;
	}
}

static int mdss_dsi_nt35596_read_status(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	if (!mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
		ctrl_pdata->status_value, 0)) {
		ctrl_pdata->status_error_count = 0;
		pr_err("%s: Read back value from panel is incorrect\n",
							__func__);
		return -EINVAL;
	} else {
		if (!mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
			ctrl_pdata->status_value, 3)) {
			ctrl_pdata->status_error_count = 0;
		} else {
			if (mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
				ctrl_pdata->status_value, 4) ||
				mdss_dsi_cmp_panel_reg(ctrl_pdata->status_buf,
				ctrl_pdata->status_value, 5))
				ctrl_pdata->status_error_count = 0;
			else
				ctrl_pdata->status_error_count++;
			if (ctrl_pdata->status_error_count >=
					ctrl_pdata->max_status_error_count) {
				ctrl_pdata->status_error_count = 0;
				pr_err("%s: Read value bad. Error_cnt = %i\n",
					 __func__,
					ctrl_pdata->status_error_count);
				return -EINVAL;
			}
		}
		return 1;
	}
}

static void mdss_dsi_parse_roi_alignment(struct device_node *np,
		struct mdss_panel_info *pinfo)
{
	int len = 0;
	u32 value[6];
	struct property *data;
	data = of_find_property(np, "qcom,panel-roi-alignment", &len);
	len /= sizeof(u32);
	if (!data || (len != 6)) {
		pr_debug("%s: Panel roi alignment not found", __func__);
	} else {
		int rc = of_property_read_u32_array(np,
				"qcom,panel-roi-alignment", value, len);
		if (rc)
			pr_debug("%s: Error reading panel roi alignment values",
					__func__);
		else {
			pinfo->xstart_pix_align = value[0];
			pinfo->ystart_pix_align = value[1];
			pinfo->width_pix_align = value[2];
			pinfo->height_pix_align = value[3];
			pinfo->min_width = value[4];
			pinfo->min_height = value[5];
		}

		pr_debug("%s: ROI alignment: [%d, %d, %d, %d, %d, %d]",
				__func__, pinfo->xstart_pix_align,
				pinfo->width_pix_align, pinfo->ystart_pix_align,
				pinfo->height_pix_align, pinfo->min_width,
				pinfo->min_height);
	}
}

static void mdss_dsi_parse_dms_config(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct mdss_panel_info *pinfo = &ctrl->panel_data.panel_info;
	const char *data;
	bool dms_enabled;

	dms_enabled = of_property_read_bool(np,
		"qcom,dynamic-mode-switch-enabled");

	if (!dms_enabled) {
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_DISABLED;
		goto exit;
	}

	/* default mode is suspend_resume */
	pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_SUSPEND_RESUME;
	data = of_get_property(np, "qcom,dynamic-mode-switch-type", NULL);
	if (data && !strcmp(data, "dynamic-resolution-switch-immediate")) {
		if (!list_empty(&ctrl->panel_data.timings_list))
			pinfo->mipi.dms_mode =
				DYNAMIC_MODE_RESOLUTION_SWITCH_IMMEDIATE;
		else
			pinfo->mipi.dms_mode =
				DYNAMIC_MODE_SWITCH_DISABLED;
		goto exit;
	}

	if (data && !strcmp(data, "dynamic-switch-immediate"))
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_IMMEDIATE;
	else
		pr_debug("%s: default dms suspend/resume\n", __func__);

	mdss_dsi_parse_dcs_cmds(np, &ctrl->video2cmd,
		"qcom,video-to-cmd-mode-switch-commands", NULL);

	mdss_dsi_parse_dcs_cmds(np, &ctrl->cmd2video,
		"qcom,cmd-to-video-mode-switch-commands", NULL);

	mdss_dsi_parse_dcs_cmds(np, &ctrl->post_dms_on_cmds,
		"qcom,mdss-dsi-post-mode-switch-on-command",
		"qcom,mdss-dsi-post-mode-switch-on-command-state");

	if (pinfo->mipi.dms_mode == DYNAMIC_MODE_SWITCH_IMMEDIATE &&
		!ctrl->post_dms_on_cmds.cmd_cnt) {
		pr_warn("%s: No post dms on cmd specified\n", __func__);
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_DISABLED;
	}

	if (!ctrl->video2cmd.cmd_cnt || !ctrl->cmd2video.cmd_cnt) {
		pr_warn("%s: No commands specified for dynamic switch\n",
			__func__);
		pinfo->mipi.dms_mode = DYNAMIC_MODE_SWITCH_DISABLED;
	}
exit:
	pr_info("%s: dynamic switch feature enabled: %d\n", __func__,
		pinfo->mipi.dms_mode);
	return;
}

static void mdss_dsi_parse_esd_params(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	u32 tmp;
	int rc;
	struct property *data;
	const char *string;
	struct mdss_panel_info *pinfo = &ctrl->panel_data.panel_info;

	pinfo->esd_check_enabled = of_property_read_bool(np,
		"qcom,esd-check-enabled");

	if (!pinfo->esd_check_enabled)
		return;

	mdss_dsi_parse_dcs_cmds(np, &ctrl->status_cmds,
			"qcom,mdss-dsi-panel-status-command",
				"qcom,mdss-dsi-panel-status-command-state");

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-status-read-length",
		&tmp);
	ctrl->status_cmds_rlen = (!rc ? tmp : 1);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-max-error-count",
		&tmp);
	ctrl->max_status_error_count = (!rc ? tmp : 0);

	ctrl->status_value = kzalloc(sizeof(u32) * ctrl->status_cmds_rlen,
				GFP_KERNEL);
	if (!ctrl->status_value) {
		pr_err("%s: Error allocating memory for status buffer\n",
			__func__);
		pinfo->esd_check_enabled = false;
		return;
	}

	data = of_find_property(np, "qcom,mdss-dsi-panel-status-value", &tmp);
	tmp /= sizeof(u32);
	if (!data || (tmp != ctrl->status_cmds_rlen)) {
		pr_debug("%s: Panel status values not found\n", __func__);
		memset(ctrl->status_value, 0, ctrl->status_cmds_rlen);
	} else {
		rc = of_property_read_u32_array(np,
			"qcom,mdss-dsi-panel-status-value",
			ctrl->status_value, tmp);
		if (rc) {
			pr_debug("%s: Error reading panel status values\n",
					__func__);
			memset(ctrl->status_value, 0, ctrl->status_cmds_rlen);
		}
	}

	ctrl->status_mode = ESD_MAX;
	rc = of_property_read_string(np,
			"qcom,mdss-dsi-panel-status-check-mode", &string);
	if (!rc) {
		if (!strcmp(string, "bta_check")) {
			ctrl->status_mode = ESD_BTA;
		} else if (!strcmp(string, "reg_read")) {
			ctrl->status_mode = ESD_REG;
			ctrl->check_read_status =
				mdss_dsi_gen_read_status;
		} else if (!strcmp(string, "reg_read_nt35596")) {
			ctrl->status_mode = ESD_REG_NT35596;
			ctrl->status_error_count = 0;
			ctrl->check_read_status =
				mdss_dsi_nt35596_read_status;
		} else if (!strcmp(string, "te_signal_check")) {
			if (pinfo->mipi.mode == DSI_CMD_MODE) {
				ctrl->status_mode = ESD_TE;
			} else {
				pr_err("TE-ESD not valid for video mode\n");
				goto error;
			}
		} else {
			pr_err("No valid panel-status-check-mode string\n");
			goto error;
		}
	}
	return;

error:
	kfree(ctrl->status_value);
	pinfo->esd_check_enabled = false;
}

static int mdss_dsi_parse_panel_features(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct mdss_panel_info *pinfo;

	if (!np || !ctrl) {
		pr_err("%s: Invalid arguments\n", __func__);
		return -ENODEV;
	}

	pinfo = &ctrl->panel_data.panel_info;

	pinfo->partial_update_supported = of_property_read_bool(np,
		"qcom,partial-update-enabled");
	if (pinfo->mipi.mode == DSI_CMD_MODE) {
		pinfo->partial_update_enabled = pinfo->partial_update_supported;
		pr_info("%s: partial_update_enabled=%d\n", __func__,
					pinfo->partial_update_enabled);
		ctrl->set_col_page_addr = mdss_dsi_set_col_page_addr;
		if (pinfo->partial_update_enabled) {
			pinfo->partial_update_roi_merge =
					of_property_read_bool(np,
					"qcom,partial-update-roi-merge");
		}

		pinfo->dcs_cmd_by_left = of_property_read_bool(np,
				"qcom,dcs-cmd-by-left");
	}

	pinfo->ulps_feature_enabled = of_property_read_bool(np,
		"qcom,ulps-enabled");
	pr_info("%s: ulps feature %s\n", __func__,
		(pinfo->ulps_feature_enabled ? "enabled" : "disabled"));

	pinfo->ulps_suspend_enabled = of_property_read_bool(np,
		"qcom,suspend-ulps-enabled");
	pr_info("%s: ulps during suspend feature %s", __func__,
		(pinfo->ulps_suspend_enabled ? "enabled" : "disabled"));

	mdss_dsi_parse_dms_config(np, ctrl);

	pinfo->panel_ack_disabled = pinfo->sim_panel_mode ?
		1 : of_property_read_bool(np, "qcom,panel-ack-disabled");

	mdss_dsi_parse_esd_params(np, ctrl);

	if (pinfo->panel_ack_disabled && pinfo->esd_check_enabled) {
		pr_warn("ESD should not be enabled if panel ACK is disabled\n");
		pinfo->esd_check_enabled = false;
	}

	if (ctrl->disp_en_gpio <= 0) {
		ctrl->disp_en_gpio = of_get_named_gpio(
			np,
			"qcom,5v-boost-gpio", 0);

		if (!gpio_is_valid(ctrl->disp_en_gpio))
			pr_debug("%s:%d, Disp_en gpio not specified\n",
					__func__, __LINE__);
	}

	return 0;
}

static void mdss_dsi_parse_panel_horizintal_line_idle(struct device_node *np,
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	const u32 *src;
	int i, len, cnt;
	struct panel_horizontal_idle *kp;

	if (!np || !ctrl) {
		pr_err("%s: Invalid arguments\n", __func__);
		return;
	}

	src = of_get_property(np, "qcom,mdss-dsi-hor-line-idle", &len);
	if (!src || len == 0)
		return;

	cnt = len % 3; /* 3 fields per entry */
	if (cnt) {
		pr_err("%s: invalid horizontal idle len=%d\n", __func__, len);
		return;
	}

	cnt = len / sizeof(u32);

	kp = kzalloc(sizeof(*kp) * (cnt / 3), GFP_KERNEL);
	if (kp == NULL) {
		pr_err("%s: No memory\n", __func__);
		return;
	}

	ctrl->line_idle = kp;
	for (i = 0; i < cnt; i += 3) {
		kp->min = be32_to_cpu(src[i]);
		kp->max = be32_to_cpu(src[i+1]);
		kp->idle = be32_to_cpu(src[i+2]);
		kp++;
		ctrl->horizontal_idle_cnt++;
	}

	pr_debug("%s: horizontal_idle_cnt=%d\n", __func__,
				ctrl->horizontal_idle_cnt);
}

static int mdss_dsi_set_refresh_rate_range(struct device_node *pan_node,
		struct mdss_panel_info *pinfo)
{
	int rc = 0;
	rc = of_property_read_u32(pan_node,
			"qcom,mdss-dsi-min-refresh-rate",
			&pinfo->min_fps);
	if (rc) {
		pr_warn("%s:%d, Unable to read min refresh rate\n",
				__func__, __LINE__);

		/*
		 * Since min refresh rate is not specified when dynamic
		 * fps is enabled, using minimum as 30
		 */
		pinfo->min_fps = MIN_REFRESH_RATE;
		rc = 0;
	}

	rc = of_property_read_u32(pan_node,
			"qcom,mdss-dsi-max-refresh-rate",
			&pinfo->max_fps);
	if (rc) {
		pr_warn("%s:%d, Unable to read max refresh rate\n",
				__func__, __LINE__);

		/*
		 * Since max refresh rate was not specified when dynamic
		 * fps is enabled, using the default panel refresh rate
		 * as max refresh rate supported.
		 */
		pinfo->max_fps = pinfo->mipi.frame_rate;
		rc = 0;
	}

	pr_info("dyn_fps: min = %d, max = %d\n",
			pinfo->min_fps, pinfo->max_fps);
	return rc;
}

static void mdss_dsi_parse_dfps_config(struct device_node *pan_node,
			struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	const char *data;
	bool dynamic_fps;
	struct mdss_panel_info *pinfo = &(ctrl_pdata->panel_data.panel_info);

	dynamic_fps = of_property_read_bool(pan_node,
			"qcom,mdss-dsi-pan-enable-dynamic-fps");

	if (!dynamic_fps)
		return;

	pinfo->dynamic_fps = true;
	data = of_get_property(pan_node, "qcom,mdss-dsi-pan-fps-update", NULL);
	if (data) {
		if (!strcmp(data, "dfps_suspend_resume_mode")) {
			pinfo->dfps_update = DFPS_SUSPEND_RESUME_MODE;
			pr_debug("dfps mode: suspend/resume\n");
		} else if (!strcmp(data, "dfps_immediate_clk_mode")) {
			pinfo->dfps_update = DFPS_IMMEDIATE_CLK_UPDATE_MODE;
			pr_debug("dfps mode: Immediate clk\n");
		} else if (!strcmp(data, "dfps_immediate_porch_mode_hfp")) {
			pinfo->dfps_update =
				DFPS_IMMEDIATE_PORCH_UPDATE_MODE_HFP;
			pr_debug("dfps mode: Immediate porch HFP\n");
		} else if (!strcmp(data, "dfps_immediate_porch_mode_vfp")) {
			pinfo->dfps_update =
				DFPS_IMMEDIATE_PORCH_UPDATE_MODE_VFP;
			pr_debug("dfps mode: Immediate porch VFP\n");
		} else {
			pinfo->dfps_update = DFPS_SUSPEND_RESUME_MODE;
			pr_debug("default dfps mode: suspend/resume\n");
		}
		mdss_dsi_set_refresh_rate_range(pan_node, pinfo);
	} else {
		pinfo->dynamic_fps = false;
		pr_debug("dfps update mode not configured: disable\n");
	}
	pinfo->new_fps = pinfo->mipi.frame_rate;

	return;
}

int mdss_panel_parse_bl_settings(struct device_node *np,
			struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	const char *data;
	int rc = 0;
	u32 tmp;

	ctrl_pdata->bklt_ctrl = UNKNOWN_CTRL;
	data = of_get_property(np, "qcom,mdss-dsi-bl-pmic-control-type", NULL);
	if (data) {
		if (!strcmp(data, "bl_ctrl_wled")) {
			led_trigger_register_simple("bkl-trigger",
				&bl_led_trigger);
			pr_debug("%s: SUCCESS-> WLED TRIGGER register\n",
				__func__);
			ctrl_pdata->bklt_ctrl = BL_WLED;
		} else if (!strcmp(data, "bl_ctrl_pwm")) {
			ctrl_pdata->bklt_ctrl = BL_PWM;
			ctrl_pdata->pwm_pmi = of_property_read_bool(np,
					"qcom,mdss-dsi-bl-pwm-pmi");
			rc = of_property_read_u32(np,
				"qcom,mdss-dsi-bl-pmic-pwm-frequency", &tmp);
			if (rc) {
				pr_err("%s:%d, Error, panel pwm_period\n",
						__func__, __LINE__);
				return -EINVAL;
			}
			ctrl_pdata->pwm_period = tmp;
			if (ctrl_pdata->pwm_pmi) {
				ctrl_pdata->pwm_bl = of_pwm_get(np, NULL);
				if (IS_ERR(ctrl_pdata->pwm_bl)) {
					pr_err("%s: Error, pwm device\n",
								__func__);
					ctrl_pdata->pwm_bl = NULL;
					return -EINVAL;
				}
			} else {
				rc = of_property_read_u32(np,
					"qcom,mdss-dsi-bl-pmic-bank-select",
								 &tmp);
				if (rc) {
					pr_err("%s:%d, Error, lpg channel\n",
							__func__, __LINE__);
					return -EINVAL;
				}
				ctrl_pdata->pwm_lpg_chan = tmp;
				tmp = of_get_named_gpio(np,
					"qcom,mdss-dsi-pwm-gpio", 0);
				ctrl_pdata->pwm_pmic_gpio = tmp;
				pr_debug("%s: Configured PWM bklt ctrl\n",
								 __func__);
			}
		} else if (!strcmp(data, "bl_ctrl_dcs")) {
			ctrl_pdata->bklt_ctrl = BL_DCS_CMD;
			pr_debug("%s: Configured DCS_CMD bklt ctrl\n",
								__func__);
		}
	}
	return 0;
}

int mdss_dsi_panel_timing_switch(struct mdss_dsi_ctrl_pdata *ctrl,
			struct mdss_panel_timing *timing)
{
	struct dsi_panel_timing *pt;
	struct mdss_panel_info *pinfo = &ctrl->panel_data.panel_info;
	int i;

	if (!timing)
		return -EINVAL;

	if (timing == ctrl->panel_data.current_timing) {
		pr_warn("%s: panel timing \"%s\" already set\n", __func__,
				timing->name);
		return 0; /* nothing to do */
	}

	pr_debug("%s: ndx=%d switching to panel timing \"%s\"\n", __func__,
			ctrl->ndx, timing->name);

	mdss_panel_info_from_timing(timing, pinfo);

	pt = container_of(timing, struct dsi_panel_timing, timing);
	pinfo->mipi.t_clk_pre = pt->t_clk_pre;
	pinfo->mipi.t_clk_post = pt->t_clk_post;

	for (i = 0; i < ARRAY_SIZE(pt->phy_timing); i++)
		pinfo->mipi.dsi_phy_db.timing[i] = pt->phy_timing[i];

	for (i = 0; i < ARRAY_SIZE(pt->phy_timing_8996); i++)
		pinfo->mipi.dsi_phy_db.timing_8996[i] = pt->phy_timing_8996[i];

	ctrl->on_cmds = pt->on_cmds;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	ctrl->vcom_cmds = pt->vcom_cmds;
#endif
#if defined(CONFIG_LGE_ENHANCE_GALLERY_SHARPNESS)
	ctrl->sharpness_on_cmds = pt->sharpness_on_cmds;
	ctrl->ce_on_cmds = pt->ce_on_cmds;
#endif
#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
	ctrl->ie_on_cmds = pt->ie_on_cmds;
	ctrl->ie_off_cmds = pt->ie_off_cmds;
	ctrl->cabc_20_cmds = pt->cabc_20_cmds;
	ctrl->cabc_30_cmds = pt->cabc_30_cmds;
#endif
	ctrl->post_panel_on_cmds = pt->post_panel_on_cmds;

	ctrl->panel_data.current_timing = timing;
	if (!timing->clk_rate)
		ctrl->refresh_clk_rate = true;
	mdss_dsi_clk_refresh(&ctrl->panel_data);

	return 0;
}

void mdss_dsi_unregister_bl_settings(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	if (ctrl_pdata->bklt_ctrl == BL_WLED)
		led_trigger_unregister_simple(bl_led_trigger);
}

static int mdss_dsi_panel_timing_from_dt(struct device_node *np,
		struct dsi_panel_timing *pt,
		struct mdss_panel_data *panel_data)
{
	u32 tmp;
	u64 tmp64;
	int rc, i, len;
	const char *data;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata;

	ctrl_pdata = container_of(panel_data, struct mdss_dsi_ctrl_pdata,
				panel_data);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-width", &tmp);
	if (rc) {
		pr_err("%s:%d, panel width not specified\n",
						__func__, __LINE__);
		return -EINVAL;
	}
	pt->timing.xres = tmp;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-height", &tmp);
	if (rc) {
		pr_err("%s:%d, panel height not specified\n",
						__func__, __LINE__);
		return -EINVAL;
	}
	pt->timing.yres = tmp;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-front-porch", &tmp);
	pt->timing.h_front_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-back-porch", &tmp);
	pt->timing.h_back_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-pulse-width", &tmp);
	pt->timing.h_pulse_width = (!rc ? tmp : 2);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-sync-skew", &tmp);
	pt->timing.hsync_skew = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-back-porch", &tmp);
	pt->timing.v_back_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-front-porch", &tmp);
	pt->timing.v_front_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-pulse-width", &tmp);
	pt->timing.v_pulse_width = (!rc ? tmp : 2);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-left-border", &tmp);
	pt->timing.border_left = !rc ? tmp : 0;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-right-border", &tmp);
	pt->timing.border_right = !rc ? tmp : 0;

	/* overriding left/right borders for split display cases */
	if (mdss_dsi_is_hw_config_split(ctrl_pdata->shared_data)) {
		if (panel_data->next)
			pt->timing.border_right = 0;
		else
			pt->timing.border_left = 0;
	}

	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-top-border", &tmp);
	pt->timing.border_top = !rc ? tmp : 0;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-bottom-border", &tmp);
	pt->timing.border_bottom = !rc ? tmp : 0;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-framerate", &tmp);
	pt->timing.frame_rate = !rc ? tmp : DEFAULT_FRAME_RATE;
	rc = of_property_read_u64(np, "qcom,mdss-dsi-panel-clockrate", &tmp64);
	if (rc == -EOVERFLOW)
		rc = of_property_read_u32(np,
			"qcom,mdss-dsi-panel-clockrate", (u32 *)&tmp64);
	pt->timing.clk_rate = !rc ? tmp64 : 0;

	data = of_get_property(np, "qcom,mdss-dsi-panel-timings", &len);
	if ((!data) || (len != 12)) {
		pr_err("%s:%d, Unable to read Phy timing settings",
		       __func__, __LINE__);
		return -EINVAL;
	}
	for (i = 0; i < len; i++)
		pt->phy_timing[i] = data[i];

	data = of_get_property(np, "qcom,mdss-dsi-panel-timings-8996", &len);
	if ((!data) || (len != 40)) {
		pr_debug("%s:%d, Unable to read 8996 Phy lane timing settings",
		       __func__, __LINE__);
	} else {
		for (i = 0; i < len; i++)
			pt->phy_timing_8996[i] = data[i];
	}

	rc = of_property_read_u32(np, "qcom,mdss-dsi-t-clk-pre", &tmp);
	pt->t_clk_pre = (!rc ? tmp : 0x24);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-t-clk-post", &tmp);
	pt->t_clk_post = (!rc ? tmp : 0x03);

	if (np->name) {
		pt->timing.name = kstrdup(np->name, GFP_KERNEL);
		pr_info("%s: found new timing \"%s\" (%p)\n", __func__,
				np->name, &pt->timing);
	}

	return 0;
}

static int  mdss_dsi_panel_config_res_properties(struct device_node *np,
		struct dsi_panel_timing *pt,
		struct mdss_panel_data *panel_data)
{
	int rc = 0;

	mdss_dsi_parse_dcs_cmds(np, &pt->on_cmds,
		"qcom,mdss-dsi-on-command",
		"qcom,mdss-dsi-on-command-state");
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	mdss_dsi_parse_dcs_cmds(np, &pt->vcom_cmds,
		"qcom,mdss-dsi-vcom-command",
		"qcom,mdss-dsi-on-command-state");
#endif
#if defined(CONFIG_LGE_ENHANCE_GALLERY_SHARPNESS)
	mdss_dsi_parse_dcs_cmds(np, &pt->sharpness_on_cmds,
		"qcom,mdss-dsi-sharpness-on-command",
		"qcom,mdss-dsi-on-command-state");
	mdss_dsi_parse_dcs_cmds(np, &pt->ce_on_cmds,
		"qcom,mdss-dsi-ce-on-command",
		"qcom,mdss-dsi-on-command-state");
#endif
#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
	mdss_dsi_parse_dcs_cmds(np, &pt->ie_on_cmds,
		"qcom,mdss-dsi-ie-on-command",
		"qcom,mdss-dsi-on-command-state");
	mdss_dsi_parse_dcs_cmds(np, &pt->ie_off_cmds,
		"qcom,mdss-dsi-ie-off-command",
		"qcom,mdss-dsi-on-command-state");
	mdss_dsi_parse_dcs_cmds(np, &pt->cabc_20_cmds,
		"qcom,mdss-dsi-cabc-20-command",
		"qcom,mdss-dsi-on-command-state");
	mdss_dsi_parse_dcs_cmds(np, &pt->cabc_30_cmds,
		"qcom,mdss-dsi-cabc-30-command",
		"qcom,mdss-dsi-on-command-state");
#endif
	mdss_dsi_parse_dcs_cmds(np, &pt->post_panel_on_cmds,
		"qcom,mdss-dsi-post-panel-on-command", NULL);

	mdss_dsi_parse_dcs_cmds(np, &pt->switch_cmds,
		"qcom,mdss-dsi-timing-switch-command",
		"qcom,mdss-dsi-timing-switch-command-state");

	rc = mdss_dsi_parse_topology_config(np, pt, panel_data);
	if (rc) {
		pr_err("%s: parsing compression params failed. rc:%d\n",
			__func__, rc);
		return rc;
	}

	mdss_panel_parse_te_params(np, &pt->timing);
	return rc;
}

static int mdss_panel_parse_display_timings(struct device_node *np,
		struct mdss_panel_data *panel_data)
{
	struct mdss_dsi_ctrl_pdata *ctrl;
	struct dsi_panel_timing *modedb;
	struct device_node *timings_np;
	struct device_node *entry;
	int num_timings, rc;
	int i = 0, active_ndx = 0;

	ctrl = container_of(panel_data, struct mdss_dsi_ctrl_pdata, panel_data);

	INIT_LIST_HEAD(&panel_data->timings_list);

	timings_np = of_get_child_by_name(np, "qcom,mdss-dsi-display-timings");
	if (!timings_np) {
		struct dsi_panel_timing pt;
		memset(&pt, 0, sizeof(struct dsi_panel_timing));

		/*
		 * display timings node is not available, fallback to reading
		 * timings directly from root node instead
		 */
		pr_debug("reading display-timings from panel node\n");
		rc = mdss_dsi_panel_timing_from_dt(np, &pt, panel_data);
		if (!rc) {
			mdss_dsi_panel_config_res_properties(np, &pt,
					panel_data);
			rc = mdss_dsi_panel_timing_switch(ctrl, &pt.timing);
		}
		return rc;
	}

	num_timings = of_get_child_count(timings_np);
	if (num_timings == 0) {
		pr_err("no timings found within display-timings\n");
		rc = -EINVAL;
		goto exit;
	}

	modedb = kcalloc(num_timings, sizeof(*modedb), GFP_KERNEL);
	if (!modedb) {
		rc = -ENOMEM;
		goto exit;
	}

	for_each_child_of_node(timings_np, entry) {
		rc = mdss_dsi_panel_timing_from_dt(entry, (modedb + i),
				panel_data);
		if (rc) {
			kfree(modedb);
			goto exit;
		}

		mdss_dsi_panel_config_res_properties(entry, (modedb + i),
				panel_data);

		/* if default is set, use it otherwise use first as default */
		if (of_property_read_bool(entry,
				"qcom,mdss-dsi-timing-default"))
			active_ndx = i;

		list_add(&modedb[i].timing.list,
				&panel_data->timings_list);
		i++;
	}

	/* Configure default timing settings */
	rc = mdss_dsi_panel_timing_switch(ctrl, &modedb[active_ndx].timing);
	if (rc)
		pr_err("unable to configure default timing settings\n");

exit:
	of_node_put(timings_np);

	return rc;
}

static int mdss_panel_parse_dt(struct device_node *np,
			struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	u32 tmp;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	int i;
	u32 *array;
#endif
	int rc;
	const char *data;
	static const char *pdest;
	struct mdss_panel_info *pinfo = &(ctrl_pdata->panel_data.panel_info);

	if (mdss_dsi_is_hw_config_split(ctrl_pdata->shared_data))
		pinfo->is_split_display = true;

	rc = of_property_read_u32(np,
		"qcom,mdss-pan-physical-width-dimension", &tmp);
	pinfo->physical_width = (!rc ? tmp : 0);
	rc = of_property_read_u32(np,
		"qcom,mdss-pan-physical-height-dimension", &tmp);
	pinfo->physical_height = (!rc ? tmp : 0);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-bpp", &tmp);
	if (rc) {
		pr_err("%s:%d, bpp not specified\n", __func__, __LINE__);
		return -EINVAL;
	}
	pinfo->bpp = (!rc ? tmp : 24);
	pinfo->mipi.mode = DSI_VIDEO_MODE;
	data = of_get_property(np, "qcom,mdss-dsi-panel-type", NULL);
	if (data && !strncmp(data, "dsi_cmd_mode", 12))
		pinfo->mipi.mode = DSI_CMD_MODE;
	pinfo->mipi.boot_mode = pinfo->mipi.mode;
	tmp = 0;
	data = of_get_property(np, "qcom,mdss-dsi-pixel-packing", NULL);
	if (data && !strcmp(data, "loose"))
		pinfo->mipi.pixel_packing = 1;
	else
		pinfo->mipi.pixel_packing = 0;
	rc = mdss_panel_get_dst_fmt(pinfo->bpp,
		pinfo->mipi.mode, pinfo->mipi.pixel_packing,
		&(pinfo->mipi.dst_format));
	if (rc) {
		pr_debug("%s: problem determining dst format. Set Default\n",
			__func__);
		pinfo->mipi.dst_format =
			DSI_VIDEO_DST_FORMAT_RGB888;
	}
	pdest = of_get_property(np,
		"qcom,mdss-dsi-panel-destination", NULL);

	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-underflow-color", &tmp);
	pinfo->lcdc.underflow_clr = (!rc ? tmp : 0xff);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-border-color", &tmp);
	pinfo->lcdc.border_clr = (!rc ? tmp : 0);
	data = of_get_property(np, "qcom,mdss-dsi-panel-orientation", NULL);
	if (data) {
		pr_debug("panel orientation is %s\n", data);
		if (!strcmp(data, "180"))
			pinfo->panel_orientation = MDP_ROT_180;
		else if (!strcmp(data, "hflip"))
			pinfo->panel_orientation = MDP_FLIP_LR;
		else if (!strcmp(data, "vflip"))
			pinfo->panel_orientation = MDP_FLIP_UD;
	}

	rc = of_property_read_u32(np, "qcom,mdss-brightness-max-level", &tmp);
	pinfo->brightness_max = (!rc ? tmp : MDSS_MAX_BL_BRIGHTNESS);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-bl-min-level", &tmp);
	pinfo->bl_min = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-bl-max-level", &tmp);
	pinfo->bl_max = (!rc ? tmp : 255);
	ctrl_pdata->bklt_max = pinfo->bl_max;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	rc = of_property_read_u32(np, "qcom,blmap-size", &tmp);
	pinfo->blmap_size = (!rc ? tmp : 0);
#if defined(CONFIG_LGE_HIGH_LUMINANCE_MODE)
	rc = of_property_read_u32(np, "qcom,hl-blmap-size", &tmp);
	pinfo->hl_blmap_size = (!rc ? tmp : 0);
#endif
	if (pinfo->blmap_size) {
		array = kzalloc(sizeof(u32) * pinfo->blmap_size, GFP_KERNEL);

		if (!array)
			return -ENOMEM;
		rc = of_property_read_u32_array(np,
			"qcom,blmap", array, pinfo->blmap_size);

		if (rc) {
			pr_err("%s:%d, unable to read backlight map\n",
					__func__, __LINE__);
			kfree(array);
			goto error;
		}

		pinfo->blmap = kzalloc(sizeof(int) * pinfo->blmap_size,
					GFP_KERNEL);
		if (!pinfo->blmap){
			kfree(array);
			return -ENOMEM;
		}

		for (i = 0; i < pinfo->blmap_size; i++)
			pinfo->blmap[i] = array[i];

		kfree(array);
	} else {
		pinfo->blmap = NULL;
	}
#if defined(CONFIG_LGE_HIGH_LUMINANCE_MODE)
	if (pinfo->hl_blmap_size) {
		array = kzalloc(sizeof(u32) * pinfo->hl_blmap_size, GFP_KERNEL);

		if (!array)
			return -ENOMEM;
		rc = of_property_read_u32_array(np,
			"qcom,hl-blmap", array, pinfo->hl_blmap_size);

		if (rc) {
			pr_err("%s:%d, unable to read backlight map\n",
					__func__, __LINE__);
			kfree(array);
			goto error;
		}

		pinfo->hl_blmap = kzalloc(sizeof(int) * pinfo->hl_blmap_size,
					GFP_KERNEL);
		if (!pinfo->hl_blmap){
			kfree(array);
			return -ENOMEM;
		}

		for (i = 0; i < pinfo->hl_blmap_size; i++)
			pinfo->hl_blmap[i] = array[i];

		kfree(array);
	} else {
		pinfo->hl_blmap = NULL;
	}
#endif
#endif

	rc = of_property_read_u32(np, "qcom,mdss-dsi-interleave-mode", &tmp);
	pinfo->mipi.interleave_mode = (!rc ? tmp : 0);

	pinfo->mipi.vsync_enable = of_property_read_bool(np,
		"qcom,mdss-dsi-te-check-enable");

	if (pinfo->sim_panel_mode == SIM_SW_TE_MODE)
		pinfo->mipi.hw_vsync_mode = false;
	else
		pinfo->mipi.hw_vsync_mode = of_property_read_bool(np,
			"qcom,mdss-dsi-te-using-te-pin");

	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-h-sync-pulse", &tmp);
	pinfo->mipi.pulse_mode_hsa_he = (!rc ? tmp : false);

	pinfo->mipi.hfp_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-hfp-power-mode");
	pinfo->mipi.hsa_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-hsa-power-mode");
	pinfo->mipi.hbp_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-hbp-power-mode");
	pinfo->mipi.last_line_interleave_en = of_property_read_bool(np,
		"qcom,mdss-dsi-last-line-interleave");
	pinfo->mipi.bllp_power_stop = of_property_read_bool(np,
		"qcom,mdss-dsi-bllp-power-mode");
	pinfo->mipi.eof_bllp_power_stop = of_property_read_bool(
		np, "qcom,mdss-dsi-bllp-eof-power-mode");
	pinfo->mipi.traffic_mode = DSI_NON_BURST_SYNCH_PULSE;
	data = of_get_property(np, "qcom,mdss-dsi-traffic-mode", NULL);
	if (data) {
		if (!strcmp(data, "non_burst_sync_event"))
			pinfo->mipi.traffic_mode = DSI_NON_BURST_SYNCH_EVENT;
		else if (!strcmp(data, "burst_mode"))
			pinfo->mipi.traffic_mode = DSI_BURST_MODE;
	}
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-te-dcs-command", &tmp);
	pinfo->mipi.insert_dcs_cmd =
			(!rc ? tmp : 1);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-wr-mem-continue", &tmp);
	pinfo->mipi.wr_mem_continue =
			(!rc ? tmp : 0x3c);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-wr-mem-start", &tmp);
	pinfo->mipi.wr_mem_start =
			(!rc ? tmp : 0x2c);
	rc = of_property_read_u32(np,
		"qcom,mdss-dsi-te-pin-select", &tmp);
	pinfo->mipi.te_sel =
			(!rc ? tmp : 1);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-virtual-channel-id", &tmp);
	pinfo->mipi.vc = (!rc ? tmp : 0);
	pinfo->mipi.rgb_swap = DSI_RGB_SWAP_RGB;
	data = of_get_property(np, "qcom,mdss-dsi-color-order", NULL);
	if (data) {
		if (!strcmp(data, "rgb_swap_rbg"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_RBG;
		else if (!strcmp(data, "rgb_swap_bgr"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_BGR;
		else if (!strcmp(data, "rgb_swap_brg"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_BRG;
		else if (!strcmp(data, "rgb_swap_grb"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_GRB;
		else if (!strcmp(data, "rgb_swap_gbr"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_GBR;
	}
	pinfo->mipi.data_lane0 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-0-state");
	pinfo->mipi.data_lane1 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-1-state");
	pinfo->mipi.data_lane2 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-2-state");
	pinfo->mipi.data_lane3 = of_property_read_bool(np,
		"qcom,mdss-dsi-lane-3-state");

	rc = mdss_panel_parse_display_timings(np, &ctrl_pdata->panel_data);
	if (rc)
		return rc;

	pinfo->mipi.rx_eot_ignore = of_property_read_bool(np,
		"qcom,mdss-dsi-rx-eot-ignore");
	pinfo->mipi.tx_eot_append = of_property_read_bool(np,
		"qcom,mdss-dsi-tx-eot-append");

	rc = of_property_read_u32(np, "qcom,mdss-dsi-stream", &tmp);
	pinfo->mipi.stream = (!rc ? tmp : 0);

	data = of_get_property(np, "qcom,mdss-dsi-panel-mode-gpio-state", NULL);
	if (data) {
		if (!strcmp(data, "high"))
			pinfo->mode_gpio_state = MODE_GPIO_HIGH;
		else if (!strcmp(data, "low"))
			pinfo->mode_gpio_state = MODE_GPIO_LOW;
	} else {
		pinfo->mode_gpio_state = MODE_GPIO_NOT_VALID;
	}

	rc = of_property_read_u32(np, "qcom,mdss-mdp-transfer-time-us", &tmp);
	pinfo->mdp_transfer_time_us = (!rc ? tmp : DEFAULT_MDP_TRANSFER_TIME);

	pinfo->mipi.lp11_init = of_property_read_bool(np,
					"qcom,mdss-dsi-lp11-init");
	rc = of_property_read_u32(np, "qcom,mdss-dsi-init-delay-us", &tmp);
	pinfo->mipi.init_delay = (!rc ? tmp : 0);

	mdss_dsi_parse_roi_alignment(np, pinfo);

	mdss_dsi_parse_trigger(np, &(pinfo->mipi.mdp_trigger),
		"qcom,mdss-dsi-mdp-trigger");

	mdss_dsi_parse_trigger(np, &(pinfo->mipi.dma_trigger),
		"qcom,mdss-dsi-dma-trigger");

	mdss_dsi_parse_reset_seq(np, pinfo->rst_seq, &(pinfo->rst_seq_len),
		"qcom,mdss-dsi-reset-sequence");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->off_cmds,
		"qcom,mdss-dsi-off-command", "qcom,mdss-dsi-off-command-state");

	rc = of_property_read_u32(np, "qcom,adjust-timer-wakeup-ms", &tmp);
	pinfo->adjust_timer_delay_ms = (!rc ? tmp : 0);

	pinfo->mipi.force_clk_lane_hs = of_property_read_bool(np,
		"qcom,mdss-dsi-force-clock-lane-hs");

	rc = mdss_dsi_parse_panel_features(np, ctrl_pdata);
	if (rc) {
		pr_err("%s: failed to parse panel features\n", __func__);
		goto error;
	}

	mdss_dsi_parse_panel_horizintal_line_idle(np, ctrl_pdata);

	mdss_dsi_parse_dfps_config(np, ctrl_pdata);

	return 0;

error:
	return -EINVAL;
}

int mdss_dsi_panel_init(struct device_node *node,
	struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	int rc = 0;
	static const char *panel_name;
	struct mdss_panel_info *pinfo;
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	static struct class *panel = NULL;
	static struct device *panel_sysfs_dev = NULL;
#endif

	if (!node || !ctrl_pdata) {
		pr_err("%s: Invalid arguments\n", __func__);
		return -ENODEV;
	}

	pinfo = &ctrl_pdata->panel_data.panel_info;

	pr_debug("%s:%d\n", __func__, __LINE__);
	pinfo->panel_name[0] = '\0';
	panel_name = of_get_property(node, "qcom,mdss-dsi-panel-name", NULL);
	if (!panel_name) {
		pr_info("%s:%d, Panel name not specified\n",
						__func__, __LINE__);
	} else {
		pr_info("%s: Panel Name = %s\n", __func__, panel_name);
		strlcpy(&pinfo->panel_name[0], panel_name, MDSS_MAX_PANEL_LEN);

#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
		if (strncmp(panel_name, "LGD SIC LG4945", 14) == 0) {
			pr_err("%s: panel_type is LGD_SIC_LG4945_INCELL_CMD_PANEL\n",
					__func__);
			pinfo->panel_type = LGD_SIC_LG4945_INCELL_CMD_PANEL;
		} else if (strncmp(panel_name, "LGD SIC LG4946", 14) == 0) {
			pr_err("%s: panel_type is LGD_SIC_LG4946_INCELL_CMD_PANEL\n",
					__func__);
			pinfo->panel_type = LGE_SIC_LG4946_INCELL_CND_PANEL;
		} else if (strncmp(panel_name, "LGD RSP", 7) == 0) {
			pr_err("%s: panel_type is LGD_R69007_INCELL_CMD_PANEL\n",
					__func__);
			pinfo->panel_type = LGD_R69007_INCELL_CMD_PANEL;
		} else if (strncmp(panel_name, "LGD Synaptics", 13)==0) {
			pr_err("%s: panel_type is LGD_TD4302_INCELL_CMD_PANEL\n",
					__func__);
			pinfo->panel_type = LGE_TD4302_INCELL_CND_PANEL;
		} else {
			pr_err("%s: Invalid panel type\n", __func__);
		}
		lge_set_panel(pinfo->panel_type);
#endif
	}
	rc = mdss_panel_parse_dt(node, ctrl_pdata);
	if (rc) {
		pr_err("%s:%d panel dt parse failed\n", __func__, __LINE__);
		return rc;
	}

	pinfo->dynamic_switch_pending = false;
	pinfo->is_lpm_mode = false;
	pinfo->esd_rdy = false;
#ifdef CONFIG_LGE_LCD_MFTS_MODE
	if(lge_get_mfts_mode())
		pinfo->power_ctrl = true;
#endif
	ctrl_pdata->on = mdss_dsi_panel_on;
	ctrl_pdata->post_panel_on = mdss_dsi_post_panel_on;
	ctrl_pdata->off = mdss_dsi_panel_off;
	ctrl_pdata->low_power_config = mdss_dsi_panel_low_power_config;
	ctrl_pdata->panel_data.set_backlight = mdss_dsi_panel_bl_ctrl;
	ctrl_pdata->switch_mode = mdss_dsi_panel_switch_mode;
#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
	ctrl_pdata->ie_on = 1;
#endif
#if defined(CONFIG_LGE_DISPLAY_AOD_SUPPORTED)
	/* Only parse aod command in DSI0 ctrl */
	if (ctrl_pdata->panel_data.panel_info.pdest == DISPLAY_1) {
		rc = oem_mdss_aod_init(node, ctrl_pdata);
		if (rc) {
			pr_err("[AOD] %s:%d aod cmd dt parse failed\n", __func__, __LINE__);
			return rc;
		}
	}
#endif
#if defined(CONFIG_MACH_LGE)
	if (ctrl_pdata->panel_data.panel_info.pdest == DISPLAY_1) {
		if (pdata_base == NULL)
			pdata_base = &(ctrl_pdata->panel_data);
	}
#endif
#if defined(CONFIG_LGE_MIPI_H1_INCELL_QHD_CMD_PANEL)
	if(!panel){
		panel = class_create(THIS_MODULE, "panel");
		if (IS_ERR(panel))
			pr_err("%s: Failed to create panel class\n", __func__);
	}
	if(!panel_sysfs_dev){
		panel_sysfs_dev = device_create(panel, NULL, 0, NULL, "img_tune");
		if (IS_ERR(panel_sysfs_dev)) {
			pr_err("%s: Failed to create dev(panel_sysfs_dev)!", __func__);
		}
		else{
#if defined(CONFIG_LGE_ENHANCE_GALLERY_SHARPNESS)
			if (device_create_file(panel_sysfs_dev, &dev_attr_sharpness) < 0)
				pr_err("%s: add sharpness tuning node fail!", __func__);
			if (device_create_file(panel_sysfs_dev, &dev_attr_color_enhance) < 0)
				pr_err("%s: add color enhancement tuning node fail!", __func__);
#endif
#if defined(CONFIG_LGE_LCD_DYNAMIC_CABC_MIE_CTRL)
			if (device_create_file(panel_sysfs_dev, &dev_attr_image_enhance_set) < 0)
				pr_err("%s: add image enhance set node fail!", __func__);
			if (device_create_file(panel_sysfs_dev, &dev_attr_cabc) < 0)
				pr_err("%s: add cabc set node fail!", __func__);
#endif
		}
	}
#endif
	return 0;
}

#if defined(CONFIG_LGE_PP_AD_SUPPORTED)
void mdss_dsi_panel_dimming_ctrl(int bl_level, int current_bl)
{
	qpnp_wled_dimming(bl_level,current_bl);
}
#endif
