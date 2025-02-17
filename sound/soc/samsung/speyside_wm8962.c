/*
 * Speyside with WM8962 audio support
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
#include <linux/module.h>

#include "../codecs/wm8962.h"

static int sample_rate = 44100;

static int speyside_wm8962_set_bias_level(struct snd_soc_card *card,
					  struct snd_soc_dapm_context *dapm,
					  enum snd_soc_bias_level level)
{
	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dai;
	int ret;

	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level == SND_SOC_BIAS_STANDBY) {
			ret = snd_soc_dai_set_pll(codec_dai, WM8962_FLL,
						  WM8962_FLL_MCLK, 32768,
						  sample_rate * 512);
			if (ret < 0)
				pr_err("Failed to start FLL: %d\n", ret);

			ret = snd_soc_dai_set_sysclk(codec_dai,
						     WM8962_SYSCLK_FLL,
						     sample_rate * 512,
						     SND_SOC_CLOCK_IN);
			if (ret < 0) {
				pr_err("Failed to set SYSCLK: %d\n", ret);
				return ret;
			}
		}
		break;

	default:
		break;
	}

	return 0;
}

static int speyside_wm8962_set_bias_level_post(struct snd_soc_card *card,
					       struct snd_soc_dapm_context *dapm,
					       enum snd_soc_bias_level level)
{
	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dai;
	int ret;

	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_MCLK,
					     32768, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			pr_err("Failed to switch away from FLL: %d\n", ret);
			return ret;
		}

		ret = snd_soc_dai_set_pll(codec_dai, WM8962_FLL,
					  0, 0, 0);
		if (ret < 0) {
			pr_err("Failed to stop FLL: %d\n", ret);
			return ret;
		}
		break;

	default:
		break;
	}

	dapm->bias_level = level;

	return 0;
}

static int speyside_wm8962_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	sample_rate = params_rate(params);

	return 0;
}

static struct snd_soc_ops speyside_wm8962_ops = {
	.hw_params = speyside_wm8962_hw_params,
};

static struct snd_soc_dai_link speyside_wm8962_dai[] = {
	{
		.name = "CPU",
		.stream_name = "CPU",
		.cpu_dai_name = "samsung-i2s.0",
		.codec_dai_name = "wm8962",
		.platform_name = "samsung-audio",
		.codec_name = "wm8962.1-001a",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
				| SND_SOC_DAIFMT_CBM_CFM,
		.ops = &speyside_wm8962_ops,
	},
};

static const struct snd_kcontrol_new controls[] = {
	SOC_DAPM_PIN_SWITCH("Main Speaker"),
	SOC_DAPM_PIN_SWITCH("DMIC"),
};

static struct snd_soc_dapm_widget widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),

	SND_SOC_DAPM_MIC("DMIC", NULL),
	SND_SOC_DAPM_MIC("AMIC", NULL),

	SND_SOC_DAPM_SPK("Main Speaker", NULL),
};

static struct snd_soc_dapm_route audio_paths[] = {
	{ "Headphone", NULL, "HPOUTL" },
	{ "Headphone", NULL, "HPOUTR" },

	{ "Main Speaker", NULL, "SPKOUTL" },
	{ "Main Speaker", NULL, "SPKOUTR" },

	{ "Headset Mic", NULL, "MICBIAS" },
	{ "IN4L", NULL, "Headset Mic" },
	{ "IN4R", NULL, "Headset Mic" },

	{ "AMIC", NULL, "MICBIAS" },
	{ "IN1L", NULL, "AMIC" },
	{ "IN1R", NULL, "AMIC" },

	{ "DMIC", NULL, "MICBIAS" },
	{ "DMICDAT", NULL, "DMIC" },
};

static struct snd_soc_jack speyside_wm8962_headset;

/* Headset jack detection DAPM pins */
static struct snd_soc_jack_pin speyside_wm8962_headset_pins[] = {
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headphone",
		.mask = SND_JACK_MICROPHONE,
	},
};

static int speyside_wm8962_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_codec *codec = card->rtd[0].codec;
	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dai;
	int ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_MCLK,
				     32768, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	ret = snd_soc_jack_new(codec, "Headset",
			       SND_JACK_HEADSET | SND_JACK_BTN_0,
			       &speyside_wm8962_headset);
	if (ret)
		return ret;

	ret = snd_soc_jack_add_pins(&speyside_wm8962_headset,
				    ARRAY_SIZE(speyside_wm8962_headset_pins),
				    speyside_wm8962_headset_pins);
	if (ret)
		return ret;

	wm8962_mic_detect(codec, &speyside_wm8962_headset);

	return 0;
}

static struct snd_soc_card speyside_wm8962 = {
	.name = "Speyside WM8962",
	.dai_link = speyside_wm8962_dai,
	.num_links = ARRAY_SIZE(speyside_wm8962_dai),

	.set_bias_level = speyside_wm8962_set_bias_level,
	.set_bias_level_post = speyside_wm8962_set_bias_level_post,

	.controls = controls,
	.num_controls = ARRAY_SIZE(controls),
	.dapm_widgets = widgets,
	.num_dapm_widgets = ARRAY_SIZE(widgets),
	.dapm_routes = audio_paths,
	.num_dapm_routes = ARRAY_SIZE(audio_paths),

	.late_probe = speyside_wm8962_late_probe,
};

static int speyside_wm8962_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &speyside_wm8962;
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

static int speyside_wm8962_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	snd_soc_unregister_card(card);

	return 0;
}

static struct platform_driver speyside_wm8962_driver = {
	.driver = {
		.name = "speyside-wm8962",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = speyside_wm8962_probe,
	.remove = speyside_wm8962_remove,
};

static int __init speyside_wm8962_audio_init(void)
{
	return platform_driver_register(&speyside_wm8962_driver);
}
module_init(speyside_wm8962_audio_init);

static void __exit speyside_wm8962_audio_exit(void)
{
	platform_driver_unregister(&speyside_wm8962_driver);
}
module_exit(speyside_wm8962_audio_exit);

MODULE_DESCRIPTION("Speyside WM8962 audio support");
MODULE_AUTHOR("Mark Brown <broonie@opensource.wolfsonmicro.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:speyside-wm8962");
