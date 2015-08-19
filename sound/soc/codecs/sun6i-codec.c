/*
 * sound\soc\sun6i\sun6i-codec.c
 * (C) Copyright 2010-2016
 * reuuimllatech Technology Co., Ltd. <www.reuuimllatech.com>
 * huangxin <huangxin@reuuimllatech.com>
 *
 * some simple description for this code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */
#define DEBUG
#ifndef CONFIG_PM
#define CONFIG_PM
#endif
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#ifdef CONFIG_PM
#include <linux/pm.h>
#endif
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/initval.h>
#include <sound/soc.h>
#include <linux/clk.h>
#include <linux/timer.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/of_gpio.h>
#include "sun6i-codec.h"

struct clk *codec_apbclk,*codec_pll2clk,*codec_moduleclk;
static unsigned int play_dmasrc = 0;

/*for pa gpio ctrl*/
static int req_status;
static bool codec_lineinin_enabled = false;
static bool codec_lineincap_enabled = false;
static bool codec_speakerout_enabled = false;
static bool codec_adcphonein_enabled = false;
static bool codec_dacphoneout_enabled = false;
static bool codec_headphoneout_enabled = false;
static bool codec_earpieceout_enabled = false;
static bool codec_phonecap_enabled = false;
static bool codec_phonein_enabled = false;
static bool codec_phoneout_enabled = false;
static bool codec_speaker_enabled = false;

struct sun6i_priv {
	void __iomem	*base;

	struct clk	*apb_clk;
	struct clk	*mod_clk;

	struct reset_control	*rstc;

	unsigned	pa_gpio;
};

struct sun6i_codec {
	long samplerate;
	struct snd_card *card;
	struct snd_pcm *pcm;		
};

/*------------- Structure/enum declaration ------------------- */
typedef struct codec_board_info {
	struct device	*dev;	     		/* parent device */
	struct resource	*codec_base_res;   /* resources found */
	struct resource	*codec_base_req;   /* resources found */

	spinlock_t	lock;
} codec_board_info_t;

struct sun6i_playback_runtime_data {
	spinlock_t lock;
	int state;
	unsigned int dma_loaded;
	unsigned int dma_limit;
	unsigned int dma_period;
	dma_addr_t   dma_start;
	dma_addr_t   dma_pos;
	dma_addr_t	 dma_end;
//	dm_hdl_t	dma_hdl;
	bool		play_dma_flag;
//	struct dma_cb_t play_done_cb;
	struct sun6i_pcm_dma_params	*params;
};

struct sun6i_capture_runtime_data {
	spinlock_t lock;
	int state;
	unsigned int dma_loaded;
	unsigned int dma_limit;
	unsigned int dma_period;
	dma_addr_t   dma_start;
	dma_addr_t   dma_pos;
	dma_addr_t	 dma_end;
//	dm_hdl_t	dma_hdl;
	bool		capture_dma_flag;
//	struct dma_cb_t capture_done_cb;
	struct sun6i_pcm_dma_params	*params;
};

/**
* codec_wrreg_bits - update codec register bits
* @reg: codec register
* @mask: register mask
* @value: new value
*
* Writes new register value.
* Return 1 for change else 0.
*/
int codec_wrreg_bits(unsigned short reg, unsigned int	mask,	unsigned int value)
{
	unsigned int old, new;
		
	old	=	codec_rdreg(reg);
	new	=	(old & ~mask) | value;
	codec_wrreg(reg,new);

	return 0;
}

/**
*	snd_codec_info_volsw	-	single	mixer	info	callback
*	@kcontrol:	mixer control
*	@uinfo:	control	element	information
*	Callback to provide information about a single mixer control
*
*	Returns 0 for success
*/
int snd_codec_info_volsw(struct snd_kcontrol *kcontrol,
		struct	snd_ctl_elem_info	*uinfo)
{
	struct	codec_mixer_control *mc	= (struct codec_mixer_control*)kcontrol->private_value;
	int	max	=	mc->max;
	unsigned int shift  = mc->shift;
	unsigned int rshift = mc->rshift;

	if(max	== 1)
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;//the info of type
	else
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;

	uinfo->count = shift ==	rshift	?	1:	2;	//the info of elem count
	uinfo->value.integer.min = 0;				//the info of min value
	uinfo->value.integer.max = max;				//the info of max value
	return	0;
}

/**
*	snd_codec_get_volsw	-	single	mixer	get	callback
*	@kcontrol:	mixer	control
*	@ucontrol:	control	element	information
*
*	Callback to get the value of a single mixer control
*	return 0 for success.
*/
int snd_codec_get_volsw(struct snd_kcontrol	*kcontrol,
		struct	snd_ctl_elem_value	*ucontrol)
{
	struct codec_mixer_control *mc= (struct codec_mixer_control*)kcontrol->private_value;
	unsigned int shift = mc->shift;
	unsigned int rshift = mc->rshift;
	int	max = mc->max;
	/*fls(7) = 3,fls(1)=1,fls(0)=0,fls(15)=4,fls(3)=2,fls(23)=5*/
	unsigned int mask = (1 << fls(max)) -1;
	unsigned int invert = mc->invert;
	unsigned int reg = mc->reg;

	ucontrol->value.integer.value[0] =	
		(codec_rdreg(reg)>>	shift) & mask;
	if(shift != rshift)
		ucontrol->value.integer.value[1] =
			(codec_rdreg(reg) >> rshift) & mask;

	if(invert){
		ucontrol->value.integer.value[0] =
			max - ucontrol->value.integer.value[0];
		if(shift != rshift)
			ucontrol->value.integer.value[1] =
				max - ucontrol->value.integer.value[1];
		}
	
		return 0;
}

/**
*	snd_codec_put_volsw	-	single	mixer put callback
*	@kcontrol:	mixer	control
*	@ucontrol:	control	element	information
*
*	Callback to put the value of a single mixer control
*
* return 0 for success.
*/
int snd_codec_put_volsw(struct	snd_kcontrol	*kcontrol,
	struct	snd_ctl_elem_value	*ucontrol)
{
	struct codec_mixer_control *mc= (struct codec_mixer_control*)kcontrol->private_value;
	unsigned int reg = mc->reg;
	unsigned int shift = mc->shift;
	unsigned int rshift = mc->rshift;
	int max = mc->max;
	unsigned int mask = (1<<fls(max))-1;
	unsigned int invert = mc->invert;
	unsigned int	val, val2, val_mask;
	
	val = (ucontrol->value.integer.value[0] & mask);
	if(invert)
		val = max - val;
	val <<= shift;
	val_mask = mask << shift;
	if(shift != rshift){
		val2	= (ucontrol->value.integer.value[1] & mask);
		if(invert)
			val2	=	max	- val2;
		val_mask |= mask <<rshift;
		val |= val2 <<rshift;
	}
	
	return codec_wrreg_bits(reg,val_mask,val);
}

int codec_wr_control(u32 reg, u32 mask, u32 shift, u32 val)
{
	u32 reg_val;
	reg_val = val << shift;
	mask = mask << shift;
	codec_wrreg_bits(reg, mask, reg_val);
	return 0;
}

int codec_rd_control(u32 reg, u32 bit, u32 *val)
{
	return 0;
}

static int codec_pa_play_open(void)
{
	/* int pa_vol = 0; */
	/* script_item_u val; */
	/* script_item_value_type_e  type; */
	/* int pa_double_used = 0; */

	/* type = script_get_item("audio_para", "pa_double_used", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* pa_double_used = val.val; */
	/* if (!pa_double_used) { */
	/* 	type = script_get_item("audio_para", "pa_single_vol", &val); */
	/* 	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 		printk("[audiocodec] type err!\n"); */
	/* 	} */
	/* 	pa_vol = val.val; */
	/* } else { */
	/* 	type = script_get_item("audio_para", "pa_double_vol", &val); */
	/* 	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 		printk("[audiocodec] type err!\n"); */
	/* 	} */
	/* 	pa_vol = val.val; */
	/* } */

	/*mute l_pa and r_pa*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x0);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x0);

	/*enable dac digital*/
	codec_wr_control(SUN6I_DAC_DPC, 0x1, DAC_EN, 0x1);
	/*set TX FIFO send drq level*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x7f, TX_TRI_LEVEL, 0xf);
	/*set TX FIFO MODE*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, TX_FIFO_MODE, 0x1);

	//send last sample when dac fifo under run
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, LAST_SE, 0x0);

	/*enable dac_l and dac_r*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACALEN, 0x1);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACAREN, 0x1);

	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_EN, 0x1);
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_EN, 0x1);

	/* TODO: This used to be retrieved by FEX */
	/* if (!pa_double_used) { */
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_SRC_SEL, 0x1);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_SRC_SEL, 0x1);
	/* } else { */
	/* 	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_SRC_SEL, 0x0); */
	/* 	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_SRC_SEL, 0x0); */
	/* } */

	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x1);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x1);

	codec_wr_control(SUN6I_DAC_ACTL, 0x7f, RMIXMUTE, 0x2);
	codec_wr_control(SUN6I_DAC_ACTL, 0x7f, LMIXMUTE, 0x2);

	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LMIXEN, 0x1);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RMIXEN, 0x1);
	
	/*
	 * TODO: This used to be retrieved by FEX.
	 * The script has nice comments explaining what values mean in term of dB output
	 */
	codec_wr_control(SUN6I_MIC_CTRL, 0x1f, LINEOUT_VOL, 0x19);

	/* TODO: Configure the GPIO using gpiolib */
	/* mdelay(3); */
	/* item.gpio.data = 1; */
	/* /\*config gpio info of audio_pa_ctrl open*\/ */
	/* if (0 != sw_gpio_setall_range(&item.gpio, 1)) { */
	/* 	printk("sw_gpio_setall_range failed\n"); */
	/* } */
	/* mdelay(62); */

	return 0;
}

static int codec_headphone_play_open(void)
{
	int headphone_vol = 0;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */

	/* type = script_get_item("audio_para", "headphone_vol", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* headphone_vol = val.val; */

	/* TODO: This used to be retrieved by FEX */
	headphone_vol = 0x3b;

	/*mute l_pa and r_pa*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x0);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x0);

	codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, ZERO_CROSS_EN, 0x1);
	/*enable dac digital*/
	codec_wr_control(SUN6I_DAC_DPC, 0x1, DAC_EN, 0x1);

	/*set TX FIFO send drq level*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x7f, TX_TRI_LEVEL, 0xf);
	/*set TX FIFO MODE*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, TX_FIFO_MODE, 0x1);

	//send last sample when dac fifo under run
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, LAST_SE, 0x0);

	/*enable dac_l and dac_r*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACALEN, 0x1);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACAREN, 0x1);

	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LMIXEN, 0x1);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RMIXEN, 0x1);

	codec_wr_control(SUN6I_DAC_ACTL, 0x7f, RMIXMUTE, 0x2);
	codec_wr_control(SUN6I_DAC_ACTL, 0x7f, LMIXMUTE, 0x2);

	/*set HPVOL volume*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, headphone_vol);

	return 0;
}

static int codec_capture_open(void)
{
	int cap_vol = 0;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */

	/* type = script_get_item("audio_para", "cap_vol", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* cap_vol = val.val; */

	/* TODO: This used to be retrieved by FEX */
	cap_vol = 5;

	/*enable mic1 pa*/
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, MIC1AMPEN, 0x1);
	/*mic1 gain 36dB,if capture volume is too small, enlarge the mic1boost*/
	codec_wr_control(SUN6I_MIC_CTRL, 0x7,MIC1BOOST,cap_vol);//36db
	/*enable Master microphone bias*/
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, MBIASEN, 0x1);

	/*enable Right MIC1 Boost stage*/
	codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTEMIC1BOOST, 0x1);
	/*enable Left MIC1 Boost stage*/
	codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTEMIC1BOOST, 0x1);
	/*enable adc_r adc_l analog*/
	codec_wr_control(SUN6I_ADC_ACTL, 0x1,  ADCREN, 0x1);
	codec_wr_control(SUN6I_ADC_ACTL, 0x1,  ADCLEN, 0x1);
	/*set RX FIFO mode*/
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1, RX_FIFO_MODE, 0x1);
	/*set RX FIFO rec drq level*/
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1f, RX_TRI_LEVEL, 0xf);
	/*enable adc digital part*/
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1,ADC_EN, 0x1);

	return 0;
}

static int codec_play_start(void)
{
	/*enable dac drq*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, DAC_DRQ, 0x1);
	/*DAC FIFO Flush,Write '1' to flush TX FIFO, self clear to '0'*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, DAC_FIFO_FLUSH, 0x1);

	return 0;
}

static int codec_play_stop(void)
{
	int i = 0;
	int headphone_vol = 0;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */

	/* type = script_get_item("audio_para", "headphone_vol", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* headphone_vol = val.val; */

	/* TODO: This used to be retrieved by FEX*/
	headphone_vol = 0x3b;

	codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, ZERO_CROSS_EN, 0x0);
	for (i = 0; i < headphone_vol; i++) {
		/*set HPVOL volume*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, headphone_vol);
		headphone_vol = headphone_vol - i;
		mdelay(1);
		i++;
		if (i > headphone_vol-1) {
			codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, 0x0);
			break;
		}
	}
	/*mute l_pa and r_pa*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x0);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x0);

	/*disable dac drq*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, DAC_DRQ, 0x0);

	/*disable dac_l and dac_r*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACALEN, 0x0);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACAREN, 0x0);

	/*disable dac digital*/
	codec_wr_control(SUN6I_DAC_DPC ,  0x1, DAC_EN, 0x0);

	/* TODO: Configure the GPIO */
	/* item.gpio.data = 0; */
	/* /\*config gpio info of audio_pa_ctrl open*\/ */
	/* if (0 != sw_gpio_setall_range(&item.gpio, 1)) { */
	/* 	printk("sw_gpio_setall_range failed\n"); */
	/* } */

	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_EN, 0x0);
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_EN, 0x0);

	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_SRC_SEL, 0x0);
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_SRC_SEL, 0x0);

	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x0);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x0);
	
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS2, 0x0);
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS3, 0x0);
	return 0;
}

static int codec_capture_start(void)
{
	/*enable adc drq*/
	codec_wr_control(SUN6I_ADC_FIFOC ,0x1, ADC_DRQ, 0x1);
	return 0;
}

static int codec_capture_stop(void)
{
	/*disable adc digital part*/
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1,ADC_EN, 0x0);
	/*disable adc drq*/
	codec_wr_control(SUN6I_ADC_FIFOC ,0x1, ADC_DRQ, 0x0);	
	/*disable mic1 pa*/
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, MIC1AMPEN, 0x0);
	/*disable Master microphone bias*/
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, MBIASEN, 0x0);
	/*disable adc_r adc_l analog*/
	codec_wr_control(SUN6I_ADC_ACTL, 0x1, ADCREN, 0x0);
	codec_wr_control(SUN6I_ADC_ACTL, 0x1, ADCLEN, 0x0);

	return 0;
}

/*
 *	codec_lineinin_enabled == 1, open the linein in.
 *	codec_lineinin_enabled == 0, close the linein in.
 */
static int codec_set_lineinin(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	codec_lineinin_enabled = ucontrol->value.integer.value[0];

	if (codec_lineinin_enabled) {
		/*select LINEINR*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, RMIXMUTE, 0x4);
		/*select LINEINL*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, LMIXMUTE, 0x4);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LMIXEN, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RMIXEN, 0x1);
	} else {
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, RMIXMUTE, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, LMIXMUTE, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LMIXEN, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RMIXEN, 0x0);
	}
	return 0;
}

static int codec_get_lineinin(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_lineinin_enabled;
	return 0;
}

static int codec_set_lineincap(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	codec_lineincap_enabled = ucontrol->value.integer.value[0];

	if (codec_lineincap_enabled) {
		/*enable LINEINR ADC*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTELINEINR, 0x1);
		/*enable LINEINL ADC*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTELINEINL, 0x1);
	} else {
		/*disable LINEINR ADC*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTELINEINR, 0x0);
		/*disable LINEINL ADC*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTELINEINL, 0x0);
	}
	return 0;
}

static int codec_get_lineincap(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_lineincap_enabled;
	return 0;
}

/*
 *	codec_speakerout_enabled == 1, open the speaker.
 *	codec_speakerout_enabled == 0, close the speaker.
 */
static int codec_set_speakerout(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int pa_vol = 0x19;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */
	int pa_double_used = 0;

	/* TODO: This used to be retrieved by FEX */
	/* type = script_get_item("audio_para", "pa_double_used", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* pa_double_used = val.val; */
	/* if (!pa_double_used) { */
	/* 	type = script_get_item("audio_para", "pa_single_vol", &val); */
	/* 	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 		printk("[audiocodec] type err!\n"); */
	/* 	} */
	/* 	pa_vol = val.val; */
	/* } else { */
	/* 	type = script_get_item("audio_para", "pa_double_vol", &val); */
	/* 	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 		printk("[audiocodec] type err!\n"); */
	/* 	} */
	/* 	pa_vol = val.val; */
	/* } */

	codec_speakerout_enabled = ucontrol->value.integer.value[0];

	if (codec_speakerout_enabled) {
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_EN, 0x1);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_EN, 0x1);

		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_SRC_SEL, 0x1);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_SRC_SEL, 0x1);

		codec_wr_control(SUN6I_MIC_CTRL, 0x1f, LINEOUT_VOL, pa_vol);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x1);

		/* TODO: Set GPIO */
		/* mdelay(3); */
		/* item.gpio.data = 1; */
		/* /\*config gpio info of audio_pa_ctrl open*\/ */
		/* if (0 != sw_gpio_setall_range(&item.gpio, 1)) { */
		/* 	printk("sw_gpio_setall_range failed\n"); */
		/* } */
		/* mdelay(62); */
	} else {
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_EN, 0x0);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_EN, 0x0);

		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_SRC_SEL, 0x0);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_SRC_SEL, 0x0);

		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x0);

		/* TODO: Set GPIO */
		/* item.gpio.data = 0; */
		/* /\*config gpio info of audio_pa_ctrl open*\/ */
		/* if (0 != sw_gpio_setall_range(&item.gpio, 1)) { */
		/* 	printk("sw_gpio_setall_range failed\n"); */
		/* } */
	}

	return 0;
}

static int codec_get_speakerout(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_speakerout_enabled;
	return 0;
}

/*
 *	codec_headphoneout_enabled == 1, open the headphone.
 *	codec_headphoneout_enabled == 0, close the headphone.
 */
static int codec_set_headphoneout(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	int headphone_vol = 0x3b;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */

	/* TODO: Retrieved by FEX */
	/* type = script_get_item("audio_para", "headphone_vol", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* headphone_vol = val.val; */

	codec_headphoneout_enabled = ucontrol->value.integer.value[0];

	if (codec_headphoneout_enabled) {
		/*unmute l_pa and r_pa*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x1);
		/*select the analog mixer input source*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x1);
		codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, ZERO_CROSS_EN, 0x1);
		/*set HPVOL volume*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, headphone_vol);
	} else {
		/*mute l_pa and r_pa*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x0);
		/*select the default dac input source*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x0);
		codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, ZERO_CROSS_EN, 0x0);
		/*set HPVOL volume*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, 0x0);
	}

	return 0;
}

static int codec_get_headphoneout(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_headphoneout_enabled;
	return 0;
}

/*
 *	codec_earpieceout_enabled == 1, open the earpiece.
 *	codec_earpieceout_enabled == 0, close the earpiece.
 */
static int codec_set_earpieceout(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	int headphone_vol = 0x3b;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */

	/* TODO: Retrieved by FEX */
	/* type = script_get_item("audio_para", "headphone_vol", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* headphone_vol = val.val; */

	codec_earpieceout_enabled = ucontrol->value.integer.value[0];

	if (codec_earpieceout_enabled) {
		/*unmute l_pa and r_pa*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x1);
		/*select the analog mixer input source*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x1);
		/*select HPL inverting output*/
		codec_wr_control(SUN6I_PA_CTRL, 0x3, HPCOM_CTL, 0x1);

		codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, ZERO_CROSS_EN, 0x1);
		/*set HPVOL volume*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, headphone_vol);
	} else {
		/*mute l_pa and r_pa*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x0);
		codec_wr_control(SUN6I_PA_CTRL, 0x3, HPCOM_CTL, 0x0);

		codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, ZERO_CROSS_EN, 0x0);
		/*set HPVOL volume*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, 0x0);
	}

	return 0;
}

static int codec_get_earpieceout(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_earpieceout_enabled;
	return 0;
}

/*
 *	codec_phonecap_enabled == 1. open the telephone's record
 *	codec_phonecap_enabled == 0. close the telephone's record
 */
static int codec_set_phonecap(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	codec_phonecap_enabled = ucontrol->value.integer.value[0];

	if (codec_phonecap_enabled) {
		/*enable PHONEP-PHONEN Boost stage*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTEPHONEPN, 0x1);
		/*enable PHONEP-PHONEN Boost stage*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTEPHONEPN, 0x1);
		
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTEROUTPUT, 0x1);
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTELOUTPUT, 0x1);
	} else {
		/*disable PHONEP-PHONEN Boost stage*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTEPHONEPN, 0x0);
		/*disable PHONEP-PHONEN Boost stage*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTEPHONEPN, 0x0);

		codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTEROUTPUT, 0x0);
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTELOUTPUT, 0x0);
	}
	return 0;
}

static int codec_get_phonecap(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_phonecap_enabled;
	return 0;
}

/*
 *	codec_phonein_enabled == 1, the phone in is open.
 *	while you open one of the device(speaker,earpiece,headphone).
 *	you can hear the caller's voice.
 *	codec_phonein_enabled == 0. the phone in is close.
 */
static int codec_set_phonein(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	codec_phonein_enabled = ucontrol->value.integer.value[0];
	if (codec_phonein_enabled) {
		/*select PHONEP-PHONEN*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, RMIXMUTE, 0x10);
		/*select PHONEP-PHONEN*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, LMIXMUTE, 0x10);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LMIXEN, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RMIXEN, 0x1);
	} else {
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, RMIXMUTE, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x7f, LMIXMUTE, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LMIXEN, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RMIXEN, 0x0);
	}
	return 0;
}

static int codec_get_phonein(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_phonein_enabled;
	return 0;
}

/*
 *	codec_phoneout_enabled == 1, the phone out is open. receiver can hear the voice which you say.
 *	codec_phoneout_enabled == 0,	the phone out is close.
 */
static int codec_set_phoneout(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	int cap_vol = 0;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */

	/* type = script_get_item("audio_para", "cap_vol", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* cap_vol = val.val; */

	codec_phoneout_enabled = ucontrol->value.integer.value[0];

	if (codec_phoneout_enabled) {
		/*enable mic1 pa*/
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, MIC1AMPEN, 0x1);
		/*mic1 gain 36dB,if capture volume is too small, enlarge the mic1boost*/
		codec_wr_control(SUN6I_MIC_CTRL, 0x7,MIC1BOOST,cap_vol);
		/*enable Master microphone bias*/
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, MBIASEN, 0x1);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUT_EN, 0x1);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS0, 0x1);
	} else {
		/*disable mic pa*/
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, MIC1AMPEN, 0x0);
		/*disable Master microphone bias*/
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, MBIASEN, 0x0);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUT_EN, 0x0);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS0, 0x0);
	}

	return 0;
}

static int codec_get_phoneout(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_phoneout_enabled;
	return 0;
}

static int codec_dacphoneout_open(void)
{
	/*mute l_pa and r_pa*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x0);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x0);

	/*enable dac digital*/
	codec_wr_control(SUN6I_DAC_DPC, 0x1, DAC_EN, 0x1);
	/*set TX FIFO send drq level*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x7f, TX_TRI_LEVEL, 0xf);
	/*set TX FIFO MODE*/
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, TX_FIFO_MODE, 0x1);

	//send last sample when dac fifo under run
	codec_wr_control(SUN6I_DAC_FIFOC ,0x1, LAST_SE, 0x0);

	/*enable dac_l and dac_r*/
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACALEN, 0x1);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, DACAREN, 0x1);

	codec_wr_control(SUN6I_DAC_ACTL, 0x7f, RMIXMUTE, 0x2);
	codec_wr_control(SUN6I_DAC_ACTL, 0x7f, LMIXMUTE, 0x2);

	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LMIXEN, 0x1);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RMIXEN, 0x1);
	
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS2, 0x1);
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS3, 0x1);
	return 0;
}

/*
 *	codec_dacphoneout_enabled == 1, the dac phone out is open. the receiver can hear the voice from system.
 *	codec_dacphoneout_enabled == 0,	the dac phone out is close.
 */
static int codec_set_dacphoneout(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	codec_dacphoneout_enabled = ucontrol->value.integer.value[0];

	if (codec_dacphoneout_enabled) {
		ret = codec_dacphoneout_open();
	} else {
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS2, 0x0);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, PHONEOUTS3, 0x0);
	}

	return ret;
}

static int codec_get_dacphoneout(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_dacphoneout_enabled;
	return 0;
}

static int codec_adcphonein_open(void)
{
	/*enable PHONEP-PHONEN Boost stage*/
	codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTEPHONEPN, 0x1);
	/*enable PHONEP-PHONEN Boost stage*/
	codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTEPHONEPN, 0x1);
	/*enable adc_r adc_l analog*/
	codec_wr_control(SUN6I_ADC_ACTL, 0x1,  ADCREN, 0x1);
	codec_wr_control(SUN6I_ADC_ACTL, 0x1,  ADCLEN, 0x1);
	/*set RX FIFO mode*/
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1, RX_FIFO_MODE, 0x1);
	/*set RX FIFO rec drq level*/
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1f, RX_TRI_LEVEL, 0xf);
	/*enable adc digital part*/
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1,ADC_EN, 0x1);
	return 0;
}

/*
 *	codec_adcphonein_enabled == 1, the adc phone in is open. you can record the phonein from adc.
 *	codec_adcphonein_enabled == 0,	the adc phone in is close.
 */
static int codec_set_adcphonein(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	codec_adcphonein_enabled = ucontrol->value.integer.value[0];

	if (codec_adcphonein_enabled) {
		ret = codec_adcphonein_open();
	} else {
		/*disable PHONEP-PHONEN Boost stage*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, RADCMIXMUTEPHONEPN, 0x0);
		/*disable PHONEP-PHONEN Boost stage*/
		codec_wr_control(SUN6I_ADC_ACTL, 0x1, LADCMIXMUTEPHONEPN, 0x0);
	}

	return ret;
}

static int codec_get_adcphonein(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_adcphonein_enabled;
	return 0;
}

/*
 *	codec_speaker_enabled == 1, speaker is open, headphone is close.
 *	codec_speaker_enabled == 0, speaker is closed, headphone is open.
 *	this function just used for the system voice(such as music and moive voice and so on),
 *	no the phone call.
 */
static int codec_set_spk(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int headphone_vol = 0x3b;
	/* script_item_u val; */
	/* script_item_value_type_e  type; */
	int headphone_direct_used = 0;
	/* enum sw_ic_ver  codec_chip_ver; */

	/* TODO: Retrieved by FEX, plus some revisions mangling */
	/* codec_chip_ver = sw_get_ic_ver(); */
	/* type = script_get_item("audio_para", "headphone_direct_used", &val); */
	/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
	/* 	printk("[audiocodec] type err!\n"); */
	/* } */
	/* headphone_direct_used = val.val; */

	/* if (headphone_direct_used && (codec_chip_ver != MAGIC_VER_A31A)) { */
	/* 	codec_wr_control(SUN6I_PA_CTRL, 0x3, HPCOM_CTL, 0x3); */
	/* 	codec_wr_control(SUN6I_PA_CTRL, 0x1, HPCOM_PRO, 0x1); */
	/* } else { */
		codec_wr_control(SUN6I_PA_CTRL, 0x3, HPCOM_CTL, 0x0);
		codec_wr_control(SUN6I_PA_CTRL, 0x1, HPCOM_PRO, 0x0);
	/* } */

	codec_speaker_enabled = ucontrol->value.integer.value[0];
	if (codec_speaker_enabled) {
		ret = codec_pa_play_open();
	} else {
		/* item.gpio.data = 0; */
		codec_wr_control(SUN6I_MIC_CTRL, 0x1f, LINEOUT_VOL, 0x0);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_EN, 0x0);
		codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_EN, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPIS, 0x0);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPIS, 0x0);

		/* TODO: GPIOlib */
		/* /\*config gpio info of audio_pa_ctrl close*\/ */
		/* if (0 != sw_gpio_setall_range(&item.gpio, 1)) { */
		/* 	printk("sw_gpio_setall_range failed\n"); */
		/* } */

		/*unmute l_pa and r_pa*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x1);
		codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x1);
		codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, ZERO_CROSS_EN, 0x1);

		/* TODO: Retrieved by FEX */
		/* type = script_get_item("audio_para", "headphone_vol", &val); */
		/* if (SCIRPT_ITEM_VALUE_TYPE_INT != type) { */
		/* 	printk("[audiocodec] type err!\n"); */
		/* } */
		/* headphone_vol = val.val; */

		/*set HPVOL volume*/
		codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, headphone_vol);
	}
	return 0;
}

static int codec_get_spk(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = codec_speaker_enabled;
	return 0;
}

/*
 * 	.info = snd_codec_info_volsw, .get = snd_codec_get_volsw,\.put = snd_codec_put_volsw, 
 */
static const struct snd_kcontrol_new codec_snd_controls[] = {
	/*SUN6I_DAC_ACTL = 0x20,PAVOL*/
	CODEC_SINGLE("Master Playback Volume", SUN6I_DAC_ACTL,0,0x3f,0),			/*0*/
	/*total output switch PAMUTE, if set this bit to 0, the voice is mute*/
	CODEC_SINGLE("Playback LPAMUTE SWITCH", SUN6I_DAC_ACTL,6,0x1,0),			/*1*/
	CODEC_SINGLE("Playback RPAMUTE SWITCH", SUN6I_DAC_ACTL,7,0x1,0),			/*2*/
	CODEC_SINGLE("Left Headphone PA input src select", SUN6I_DAC_ACTL,8,0x1,0),	/*3*/
	CODEC_SINGLE("Right Headphone PA input src select", SUN6I_DAC_ACTL,9,0x1,0),/*4*/
	CODEC_SINGLE("Left output mixer mute control", SUN6I_DAC_ACTL,10,0x7f,0),	/*5*/
	CODEC_SINGLE("Right output mixer mute control", SUN6I_DAC_ACTL,17,0x7f,0),	/*6*/
	CODEC_SINGLE("Left analog output mixer en", SUN6I_DAC_ACTL,28,0x1,0),		/*7*/
	CODEC_SINGLE("Right analog output mixer en", SUN6I_DAC_ACTL,29,0x1,0),		/*8*/
	CODEC_SINGLE("Inter DAC analog left channel en", SUN6I_DAC_ACTL,30,0x1,0),	/*9*/
	CODEC_SINGLE("Inter DAC analog right channel en", SUN6I_DAC_ACTL,31,0x1,0),	/*10*/

	/*SUN6I_PA_CTRL = 0x24*/
	CODEC_SINGLE("r_and_l Headphone Power amplifier en", SUN6I_PA_CTRL,29,0x3,0),		/*11*/
	CODEC_SINGLE("HPCOM output protection en", SUN6I_PA_CTRL,28,0x1,0),					/*12*/
	CODEC_SINGLE("L_to_R Headphone apmplifier output mute", SUN6I_PA_CTRL,25,0x1,0),	/*13*/
	CODEC_SINGLE("R_to_L Headphone apmplifier output mute", SUN6I_PA_CTRL,24,0x1,0),	/*14*/
	CODEC_SINGLE("MIC1_G boost stage output mixer control", SUN6I_PA_CTRL,15,0x7,0),	/*15*/
	CODEC_SINGLE("MIC2_G boost stage output mixer control", SUN6I_PA_CTRL,12,0x7,0),	/*16*/
	CODEC_SINGLE("LINEIN_G boost stage output mixer control", SUN6I_PA_CTRL,9,0x7,0),	/*17*/
	CODEC_SINGLE("PHONE_G boost stage output mixer control", SUN6I_PA_CTRL,6,0x7,0),	/*18*/
	CODEC_SINGLE("PHONE_PG boost stage output mixer control", SUN6I_PA_CTRL,3,0x7,0),	/*19*/
	CODEC_SINGLE("PHONE_NG boost stage output mixer control", SUN6I_PA_CTRL,0,0x7,0),	/*20*/

	/*SUN6I_MIC_CTRL = 0x28*/
	CODEC_SINGLE("Earpiece microphone bias enable", SUN6I_MIC_CTRL,31,0x1,0),			/*21*/
	CODEC_SINGLE("Master microphone bias enable", SUN6I_MIC_CTRL,30,0x1,0),				/*22*/
	CODEC_SINGLE("Earpiece MIC bias_cur_sen and ADC enable", SUN6I_MIC_CTRL,29,0x1,0),	/*23*/
	CODEC_SINGLE("MIC1 boost AMP enable", SUN6I_MIC_CTRL,28,0x1,0),						/*24*/
	CODEC_SINGLE("MIC1 boost AMP gain control", SUN6I_MIC_CTRL,25,0x7,0),				/*25*/
	CODEC_SINGLE("MIC2 boost AMP enable", SUN6I_MIC_CTRL,24,0x1,0),						/*26*/
	CODEC_SINGLE("MIC2 boost AMP gain control", SUN6I_MIC_CTRL,21,0x7,0),				/*27*/
	CODEC_SINGLE("MIC2 source select", SUN6I_MIC_CTRL,20,0x1,0),						/*28*/
	CODEC_SINGLE("Lineout left enable", SUN6I_MIC_CTRL,19,0x1,0),						/*29*/
	CODEC_SINGLE("Lineout right enable", SUN6I_MIC_CTRL,18,0x1,0),						/*30*/
	CODEC_SINGLE("Left lineout source select", SUN6I_MIC_CTRL,17,0x1,0),				/*31*/
	CODEC_SINGLE("Right lineout source select", SUN6I_MIC_CTRL,16,0x1,0),				/*32*/
	CODEC_SINGLE("Lineout volume control", SUN6I_MIC_CTRL,11,0x1f,0),					/*33*/
	CODEC_SINGLE("PHONEP-PHONEN pre-amp gain control", SUN6I_MIC_CTRL,8,0x7,0),			/*34*/
	CODEC_SINGLE("Phoneout gain control", SUN6I_MIC_CTRL,5,0x7,0),						/*35*/
	CODEC_SINGLE("PHONEOUT en", SUN6I_MIC_CTRL,4,0x1,0),								/*36*/
	CODEC_SINGLE("MIC1 boost stage to phone out mute", SUN6I_MIC_CTRL,3,0x1,0),			/*37*/
	CODEC_SINGLE("MIC2 boost stage to phone out mute", SUN6I_MIC_CTRL,2,0x1,0),			/*38*/
	CODEC_SINGLE("Right output mixer to phone out mute", SUN6I_MIC_CTRL,1,0x1,0),		/*39*/
	CODEC_SINGLE("Left output mixer to phone out mute", SUN6I_MIC_CTRL,1,0x1,0),		/*40*/

	/*SUN6I_ADC_ACTL = 0x2c*/
	CODEC_SINGLE("ADC Right channel en", SUN6I_ADC_ACTL,31,0x1,0),						/*41*/
	CODEC_SINGLE("ADC Left channel en", SUN6I_ADC_ACTL,30,0x1,0),						/*42*/
	CODEC_SINGLE("ADC input gain ctrl", SUN6I_ADC_ACTL,27,0x7,0),						/*43*/
	CODEC_SINGLE("Right ADC mixer mute ctrl", SUN6I_ADC_ACTL,7,0x7f,0),					/*44*/
	CODEC_SINGLE("Left ADC mixer mute ctrl", SUN6I_ADC_ACTL,0,0x7f,0),					/*45*/
	/*SUN6I_ADDAC_TUNE = 0x30*/		
	CODEC_SINGLE("ADC dither on_off ctrl", SUN6I_ADDAC_TUNE,25,0x7f,0),					/*46*/
	
	/*SUN6I_HMIC_CTL = 0x50
	 * warning:
	 * the key and headphone should be check in the switch driver,
	 * can't be used in this mixer control.
	 * you should be careful while use the key and headphone check in the mixer control
	 * it may be confilcted with the key and headphone switch driver.
	 */
	CODEC_SINGLE("Hmic_M debounce key down_up", SUN6I_HMIC_CTL,28,0xf,0),				/*47*/
	CODEC_SINGLE("Hmic_N debounce earphone plug in_out", SUN6I_HMIC_CTL,24,0xf,0),		/*48*/
	
	/*SUN6I_DAC_DAP_CTL = 0x60
	 * warning:the DAP should be realize in a DAP driver?
	 * it may be strange using the mixer control to realize the DAP function.
	 */
	CODEC_SINGLE("DAP enable", SUN6I_DAC_DAP_CTL,31,0x1,0),								/*49*/
	CODEC_SINGLE("DAP start control", SUN6I_DAC_DAP_CTL,30,0x1,0),						/*50*/
	CODEC_SINGLE("DAP state", SUN6I_DAC_DAP_CTL,29,0x1,0),								/*51*/
	CODEC_SINGLE("BQ enable control", SUN6I_DAC_DAP_CTL,16,0x1,0),						/*52*/
	CODEC_SINGLE("DRC enable control", SUN6I_DAC_DAP_CTL,15,0x1,0),						/*53*/
	CODEC_SINGLE("HPF enable control", SUN6I_DAC_DAP_CTL,14,0x1,0),						/*54*/
	CODEC_SINGLE("DE function control", SUN6I_DAC_DAP_CTL,12,0x3,0),					/*55*/
	CODEC_SINGLE("Ram address", SUN6I_DAC_DAP_CTL,0,0x7f,0),							/*56*/

	/*SUN6I_DAC_DAP_VOL = 0x64*/
	CODEC_SINGLE("DAP DAC left chan soft mute ctrl", SUN6I_DAC_DAP_VOL,30,0x1,0),		/*57*/
	CODEC_SINGLE("DAP DAC right chan soft mute ctrl", SUN6I_DAC_DAP_VOL,29,0x1,0),		/*58*/
	CODEC_SINGLE("DAP DAC master soft mute ctrl", SUN6I_DAC_DAP_VOL,28,0x1,0),			/*59*/
	CODEC_SINGLE("DAP DAC vol skew time ctrl", SUN6I_DAC_DAP_VOL,24,0x3,0),				/*60*/
	CODEC_SINGLE("DAP DAC master volume", SUN6I_DAC_DAP_VOL,16,0xff,0),					/*61*/
	CODEC_SINGLE("DAP DAC left chan volume", SUN6I_DAC_DAP_VOL,8,0xff,0),				/*62*/
	CODEC_SINGLE("DAP DAC right chan volume", SUN6I_DAC_DAP_VOL,0,0xff,0),				/*63*/
	
	/*SUN6I_ADC_DAP_CTL = 0x70*/
	CODEC_SINGLE("DAP for ADC en", SUN6I_ADC_DAP_CTL,31,0x1,0),							/*64*/
	CODEC_SINGLE("DAP for ADC start up", SUN6I_ADC_DAP_CTL,30,0x1,0),					/*65*/
	CODEC_SINGLE("DAP left AGC saturation flag", SUN6I_ADC_DAP_CTL,21,0x1,0),			/*66*/
	CODEC_SINGLE("DAP left AGC noise-threshold flag", SUN6I_ADC_DAP_CTL,20,0x1,0),		/*67*/
	CODEC_SINGLE("DAP left gain applied by AGC", SUN6I_ADC_DAP_CTL,12,0xff,0),			/*68*/
	CODEC_SINGLE("DAP right AGC saturation flag", SUN6I_ADC_DAP_CTL,9,0x1,0),			/*69*/
	CODEC_SINGLE("DAP right AGC noise-threshold flag", SUN6I_ADC_DAP_CTL,8,0x1,0),		/*70*/
	CODEC_SINGLE("DAP right gain applied by AGC", SUN6I_ADC_DAP_CTL,0,0xff,0),			/*71*/

	/*SUN6I_ADC_DAP_VOL = 0x74*/
	CODEC_SINGLE("DAP ADC left chan vol mute", SUN6I_ADC_DAP_VOL,18,0x1,0),				/*72*/
	CODEC_SINGLE("DAP ADC right chan vol mute", SUN6I_ADC_DAP_VOL,17,0x1,0),			/*73*/
	CODEC_SINGLE("DAP ADC volume skew mute", SUN6I_ADC_DAP_VOL,16,0x1,0),				/*74*/
	CODEC_SINGLE("DAP ADC left chan vol set", SUN6I_ADC_DAP_VOL,8,0x3f,0),				/*75*/
	CODEC_SINGLE("DAP ADC right chan vol set", SUN6I_ADC_DAP_VOL,0,0x3f,0),				/*76*/
	
	/*SUN6I_ADC_DAP_LCTL = 0x78*/
	CODEC_SINGLE("DAP ADC Left chan noise-threshold set", SUN6I_ADC_DAP_VOL,16,0xff,0),	/*77*/
	CODEC_SINGLE("DAP Left AGC en", SUN6I_ADC_DAP_VOL,14,0x1,0),						/*78*/
	CODEC_SINGLE("DAP Left HPF en", SUN6I_ADC_DAP_VOL,13,0x1,0),						/*79*/
	CODEC_SINGLE("DAP Left noise-detect en", SUN6I_ADC_DAP_VOL,12,0x1,0),				/*80*/
	CODEC_SINGLE("DAP Left hysteresis setting", SUN6I_ADC_DAP_VOL,8,0x3,0),				/*81*/
	CODEC_SINGLE("DAP Left noise-debounce time", SUN6I_ADC_DAP_VOL,4,0xf,0),			/*82*/
	CODEC_SINGLE("DAP Left signal-debounce time", SUN6I_ADC_DAP_VOL,0,0xf,0),			/*83*/

	/*SUN6I_ADC_DAP_RCTL = 0x7c*/
	CODEC_SINGLE("DAP ADC right chan noise-threshold set", SUN6I_ADC_DAP_RCTL,0,0xff,0), 	 	/*84*/
	CODEC_SINGLE("DAP Right AGC en", SUN6I_ADC_DAP_VOL,14,0x1,0),						 	 	/*85*/
	CODEC_SINGLE("DAP Right HPF en", SUN6I_ADC_DAP_VOL,13,0x1,0),						 	 	/*86*/
	CODEC_SINGLE("DAP Right noise-detect en", SUN6I_ADC_DAP_VOL,12,0x1,0),				 	 	/*87*/
	CODEC_SINGLE("DAP Right hysteresis setting", SUN6I_ADC_DAP_VOL,8,0x3,0),			 	 	/*88*/
	CODEC_SINGLE("DAP Right noise-debounce time", SUN6I_ADC_DAP_VOL,4,0xf,0),			 	 	/*89*/
	CODEC_SINGLE("DAP Right signal-debounce time", SUN6I_ADC_DAP_VOL,0,0xf,0),			 	 	/*90*/

	SOC_SINGLE_BOOL_EXT("Audio Spk Switch", 0, codec_get_spk, codec_set_spk),			     	/*91*/
	SOC_SINGLE_BOOL_EXT("Audio phone out", 0, codec_get_phoneout, codec_set_phoneout),	 	 	/*92*/
	SOC_SINGLE_BOOL_EXT("Audio phone in", 0, codec_get_phonein, codec_set_phonein),		 	 	/*93*/
	SOC_SINGLE_BOOL_EXT("Audio phone record", 0, codec_get_phonecap, codec_set_phonecap),	 	/*94*/
	SOC_SINGLE_BOOL_EXT("Audio earpiece out", 0, codec_get_earpieceout, codec_set_earpieceout), 	/*95*/
	SOC_SINGLE_BOOL_EXT("Audio headphone out", 0, codec_get_headphoneout, codec_set_headphoneout), /*96*/
	SOC_SINGLE_BOOL_EXT("Audio speaker out", 0, codec_get_speakerout, codec_set_speakerout), 		/*97*/
	
	SOC_SINGLE_BOOL_EXT("Audio adc phonein", 0, codec_get_adcphonein, codec_set_adcphonein), 		/*98*/
	SOC_SINGLE_BOOL_EXT("Audio dac phoneout", 0, codec_get_dacphoneout, codec_set_dacphoneout),    	/*99*/

	SOC_SINGLE_BOOL_EXT("Audio linein record", 0, codec_get_lineincap, codec_set_lineincap), 		/*100*/
	SOC_SINGLE_BOOL_EXT("Audio linein in", 0, codec_get_lineinin, codec_set_lineinin),    			/*101*/
};


static int sun6i_prepare(struct snd_pcm_substream *substream,
			 struct snd_soc_dai *dai)
{
	/* struct dma_config_t play_dma_config; */
	/* struct dma_config_t capture_dma_config; */
	int play_ret = 0, capture_ret = 0;
	unsigned int reg_val;
	struct sun6i_playback_runtime_data *play_prtd = NULL;
	struct sun6i_capture_runtime_data *capture_prtd = NULL;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		switch (substream->runtime->rate) {
		case 44100:
			if (clk_set_rate(codec_pll2clk, 22579200)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 22579200)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29);
			reg_val |=(0<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 22050:
			if (clk_set_rate(codec_pll2clk, 22579200)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 22579200)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(2<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 11025:
			if (clk_set_rate(codec_pll2clk, 22579200)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 22579200)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(4<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 48000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(0<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 96000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29);
			reg_val |=(7<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 192000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29);
			reg_val |=(6<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 32000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(1<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 24000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(2<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 16000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(3<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 12000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(4<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		case 8000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29);
			reg_val |=(5<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		default:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(0<<29);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);		
			break;
		}
		switch (substream->runtime->channels) {
		case 1:
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val |=(1<<6);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);			
			break;
		case 2:
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(1<<6);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		default:
			reg_val = readl(baseaddr + SUN6I_DAC_FIFOC);
			reg_val &=~(1<<6);
			writel(reg_val, baseaddr + SUN6I_DAC_FIFOC);
			break;
		}
	} else {
		switch (substream->runtime->rate) {
		case 44100:
			if (clk_set_rate(codec_pll2clk, 22579200)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 22579200)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(0<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 22050:
			if (clk_set_rate(codec_pll2clk, 22579200)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 22579200)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(2<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 11025:
			if (clk_set_rate(codec_pll2clk, 22579200)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 22579200)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(4<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 48000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(0<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 32000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(1<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 24000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(2<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 16000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(3<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 12000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(4<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		case 8000:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(5<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		default:
			if (clk_set_rate(codec_pll2clk, 24576000)) {
				printk("set codec_pll2clk rate fail\n");
			}
			if (clk_set_rate(codec_moduleclk, 24576000)) {
				printk("set codec_moduleclk rate fail\n");
			}
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(7<<29); 
			reg_val |=(0<<29);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);		
			break;
		}
		switch (substream->runtime->channels) {
		case 1:
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val |=(1<<7);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);			
			break;
		case 2:
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(1<<7);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		default:
			reg_val = readl(baseaddr + SUN6I_ADC_FIFOC);
			reg_val &=~(1<<7);
			writel(reg_val, baseaddr + SUN6I_ADC_FIFOC);
			break;
		}
	}
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK){
   	 	play_prtd = substream->runtime->private_data;
   	 	/* return if this is a bufferless transfer e.g.
		 * codec <--> BT codec or GSM modem -- lg FIXME */       
   	 	if (!play_prtd->params) {
			return 0;
		}
   	 	/*open the dac channel register*/
		if (codec_speaker_enabled) {
			play_ret = codec_pa_play_open();
		} else if (codec_dacphoneout_enabled) {
			play_ret = codec_dacphoneout_open();
		} else {
			play_ret = codec_headphone_play_open();
		}
		/* memset(&play_dma_config, 0, sizeof(play_dma_config)); */
		/* play_dma_config.xfer_type = DMAXFER_D_BHALF_S_BHALF; */
		/* play_dma_config.address_type = DMAADDRT_D_IO_S_LN; */
		/* play_dma_config.para = 0; */
		/* play_dma_config.irq_spt = CHAN_IRQ_QD; */
		/* play_dma_config.src_addr = play_prtd->dma_start; */
		/* play_dma_config.dst_addr = play_prtd->params->dma_addr; */
		/* play_dma_config.byte_cnt = play_prtd->dma_period; */
		/* play_dma_config.bconti_mode = false; */
		/* play_dma_config.src_drq_type = DRQSRC_SDRAM; */
		/* play_dma_config.dst_drq_type = DRQDST_AUDIO_CODEC; */
		/* if (0 != sw_dma_config(play_prtd->dma_hdl, &play_dma_config, ENQUE_PHASE_NORMAL)) { */
		/* 	return -EINVAL; */
		/* } */

		play_prtd->dma_loaded = 0;
		play_prtd->dma_pos = play_prtd->dma_start;
		play_prtd->play_dma_flag = false;
		/* enqueue dma buffers */
		/* sun6i_pcm_enqueue(substream); */
		return play_ret;
	} else {
		capture_prtd = substream->runtime->private_data;                          
   	 	/* return if this is a bufferless transfer e.g.
	  	 * codec <--> BT codec or GSM modem -- lg FIXME */
   	 	if (!capture_prtd->params) {
			return 0;
		}
	   	/*open the adc channel register*/
	   	if (codec_adcphonein_enabled) {
	   		codec_adcphonein_open();
		} else {
	   		codec_capture_open();
		}
		/* memset(&capture_dma_config, 0, sizeof(capture_dma_config)); */
		/* capture_dma_config.xfer_type = DMAXFER_D_BHALF_S_BHALF;/\*16bit*\/ */
		/* capture_dma_config.address_type = DMAADDRT_D_LN_S_IO; */
		/* capture_dma_config.para = 0; */
		/* capture_dma_config.irq_spt = CHAN_IRQ_QD; */
		/* capture_dma_config.src_addr = capture_prtd->params->dma_addr; */
		/* capture_dma_config.dst_addr = capture_prtd->dma_start; */
		/* capture_dma_config.byte_cnt = capture_prtd->dma_period; */
		/* capture_dma_config.bconti_mode = false; */
		/* capture_dma_config.src_drq_type = DRQSRC_AUDIO_CODEC; */
		/* capture_dma_config.dst_drq_type = DRQDST_SDRAM; */

		/* if (0 != sw_dma_config(capture_prtd->dma_hdl, &capture_dma_config, ENQUE_PHASE_NORMAL)) { */
		/* 	return -EINVAL; */
		/* } */

		capture_prtd->dma_loaded = 0;
		capture_prtd->dma_pos = capture_prtd->dma_start;
		capture_prtd->capture_dma_flag = false;
		/* enqueue dma buffers */
		/* sun6i_pcm_enqueue(substream); */
		return capture_ret;
	}
}

static int sun6i_trigger(struct snd_pcm_substream *substream, int cmd,
			 struct snd_soc_dai *dai)
{
	int play_ret = 0, capture_ret = 0;
	struct sun6i_playback_runtime_data *play_prtd = NULL;
	struct sun6i_capture_runtime_data *capture_prtd = NULL;

	if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK){
		play_prtd = substream->runtime->private_data;
		spin_lock(&play_prtd->lock);
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			play_prtd->state |= ST_RUNNING;
			codec_play_start();
			/*
			 * start dma transfer
			 */
			/* if (0 != sw_dma_ctl(play_prtd->dma_hdl, DMA_OP_START, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			if (codec_speaker_enabled) {
			} else {
				/*set the default output is HPOUTL/R for pad headphone*/
				codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x1);
				codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x1);
			}
			break;
		case SNDRV_PCM_TRIGGER_SUSPEND:
			codec_play_stop();
			/*
			 * stop play dma transfer
			 */
			/* if (0 != sw_dma_ctl(play_prtd->dma_hdl, DMA_OP_STOP, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			break;
		case SNDRV_PCM_TRIGGER_STOP:
			play_prtd->state &= ~ST_RUNNING;
			codec_play_stop();
			/*
			 * stop play dma transfer
			 */
			/* if (0 != sw_dma_ctl(play_prtd->dma_hdl, DMA_OP_STOP, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			break;
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:							
			play_prtd->state &= ~ST_RUNNING;
			/*
			 * stop play dma transfer
			 */
			/* if (0 != sw_dma_ctl(play_prtd->dma_hdl, DMA_OP_STOP, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			break;
		default:
			printk("error:%s,%d\n", __func__, __LINE__);
			play_ret = -EINVAL;
			break;
		}
		spin_unlock(&play_prtd->lock);
	}else{
		capture_prtd = substream->runtime->private_data;
		spin_lock(&capture_prtd->lock);
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			capture_prtd->state |= ST_RUNNING;	 
			codec_capture_start();
			/*hardware fifo delay*/
			mdelay(200);
			codec_wr_control(SUN6I_ADC_FIFOC, 0x1, ADC_FIFO_FLUSH, 0x1);
			/*
			 * start dma transfer
			 */
			/* if (0 != sw_dma_ctl(capture_prtd->dma_hdl, DMA_OP_START, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			break;
		case SNDRV_PCM_TRIGGER_SUSPEND:
			codec_capture_stop();
			/*
			 * stop capture dma transfer
			 */
			/* if (0 != sw_dma_ctl(capture_prtd->dma_hdl, DMA_OP_STOP, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			break;
		case SNDRV_PCM_TRIGGER_STOP:		 
			capture_prtd->state &= ~ST_RUNNING;
			codec_capture_stop();
			/*
			 * stop capture dma transfer
			 */
			/* if (0 != sw_dma_ctl(capture_prtd->dma_hdl, DMA_OP_STOP, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			break;
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:		
			capture_prtd->state &= ~ST_RUNNING;
			/*
			 * stop capture dma transfer
			 */
			/* if (0 != sw_dma_ctl(capture_prtd->dma_hdl, DMA_OP_STOP, NULL)) { */
			/* 	return -EINVAL; */
			/* } */
			break;
		default:
			printk("error:%s,%d\n", __func__, __LINE__);
			capture_ret = -EINVAL;
			break;
		}
		spin_unlock(&capture_prtd->lock);
	}
	return 0;
}

static int sun6i_set_sysclk(struct snd_soc_dai *dai,
			    int clk_id, unsigned int freq, int dir)
{
	return 0;
}
static int sun6i_set_pll(struct snd_soc_dai *dai, int pll_id, int source,
			 unsigned int freq_in, unsigned int freq_out)
{
	return 0;
}

static int sun6i_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	return 0;
}

static int sun6i_digital_mute(struct snd_soc_dai *dai, int mute)
{
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, !!mute);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, !!mute);

	return 0;
}

static const struct snd_soc_dai_ops sun6i_dai_ops = {
	.set_sysclk		= sun6i_set_sysclk,
	.set_pll		= sun6i_set_pll,
	.set_fmt		= sun6i_set_fmt,
	.digital_mute		= sun6i_digital_mute,
	.prepare		= sun6i_prepare,
	.trigger		= sun6i_trigger,
};

static struct snd_soc_dai_driver sun6i_dai[] = {
{
	.name = "sun6i-codec",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE, },
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE, },
	.ops = &sun6i_dai_ops,
},
};

static int sun6i_soc_probe(struct snd_soc_codec *codec)
{
	/* HPCOMM is off and output is floating (WTF?!) */
	codec_wr_control(SUN6I_PA_CTRL, 0x3, HPCOM_CTL, 0x0);
	/* Disable headphone output */
	codec_wr_control(SUN6I_PA_CTRL, 0x1, HPCOM_PRO, 0x0);

	/*
	 * Enable Headset MIC Bias Current sensor & ADC
	 * Due to an hardware bug, it seems to be only possible at init
	 */
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, HBIASADCEN, 0x1);

	/*
	 * Mute Playback Left and Right channels
	 * Also disables the associated mixer and DAC
	 */
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, LHPPA_MUTE, 0x0);
	codec_wr_control(SUN6I_DAC_ACTL, 0x1, RHPPA_MUTE, 0x0);

	/* Disable Playback Lineouts */ 
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTL_EN, 0x0);
	codec_wr_control(SUN6I_MIC_CTRL, 0x1, LINEOUTR_EN, 0x0);

	/*
	 * Fix the init blaze noise
	 * Really have to find more details about that
	 */
	codec_wr_control(SUN6I_ADDAC_TUNE, 0x1, PA_SLOPE_SECECT, 0x1);

	/* Enable Power Amplifier for both headphone channels */
	codec_wr_control(SUN6I_PA_CTRL, 0x1, HPPAEN, 0x1);

	/* set HPCOM control as direct driver for floating (Redundant?) */
	codec_wr_control(SUN6I_PA_CTRL, 0x3, HPCOM_CTL, 0x0);

	/*
	 * Stop doing DMA requests whenever there's only 16 samples
	 * left available in the TX FIFO.
	 */
	codec_wr_control(SUN6I_DAC_FIFOC, 0x3, DRA_LEVEL,0x3);

	/* Flush TX FIFO */
	codec_wr_control(SUN6I_DAC_FIFOC, 0x1, DAC_FIFO_FLUSH, 0x1);

	/* Flush RX FIFO */
	codec_wr_control(SUN6I_ADC_FIFOC, 0x1, ADC_FIFO_FLUSH, 0x1);

	/* Use a 32 bits FIR */
	codec_wr_control(SUN6I_DAC_FIFOC, 0x1, FIR_VERSION, 0x1);

	/* Set HPVOL default volume */
	codec_wr_control(SUN6I_DAC_ACTL, 0x3f, VOLUME, 0x3b);

	return 0;
}

static int sun6i_soc_remove(struct snd_soc_codec *codec)
{
	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_sun6i = {
	.probe		= sun6i_soc_probe,
	.remove		= sun6i_soc_remove,

	.controls	= codec_snd_controls,
	.num_controls	= ARRAY_SIZE(codec_snd_controls),
};

static int sun6i_codec_probe(struct platform_device *pdev)
{
	struct sun6i_priv *sun6i;
	struct resource *res;
	struct clk *pll2;
	int ret;

	sun6i = devm_kzalloc(&pdev->dev, sizeof(struct sun6i_priv), GFP_KERNEL);
	if (!sun6i)
		return -ENOMEM;

	platform_set_drvdata(pdev, sun6i);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sun6i->base = devm_ioremap_resource(&pdev->dev, res);
	if (!sun6i->base)
		return PTR_ERR(sun6i->base);

	sun6i->apb_clk = devm_clk_get(&pdev->dev, "apb");
	if (IS_ERR(sun6i->apb_clk)) {
		dev_err(&pdev->dev, "Couldn't get the APB clock\n");
		return PTR_ERR(sun6i->apb_clk);
	}

	sun6i->mod_clk = devm_clk_get(&pdev->dev, "mod");
	if (IS_ERR(sun6i->mod_clk)) {
		dev_err(&pdev->dev, "Couldn't get the module clock\n");
		return PTR_ERR(sun6i->mod_clk);
	}

	pll2 = clk_get(&pdev->dev, "pll2");
	if (IS_ERR(pll2)) {
		dev_err(&pdev->dev, "Couldn't get the PLL2 clock\n");
		return PTR_ERR(pll2);
	}

	ret = clk_set_parent(sun6i->mod_clk, pll2);
	if (ret) {
		clk_put(pll2);
		dev_err(&pdev->dev, "Couldn't reparent module clock\n");
		return ret;
	}
	clk_put(pll2);

	ret = clk_set_rate(sun6i->mod_clk, 24576000);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't change the module clock rate\n");
		return ret;
	}

	sun6i->rstc = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(sun6i->rstc)) {
		dev_err(&pdev->dev, "Couldn't get the reset controller\n");
		return PTR_ERR(sun6i->rstc);
	}

	sun6i->pa_gpio = of_get_named_gpio(pdev->dev.of_node, "pa-gpio", 0);
	if (!gpio_is_valid(sun6i->pa_gpio)) {
		dev_err(&pdev->dev, "Couldn't get the PA GPIO\n");
		return sun6i->pa_gpio;
	}

	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_sun6i,
				      sun6i_dai, ARRAY_SIZE(sun6i_dai));
}

static int sun6i_codec_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);

	return 0;
}

static const struct of_device_id sun6i_codec_match[] = {
	{ .compatible = "allwinner,sun6i-a31-audio-codec", },
	{}
};
MODULE_DEVICE_TABLE(of, sun6i_codec_match);


static struct platform_driver sun6i_codec_driver = {
	.probe		= sun6i_codec_probe,
	.remove		= sun6i_codec_remove,
	.driver		= {
		.name	= "sun6i-codec",
		.of_match_table = sun6i_codec_match,
	},
};

module_platform_driver(sun6i_codec_driver);

MODULE_DESCRIPTION("sun6i CODEC ALSA codec driver");
MODULE_AUTHOR("huangxin");
MODULE_LICENSE("GPL");
