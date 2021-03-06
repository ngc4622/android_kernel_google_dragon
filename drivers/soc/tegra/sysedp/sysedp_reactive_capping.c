/*
 * Copyright (c) 2013-2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <soc/tegra/sysedp.h>

#define STATE_MAX_MW	20000
#define STATE_STEP_MW	500
#define NSTATES		(STATE_MAX_MW / STATE_STEP_MW + 1)

static unsigned int capping_states[NSTATES];

inline unsigned int count_state(int mw)
{
	int state;
	state = mw > 0 ? mw / STATE_STEP_MW + 1 : 0;
	return min(state, NSTATES - 1);
}

static void oc_throttle_alarm(struct sysedp_reactive_capping_platform_data *h)
{
	mutex_lock(&h->mutex);

	h->cur_capping_mw += h->step_alarm_mw;
	h->cur_capping_mw = min(h->cur_capping_mw, h->max_capping_mw);

	cancel_delayed_work(&h->work);

	sysedp_set_state(&h->sysedpc, count_state(h->cur_capping_mw));

	schedule_delayed_work(&h->work, msecs_to_jiffies(h->relax_ms));

	mutex_unlock(&h->mutex);
}

static void oc_throttle_work(struct work_struct *work)
{
	struct sysedp_reactive_capping_platform_data *h;
	h = container_of(to_delayed_work(work),
			 struct sysedp_reactive_capping_platform_data,
			 work);
	mutex_lock(&h->mutex);
	h->cur_capping_mw -= h->step_relax_mw;
	h->cur_capping_mw = max(h->cur_capping_mw, 0);

	sysedp_set_state(&h->sysedpc, count_state(h->cur_capping_mw));

	if (h->cur_capping_mw)
		schedule_delayed_work(&h->work, msecs_to_jiffies(h->relax_ms));

	mutex_unlock(&h->mutex);
}

static irqreturn_t sysedp_reactive_capping_irq_handler(int irq, void *data)
{
	if (!data)
		return IRQ_NONE;

	oc_throttle_alarm(data);
	return IRQ_HANDLED;
}

static int of_sysedp_reactive_capping_get_pdata(struct platform_device *pdev,
		struct sysedp_reactive_capping_platform_data **pdata)
{
	struct device_node *np = pdev->dev.of_node;
	struct sysedp_reactive_capping_platform_data *obj_ptr;
	int i;
	u32 lenp, val, irq_flags;
	const char *c_ptr;
	const void *ptr;
	int ret;
	int max_capping_mw, step_alarm_mw, step_relax_mw, relax_ms;

	struct {
		const char *name;
		int  *var_ptr;
	} srcintlist[] = {
		{"nvidia,max-capping-mw", &max_capping_mw},
		{"nvidia,step-alarm-mw", &step_alarm_mw},
		{"nvidia,step-relax-mw", &step_relax_mw},
		{"nvidia,relax-ms", &relax_ms},
	};

	*pdata = NULL;

	obj_ptr = devm_kzalloc(&pdev->dev,
		      sizeof(struct sysedp_reactive_capping_platform_data),
		      GFP_KERNEL);
	if (!obj_ptr)
		return -ENOMEM;

	ptr = of_get_property(np, "sysedpc,name", &lenp);
	if (!ptr) {
		dev_err(&pdev->dev, "Fail to read sysedpc,name\n");
		return -EINVAL;
	}
	ret = of_property_read_string(np, "sysedpc,name", &c_ptr);
	if (ret) {
		dev_err(&pdev->dev,
			"The sysedpc,name entry is not set!\n");
		return -EINVAL;
	}
	strlcpy(obj_ptr->sysedpc.name, c_ptr, SYSEDP_NAME_LEN);

	for (i = 0; i < ARRAY_SIZE(srcintlist); i++) {
		ret = of_property_read_u32(np, srcintlist[i].name, &val);
		if (ret) {
			dev_err(&pdev->dev,
				"The device node, %s, failed to read \"%s\"!\n",
				obj_ptr->sysedpc.name, srcintlist[i].name);
			return -EINVAL;
		}
		*srcintlist[i].var_ptr = (int)val;
	}

	obj_ptr->max_capping_mw = max_capping_mw;
	obj_ptr->step_alarm_mw = step_alarm_mw;
	obj_ptr->step_relax_mw = step_relax_mw;
	obj_ptr->relax_ms = relax_ms;

	/* Only interrupt at index 0 is expected per reactive capping node. */
	obj_ptr->irq = irq_of_parse_and_map(np, 0);
	if (obj_ptr->irq == 0) {
		dev_err(&pdev->dev,
			"The device node, %s, failed to map interrupts!\n",
			obj_ptr->sysedpc.name);
		return -EINVAL;
	}

	/* Parse index 1, irq flags, directly from the interrupts array */
	ret = of_property_read_u32_index(np, "interrupts", 1, &irq_flags);
	if (ret) {
		dev_err(&pdev->dev,
			"The device node, %s, failed to get the irq flags!\n",
			obj_ptr->sysedpc.name);
		return -EINVAL;
	}
	obj_ptr->irq_flags = (int)irq_flags;

	*pdata = obj_ptr;

	return 0;
}


static int sysedp_reactive_capping_probe(struct platform_device *pdev)
{
	int ret, i;
	struct sysedp_reactive_capping_platform_data *pdata = NULL;

	if (pdev->dev.of_node) {
		ret = of_sysedp_reactive_capping_get_pdata(pdev, &pdata);
		if (ret)
			return ret;
	} else
		pdata = pdev->dev.platform_data;

	if (!pdata)
		return -EINVAL;

	/* update static capping_states table */
	for (i = 0; i < NSTATES; i++)
		capping_states[i] = i * STATE_STEP_MW;

	/* sysedpc consumer name must be initialized */
	if (pdata->sysedpc.name[0] == '\0')
		return -EINVAL;
	pdata->sysedpc.states = capping_states;
	pdata->sysedpc.num_states = ARRAY_SIZE(capping_states);
	ret = sysedp_register_consumer(&pdata->sysedpc);
	if (ret) {
		pr_err("sysedp_reactive_capping_probe: consumer register failed (%d)\n",
		       ret);
		return ret;
	}
	mutex_init(&pdata->mutex);
	INIT_DELAYED_WORK(&pdata->work, oc_throttle_work);

	ret = request_threaded_irq(pdata->irq,
				   NULL,
				   sysedp_reactive_capping_irq_handler,
				   pdata->irq_flags,
				   pdata->sysedpc.name,
				   pdata);
	if (ret) {
		pr_err("sysedp_reactive_capping_probe: request_threaded_irq failed (%d)\n",
		       ret);
		sysedp_unregister_consumer(&pdata->sysedpc);
		return ret;
	}

	return 0;
}

static const struct of_device_id sysedp_reactive_capping_of_match[] = {
	{ .compatible = "nvidia,tegra124-sysedp-reactive-capping", },
	{ },
};
MODULE_DEVICE_TABLE(of, sysedp_reactive_capping_of_match);

static struct platform_driver sysedp_reactive_capping_driver = {
	.probe = sysedp_reactive_capping_probe,
	.driver = {
		.name = "sysedp_reactive_capping",
		.owner = THIS_MODULE,
		.of_match_table = sysedp_reactive_capping_of_match,
	}
};

static __init int sysedp_reactive_capping_init(void)
{
	return platform_driver_register(&sysedp_reactive_capping_driver);
}
late_initcall(sysedp_reactive_capping_init);
