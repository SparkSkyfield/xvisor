/**
 * Copyright (c) 2016 Anup Patel.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file platform.c
 * @author Anup Patel (anup@brainfault.org)
 * @brief platform pass-through emulator.
 *
 * This emulator to should be use for pass-through access to
 * platform device.
 */

#include <vmm_error.h>
#include <vmm_heap.h>
#include <vmm_stdio.h>
#include <vmm_devtree.h>
#include <vmm_devdrv.h>
#include <vmm_platform.h>
#include <vmm_host_irq.h>
#include <vmm_iommu.h>
#include <vmm_guest_aspace.h>
#include <vmm_devemu.h>
#include <vmm_modules.h>

#define MODULE_DESC			"Platform Pass-through Emulator"
#define MODULE_AUTHOR			"Anup Patel"
#define MODULE_LICENSE			"GPL"
#define MODULE_IPRIORITY		0
#define	MODULE_INIT			platform_pt_init
#define	MODULE_EXIT			platform_pt_exit

struct platform_pt_state {
	char name[64];
	struct vmm_guest *guest;
	u32 irq_count;
	u32 *host_irqs;
	u32 *host_type_irqs;
	u32 *guest_irqs;
	struct vmm_device *dev;
	struct vmm_iommu_domain *dom;
	struct vmm_notifier_block nb;
};

/* Handle host-to-guest routed IRQ generated by device */
static vmm_irq_return_t platform_pt_routed_irq(int irq, void *dev)
{
	int rc;
	bool found = FALSE;
	u32 i, host_irq = irq, guest_irq = 0;
	struct platform_pt_state *s = dev;

	/* Find guest irq */
	for (i = 0; i < s->irq_count; i++) {
		if (s->host_irqs[i] == host_irq) {
			guest_irq = s->guest_irqs[i];
			found = TRUE;
			break;
		}
	}
	if (!found) {
		goto done;
	}

	/* Lower the interrupt level.
	 * This will clear previous interrupt state.
	 */
	rc = vmm_devemu_emulate_irq(s->guest, guest_irq, 0);
	if (rc) {
		vmm_printf("%s: Emulate Guest=%s irq=%d level=0 failed\n",
			   __func__, s->guest->name, guest_irq);
	}

	/* Elevate the interrupt level.
	 * This will force interrupt triggering.
	 */
	rc = vmm_devemu_emulate_irq(s->guest, guest_irq, 1);
	if (rc) {
		vmm_printf("%s: Emulate Guest=%s irq=%d level=1 failed\n",
			   __func__, s->guest->name, guest_irq);
	}

done:
	return VMM_IRQ_HANDLED;
}

static int platform_pt_reset(struct vmm_emudev *edev)
{
	/* For now nothing to do here. */
	return VMM_OK;
}

static int platform_pt_fault(struct vmm_iommu_domain *dom,
			     struct vmm_device *dev,
			     physical_addr_t iova,
			     int flags, void *priv)
{
	struct platform_pt_state *s = priv;

	vmm_lerror("platform_pt",
		   "iommu fault flags=0x%x iova=0x%"PRIPADDR"\n",
		   flags, iova);

	vmm_manager_guest_halt(s->guest);

	return 0;
}

static void platform_pt_iter(struct vmm_guest *guest,
			     struct vmm_region *reg,
			     void *priv)
{
	struct platform_pt_state *s = priv;

	/* Map entire guest region */
	vmm_iommu_map(s->dom,
		      VMM_REGION_GPHYS_START(reg),
		      VMM_REGION_HPHYS_START(reg),
		      VMM_REGION_GPHYS_END(reg) - VMM_REGION_GPHYS_START(reg),
		      VMM_IOMMU_READ|VMM_IOMMU_WRITE);
}

static int platform_pt_guest_aspace_notification(
					struct vmm_notifier_block *nb,
					unsigned long evt, void *data)
{
	u32 i;
	struct vmm_guest_aspace_event *edata = data;
	struct platform_pt_state *s =
			container_of(nb, struct platform_pt_state, nb);

	/* We are only interested in guest aspace init events so,
	 * ignore other events.
	 */
	if (evt != VMM_GUEST_ASPACE_EVENT_INIT) {
		return NOTIFY_DONE;
	}

	/* We are only interested in events for our guest */
	if (s->guest != edata->guest) {
		return NOTIFY_DONE;
	}

	/* Map host IRQs to Guest IRQs */
	for (i = 0; i < s->irq_count; i++) {
		vmm_devemu_map_host2guest_irq(s->guest,
					      s->guest_irqs[i],
					      s->host_irqs[i]);
	}

	/* Iterate over each real ram regions of guest */
	if (s->dom) {
		vmm_guest_iterate_region(s->guest,
					 VMM_REGION_REAL |
					 VMM_REGION_MEMORY |
					 VMM_REGION_ISRAM |
					 VMM_REGION_ISHOSTRAM,
					 platform_pt_iter, s);
	}

	return NOTIFY_OK;
}

static int platform_pt_probe(struct vmm_guest *guest,
			     struct vmm_emudev *edev,
			     const struct vmm_devtree_nodeid *eid)
{
	int rc = VMM_OK;
	u32 i, irq_reg_count = 0;
	const char *iommu_device = NULL;
	struct platform_pt_state *s;

	s = vmm_zalloc(sizeof(struct platform_pt_state));
	if (!s) {
		rc = VMM_ENOMEM;
		goto platform_pt_probe_fail;
	}

	strlcpy(s->name, guest->name, sizeof(s->name));
	strlcat(s->name, "/", sizeof(s->name));
	if (strlcat(s->name, edev->node->name, sizeof(s->name)) >=
							sizeof(s->name)) {
		rc = VMM_EOVERFLOW;
		goto platform_pt_probe_freestate_fail;
	}

	s->guest = guest;
	s->irq_count = vmm_devtree_attrlen(edev->node, "host-interrupts");
	s->irq_count = s->irq_count / (sizeof(u32) * 2);
	s->guest_irqs = NULL;
	s->host_irqs = NULL;
	s->host_type_irqs = NULL;

	if (s->irq_count) {
		s->host_irqs = vmm_zalloc(sizeof(u32) * s->irq_count);
		if (!s->host_irqs) {
			rc = VMM_ENOMEM;
			goto platform_pt_probe_freestate_fail;
		}

		s->host_type_irqs = vmm_zalloc(sizeof(u32) * s->irq_count);
		if (!s->host_type_irqs) {
			rc = VMM_ENOMEM;
			goto platform_pt_probe_freehirqs_fail;
		}

		s->guest_irqs = vmm_zalloc(sizeof(u32) * s->irq_count);
		if (!s->guest_irqs) {
			rc = VMM_ENOMEM;
			goto platform_pt_probe_freehtirqs_fail;
		}
	}

	for (i = 0; i < s->irq_count; i++) {
		rc = vmm_devtree_read_u32_atindex(edev->node,
						  "host-interrupts",
						  &s->host_irqs[i], i*2);
		if (rc) {
			goto platform_pt_probe_cleanupirqs_fail;
		}

		rc = vmm_devtree_read_u32_atindex(edev->node,
					"host-interrupts",
					&s->host_type_irqs[i], i*2 + 1);
		if (rc) {
			goto platform_pt_probe_cleanupirqs_fail;
		}

		rc = vmm_devtree_read_u32_atindex(edev->node,
					  VMM_DEVTREE_INTERRUPTS_ATTR_NAME,
					  &s->guest_irqs[i], i);
		if (rc) {
			goto platform_pt_probe_cleanupirqs_fail;
		}

		rc = vmm_host_irq_set_type(s->host_irqs[i],
					   s->host_type_irqs[i]);
		if (rc) {
			goto platform_pt_probe_cleanupirqs_fail;
		}

		rc = vmm_host_irq_mark_routed(s->host_irqs[i]);
		if (rc) {
			goto platform_pt_probe_cleanupirqs_fail;
		}

		rc = vmm_host_irq_register(s->host_irqs[i], s->name,
					   platform_pt_routed_irq, s);
		if (rc) {
			vmm_host_irq_unmark_routed(s->host_irqs[i]);
			goto platform_pt_probe_cleanupirqs_fail;
		}

		irq_reg_count++;
	}

	iommu_device = NULL;
	vmm_devtree_read_string(edev->node,
				"iommu-device", &iommu_device);

	if (iommu_device) {
		s->dev = vmm_devdrv_bus_find_device_by_name(&platform_bus,
							    NULL,
							    iommu_device);
		if (!s->dev) {
			rc = VMM_EINVALID;
			goto platform_pt_probe_cleanupirqs_fail;
		}

		if (!s->dev->iommu_group) {
			rc = VMM_EINVALID;
			goto platform_pt_probe_cleanupirqs_fail;
		}

		vmm_devdrv_ref_device(s->dev);

		s->dom = vmm_iommu_domain_alloc(&platform_bus,
						s->dev->iommu_group,
						VMM_IOMMU_DOMAIN_UNMANAGED);
		if (!s->dom) {
			rc = VMM_EFAIL;
			goto platform_pt_probe_dref_dev_fail;
		}

		vmm_iommu_set_fault_handler(s->dom, platform_pt_fault, s);
	}

	s->nb.notifier_call = &platform_pt_guest_aspace_notification;
	s->nb.priority = 0;
	rc = vmm_guest_aspace_register_client(&s->nb);
	if (rc) {
		goto platform_pt_probe_free_dom_fail;
	}

	edev->priv = s;

	return VMM_OK;

platform_pt_probe_free_dom_fail:
	if (s->dom) {
		vmm_iommu_domain_free(s->dom);
	}
platform_pt_probe_dref_dev_fail:
	if (s->dev) {
		vmm_devdrv_dref_device(s->dev);
	}
platform_pt_probe_cleanupirqs_fail:
	for (i = 0; i < irq_reg_count; i++) {
		vmm_host_irq_unregister(s->host_irqs[i], s);
		vmm_host_irq_unmark_routed(s->host_irqs[i]);
	}
	if (s->guest_irqs) {
		vmm_free(s->guest_irqs);
	}
platform_pt_probe_freehtirqs_fail:
	if (s->host_type_irqs) {
		vmm_free(s->host_type_irqs);
	}
platform_pt_probe_freehirqs_fail:
	if (s->host_irqs) {
		vmm_free(s->host_irqs);
	}
platform_pt_probe_freestate_fail:
	vmm_free(s);
platform_pt_probe_fail:
	return rc;
}

static int platform_pt_remove(struct vmm_emudev *edev)
{
	u32 i;
	struct platform_pt_state *s = edev->priv;

	if (!s) {
		return VMM_EFAIL;
	}

	vmm_guest_aspace_unregister_client(&s->nb);
	if (s->dom) {
		vmm_iommu_domain_free(s->dom);
	}
	if (s->dev) {
		vmm_devdrv_dref_device(s->dev);
	}
	for (i = 0; i < s->irq_count; i++) {
		vmm_host_irq_unregister(s->host_irqs[i], s);
		vmm_host_irq_unmark_routed(s->host_irqs[i]);
	}
	if (s->guest_irqs) {
		vmm_free(s->guest_irqs);
	}
	if (s->host_type_irqs) {
		vmm_free(s->host_type_irqs);
	}
	if (s->host_irqs) {
		vmm_free(s->host_irqs);
	}
	vmm_free(s);

	edev->priv = NULL;

	return VMM_OK;
}

static struct vmm_devtree_nodeid platform_pt_emuid_table[] = {
	{ .type = "pt", .compatible = "platform", },
	{ /* end of list */ },
};

static struct vmm_emulator platform_pt_emulator = {
	.name = "platform",
	.match_table = platform_pt_emuid_table,
	.endian = VMM_DEVEMU_NATIVE_ENDIAN,
	.probe = platform_pt_probe,
	.reset = platform_pt_reset,
	.remove = platform_pt_remove,
};

static int __init platform_pt_init(void)
{
	return vmm_devemu_register_emulator(&platform_pt_emulator);
}

static void __exit platform_pt_exit(void)
{
	vmm_devemu_unregister_emulator(&platform_pt_emulator);
}

VMM_DECLARE_MODULE(MODULE_DESC,
			MODULE_AUTHOR,
			MODULE_LICENSE,
			MODULE_IPRIORITY,
			MODULE_INIT,
			MODULE_EXIT);
