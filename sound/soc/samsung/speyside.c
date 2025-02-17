/*
 * Speyside audio support
 *
 * Copyright 2011 Wolfson Microelectronics
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/jack.h>
#include <linux/gpio.h>

#include "../codecs/wm8915.h"
#include "../codecs/wm9081.h"

#define WM8915_HPSEL_GPIO 214

static int speyside_set_bias_level(struct snd_soc_card *card,
				   enum snd_soc_bias_level level)
{
	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dai;
	int ret;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		ret = snd_soc_dai_set_sysclk(codec_dai, WM8915_SYSCLK_MCLK1,
					     32768, SND_SOC_CLOCK_IN);
		if (ret < 0)
			return ret;

		ret = snd_soc_dai_set_pll(codec_dai, WM8915_FLL_MCLK1,
					  0, 0, 0);
		if (ret < 0) {
			pr_err("Failed to stop FLL\n");
			return ret;
		}

	default:
		break;
	}

	return 0;
}

static int speyside_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S
					 | SND_SOC_DAIFMT_NB_NF
					 | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S
					 | SND_SOC_DAIFMT_NB_NF
					 | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_pll(codec_dai, 0, WM8915_FLL_MCLK1,
				  32768, 256 * 48000);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8915_SYSCLK_FLL,
				     256 * 48000, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	return 0;
}

static struct snd_soc_ops speyside_ops = {
	.hw_params = speyside_hw_params,
};

static struct snd_soc_jack speyside_headset;

/* Headset jack detection DAPM pins */
static struct snd_soc_jack_pin speyside_headset_pins[] = {
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headphone",
		.mask = SND_JACK_HEADPHONE,
	},
};

/* Default the headphone selection to active high */
static int speyside_jack_polarity;

static int speyside_get_micbias(struct snd_soc_dapm_widget *source,
				struct snd_soc_dapm_widget *sink)
{
	if (speyside_jack_polarity && (strcmp(source->name, "MICB1") == 0))
		return 1;
	if (!speyside_jack_polarity && (strcmp(source->name, "MICB2") == 0))
		return 1;

	return 0;
}

static void speyside_set_polarity(struct snd_soc_codec *codec,
				  int polarity)
{
	speyside_jack_polarity = !polarity;
	gpio_direction_output(WM8915_HPSEL_GPIO, speyside_jack_polarity);

	/* Re-run DAPM to make sure we're using the correct mic bias */
	snd_soc_dapm_sync(&codec->dapm);
}

static int speyside_wm8915_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *dai = rtd->codec_dai;
	struct snd_soc_codec *codec = rtd->codec;
	int ret;

	ret = snd_soc_dai_set_sysclk(dai, WM8915_SYSCLK_MCLK1, 32768, 0);
	if (ret < 0)
		return ret;

	ret = gpio_request(WM8915_HPSEL_GPIO, "HP_SEL");
	if (ret != 0)
		pr_err("Failed to request HP_SEL GPIO: %d\n", ret);
	gpio_direction_output(WM8915_HPSEL_GPIO, speyside_jack_polarity);

	ret = snd_soc_jack_new(codec, "Headset",
			       SND_JACK_HEADSET | SND_JACK_BTN_0,
			       &speyside_headset);
	if (ret)
		return ret;

	ret = snd_soc_jack_add_pins(&speyside_headset,
				    ARRAY_SIZE(speyside_headset_pins),
				    speyside_headset_pins);
	if (ret)
		return ret;

	wm8915_detect(codec, &speyside_headset, speyside_set_polarity);

	return 0;
}

static int speyside_late_probe(struct snd_soc_card *card)
{
	snd_soc_dapm_ignore_suspend(&card->dapm, "Headphone");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Headset Mic");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Main AMIC");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Main DMIC");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Speaker");
	snd_soc_dapm_ignore_suspend(&card->dapm, "WM1250 Output");
	snd_soc_dapm_ignore_suspend(&card->dapm, "WM1250 Input");

	return 0;
}

static struct snd_soc_dai_link speyside_dai[] = {
	{
		.name = "CPU",
		.stream_name = "CPU",
		.cpu_dai_name = "samsung-i2s.0",
		.codec_dai_name = "wm8915-aif1",
		.platform_name = "samsung-audio",
		.codec_name = "wm8915.1-001a",
		.init = speyside_wm8915_init,
		.ops = &speyside_ops,
	},
	{
		.name = "Baseband",
		.stream_name = "Baseband",
		.cpu_dai_name = "wm8915-aif2",
		.codec_dai_name = "wm1250-ev1",
		.codec_name = "wm1250-ev1.1-0027",
		.ops = &speyside_ops,
		.ignore_suspend = 1,
	},
};

static int speyside_wm9081_init(struct snd_soc_dapm_context *dapm)
{
	snd_soc_dapm_nc_pin(dapm, "LINEOUT");

	/* At any time the WM9081 is active it will have this clock */
	return snd_soc_codec_set_sysclk(dapm->codec, WM9081_SYSCLK_MCLK,
					48000 * 256, 0);
}

static struct snd_soc_aux_dev speyside_aux_dev[] = {
	{
		.name = "wm9081",
		.codec_name = "wm9081.1-006c",
		.init = speyside_wm9081_init,
	},
};

static struct snd_soc_codec_conf speyside_codec_conf[] = {
	{
		.dev_name = "wm9081.1-006c",
		.name_prefix = "Sub",
	},
};

static const struct snd_kcontrol_new controls[] = {
	SOC_DAPM_PIN_SWITCH("Main Speaker"),
	SOC_DAPM_PIN_SWITCH("Main DMIC"),
	SOC_DAPM_PIN_SWITCH("Main AMIC"),
	SOC_DAPM_PIN_SWITCH("WM1250 Input"),
	SOC_DAPM_PIN_SWITCH("WM1250 Output"),
};

static struct snd_soc_dapm_widget widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),

	SND_SOC_DAPM_SPK("Main Speaker", NULL),

	SND_SOC_DAPM_MIC("Main AMIC", NULL),
	SND_SOC_DAPM_MIC("Main DMIC", NULL),
};

static struct snd_soc_dapm_route audio_paths[] = {
	{ "IN1RN", NULL, "MICB1" },
	{ "IN1RP", NULL, "MICB1" },
	{ "IN1RN", NULL, "MICB2" },
	{ "IN1RP", NULL, "MICB2" },
	{ "MICB1", NULL, "Headset Mic", speyside_get_micbias },
	{ "MICB2", NULL, "Headset Mic", speyside_get_micbias },

	{ "IN1LP", NULL, "MICB2" },
	{ "IN1RN", NULL, "MICB1" },
	{ "MICB2", NULL, "Main AMIC" },

	{ "DMIC1DAT", NULL, "MICB1" },
	{ "DMIC2DAT", NULL, "MICB1" },
	{ "MICB1", NULL, "Main DMIC" },

	{ "Headphone", NULL, "HPOUT1L" },
	{ "Headphone", NULL, "HPOUT1R" },

	{ "Sub IN1", NULL, "HPOUT2L" },
	{ "Sub IN2", NULL, "HPOUT2R" },

	{ "Main Speaker", NULL, "Sub SPKN" },
	{ "Main Speaker", NULL, "Sub SPKP" },
	{ "Main Speaker", NULL, "SPKDAT" },
};

static struct snd_soc_card speyside = {
	.name = "Speyside",
	.dai_link = speyside_dai,
	.num_links = ARRAY_SIZE(speyside_dai),
	.aux_dev = speyside_aux_dev,
	.num_aux_devs = ARRAY_SIZE(speyside_aux_dev),
	.codec_conf = speyside_codec_conf,
	.num_configs = ARRAY_SIZE(speyside_codec_conf),

	.set_bias_level = speyside_set_bias_level,

	.controls = controls,
	.num_controls = ARRAY_SIZE(controls),
	.dapm_widgets = widgets,
	.num_dapm_widgets = ARRAY_SIZE(widgets),
	.dapm_routes = audio_paths,
	.num_dapm_routes = ARRAY_SIZE(audio_paths),

	.late_probe = speyside_late_probe,
};

static int speyside_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &speyside;
	int ret;

	card->dev = &pdev->dev;

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static int speyside_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	snd_soc_unregister_card(card);

	return 0;
}

static struct platform_driver speyside_driver = {
	.driver = {
		.name = "speyside",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = speyside_probe,
	.remove = speyside_remove,
};

static int __init speyside_audio_init(void)
{
	return platform_driver_register(&speyside_driver);
}
module_init(speyside_audio_init);

static void __exit speyside_audio_exit(void)
{
	platform_driver_unregister(&speyside_driver);
}
module_exit(speyside_audio_exit);

MODULE_DESCRIPTION("Speyside audio support");
MODULE_AUTHOR("Mark Brown <broonie@opensource.wolfsonmicro.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:speyside");
