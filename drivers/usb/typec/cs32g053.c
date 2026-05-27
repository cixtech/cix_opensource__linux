// SPDX-License-Identifier: GPL-2.0
/*
 * PD driver for typec connection
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_tbt.h>
#include <linux/usb/role.h>
#include <linux/regulator/consumer.h>
#include <linux/acpi.h>
#include "class.h"

#define CS_FIELD_GET(_mask, _reg) ((typeof(_mask))((_reg) & (_mask)) >> __bf_shf(_mask))
#define CS32G053_REG_VID		0x00
#define CS32G053_REG_PID		0x00
#define CS_DATA_CONTROL 0x50
#define I2C_INT_ACK_BIT BIT(2)
#define HPD_IRQ_ACK BIT(5)
#define CS_DATA_STATUS	 0x5F
//byte 1
#define CONNECTION_ORIENTATION  BIT(1)
#define USB2_CONNECTION BIT(4)
#define USB3_2_CONNECTION BIT(5)
#define USB_DATA_ROLE BIT(7)
//byte 2
#define DP_CONNECTION BIT(0)
#define DP_SOURCE_SINK BIT(1)
#define DEBUG_ACCESSORY_ATTACHED BIT(4)
#define HPD_IRQ BIT(6)
#define HPD_STATE BIT(7)
//byte 3
#define POWER_SOURCE BIT(0)
#define POWER_SINK BIT(1)
#define NO_POWER 0
#define POWER_MASK 3

#define DATA_STATUS_COUNT 6

enum {
	ALTMODE_DP = 0,
	ALTMODE_MAX,
};

struct cs32g053h {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock; /* device lock */
	u8 i2c_protocol:1;	/*i2c or smbus*/
	int id;
	u8 data_status[DATA_STATUS_COUNT];
	struct typec_mux_state state;
	struct typec_capability caps;
	struct typec_switch *ori_sw;
	struct typec_mux *mux;
	struct usb_role_switch *role_sw;
	struct typec_altmode p_altmode[ALTMODE_MAX];
	struct typec_port *port;
	enum usb_role role;
	unsigned long mode;
	u32 hpd_status;
	struct typec_partner	*partner;
	struct typec_partner_desc desc;
	bool pd_int_disabled;
	struct list_head list;
	struct delayed_work irq_work;
};

static LIST_HEAD(cs_list);
static DEFINE_MUTEX(cs_lock);

#define RTS_MAX_LEN	64

static int
cs32g053h_block_read(struct cs32g053h *cs, u8 reg, void *val, size_t len)
{
	return regmap_bulk_read(cs->regmap, reg, val, len);
}
static int cs32g053h_block_write(struct cs32g053h *cs, u8 reg,
				const void *val, size_t len)
{
	return regmap_bulk_write(cs->regmap, reg, val, len);
}
static inline int cs32g053h_read16(struct cs32g053h *cs, u8 reg, u16 *val)
{
	return cs32g053h_block_read(cs, reg, val, sizeof(u16));
}
static inline int cs32g053h_write16(struct cs32g053h *cs, u8 reg, u16 val)
{
	return cs32g053h_block_write(cs, reg, &val, sizeof(u16));
}
static inline int cs32g053h_read32(struct cs32g053h *cs, u8 reg, u32 *val)
{
	return cs32g053h_block_read(cs, reg, val, sizeof(u32));
}
static inline int cs32g053h_write32(struct cs32g053h *cs, u8 reg, u32 val)
{
	return cs32g053h_block_write(cs, reg, &val, sizeof(u32));
}

static const struct regmap_config cs32g053h_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x7F,
};

char cs_dp_only_mode[] = "Alt Mode:DP";
char cs_dp_usb_mode[] = "Alt Mode:DP + USB";
char cs_usb_mode[] = "Alt Mode:USB";

char cs_power_source[] = "power source";
char cs_power_sink[] = "power sink";

char cs_oritation_normal[] = "oritation normal";
char cs_oritation_reverse[] = "oritation reverse";

char cs_data_host[] = "usb host";
char cs_data_device[] = "usb device";


static int cs32g053h_typec_port_update(struct cs32g053h *typec)
{
	enum typec_orientation orientation;
	int ret = 0;
	struct typec_displayport_data dp_data;
	enum usb_role role;

	if (!(typec->data_status[1] & USB3_2_CONNECTION) && !(typec->data_status[2] & DP_CONNECTION)) {
		orientation = TYPEC_ORIENTATION_NONE;
	} else if (typec->data_status[1] & CONNECTION_ORIENTATION) {
		orientation = TYPEC_ORIENTATION_REVERSE;
	} else {
		orientation = TYPEC_ORIENTATION_NORMAL;
	}

	ret = typec_switch_set(typec->ori_sw, orientation);

	if (ret)
		return ret;

	if ((!(typec->data_status[1] & USB3_2_CONNECTION)) && (!(typec->data_status[1] & USB2_CONNECTION))) {
		role = USB_ROLE_NONE;
	} else if (typec->data_status[1] & USB_DATA_ROLE) {
		role = USB_ROLE_DEVICE;
	} else {
		if ((!(typec->data_status[1] & USB3_2_CONNECTION)) && (typec->data_status[2] & DP_CONNECTION)) {
			role = USB_ROLE_HOST_20;
		} else {
			role = USB_ROLE_HOST;
		}
	}

	if (typec->data_status[2] & DEBUG_ACCESSORY_ATTACHED) {
		//debug accessory mode
		dev_err(typec->dev, "debug accessory mode not support, try with another usb cable\n");
		return 0;
	}

	if ((typec->data_status[1] & USB3_2_CONNECTION) &&
		(typec->data_status[2] & DP_CONNECTION)) {
		//usb + dp
		dp_data.status = DP_STATUS_ENABLED;
		if (typec->data_status[2] & HPD_STATE)
			dp_data.status |= DP_STATUS_HPD_STATE;
		if (typec->data_status[2] & HPD_IRQ)
			dp_data.status |= DP_STATUS_IRQ_HPD;

		typec->state.data = &dp_data;
		typec->state.mode = TYPEC_DP_STATE_D;
		typec->state.alt = &typec->p_altmode[ALTMODE_DP];
	} else if (typec->data_status[2] & DP_CONNECTION) {
		//dp only
		dp_data.status = DP_STATUS_ENABLED;
		if (typec->data_status[2] & HPD_STATE)
			dp_data.status |= DP_STATUS_HPD_STATE;
		if (typec->data_status[2] & HPD_IRQ)
			dp_data.status |= DP_STATUS_IRQ_HPD;
		typec->state.data = &dp_data;
		typec->state.mode = TYPEC_DP_STATE_E;
		typec->state.alt = &typec->p_altmode[ALTMODE_DP];
	} else if (typec->data_status[1] & USB3_2_CONNECTION) {
		//usb only
		dp_data.status = 0;
		typec->state.mode = TYPEC_STATE_USB;
		typec->state.alt = NULL;
		typec->state.data = NULL;
	} else {
		//disconnect
		dp_data.status = 0;
		typec->state.mode = TYPEC_STATE_SAFE;
		typec->state.alt = NULL;
		typec->state.data = NULL;
	}

	ret = typec_mux_set(typec->mux, &typec->state);

	if (ret)
		return ret;

	if ((typec->data_status[3]& POWER_MASK) == POWER_SOURCE) {
		typec->caps.type = TYPEC_PORT_SRC;
		typec_set_pwr_role(typec->port, TYPEC_SOURCE);
	} else if ((typec->data_status[3]& POWER_MASK) == POWER_SINK) {
		typec->caps.type = TYPEC_PORT_SNK;
		typec_set_pwr_role(typec->port, TYPEC_SINK);
	} else {
		dev_dbg(typec->dev, "typec port(%d): unkonwn power state %d\n", typec->id, typec->data_status[3]);
	}

	if (typec->role != role) {
		typec->role = role;
		ret = usb_role_switch_set_role(typec->role_sw, role);
		if (role == USB_ROLE_DEVICE) {
			typec_set_data_role(typec->port, TYPEC_DEVICE);
		} else {
			typec_set_data_role(typec->port, TYPEC_HOST);
		}
	}

	if (typec->mode == typec->state.mode && typec->hpd_status == dp_data.status)
		return ret;

	if (typec->state.mode == TYPEC_DP_STATE_D) {
		dev_info(typec->dev, "typec port(%d): alt mode(%s) \n oritation(%s), usb_data_role(%s)\n power role(%s), hpd_state(%d)\n",
			typec->id,
			cs_dp_usb_mode,
			orientation == TYPEC_ORIENTATION_REVERSE ? cs_oritation_reverse : cs_oritation_normal,
			role == USB_ROLE_DEVICE ? cs_data_device : cs_data_host,
			typec->caps.type == TYPEC_PORT_SRC ? cs_power_source:cs_power_sink,
			(int)(dp_data.status & DP_STATUS_HPD_STATE));
	} else if (typec->state.mode == TYPEC_DP_STATE_E) {
		dev_info(typec->dev, "typec port(%d): alt mode(%s) \n oritation(%s)\n power role(%s), hpd_state(%d)\n",
			typec->id,
			cs_dp_only_mode,
			orientation == TYPEC_ORIENTATION_REVERSE ? cs_oritation_reverse : cs_oritation_normal,
			typec->caps.type == TYPEC_PORT_SRC ? cs_power_source:cs_power_sink,
			(int)(dp_data.status & DP_STATUS_HPD_STATE));
	} else if (typec->state.mode == TYPEC_STATE_USB) {
		dev_info(typec->dev, "typec port(%d): alt mode(%s) \n oritation(%s)\n power role(%s), usb_data_role(%s)\n",
			typec->id,
			cs_usb_mode,
			orientation == TYPEC_ORIENTATION_REVERSE ? cs_oritation_reverse : cs_oritation_normal,
			typec->caps.type == TYPEC_PORT_SRC ? cs_power_source:cs_power_sink,
			role == USB_ROLE_DEVICE ? cs_data_device : cs_data_host);
	} else if (typec->state.mode == TYPEC_STATE_SAFE) {
		dev_info(typec->dev, "typec port(%d): disconnect state \n", typec->id);
	}

	typec->mode = typec->state.mode;
	typec->hpd_status = dp_data.status;

	if ((typec->data_status[1] == 0) && (typec->data_status[2] == 0)) {
		dev_info(typec->dev, "typec port(%d): unregister partner\n", typec->id);
		typec_unregister_partner(typec->partner);
		typec->partner = NULL;
	} else {
		if (NULL == typec->partner) {
			dev_info(typec->dev, "typec port(%d): register partner\n", typec->id);
			typec->partner = typec_register_partner(typec->port, &typec->desc);
		}

		if (IS_ERR(typec->partner)) {
			dev_info(typec->dev, "typec port(%d): register partner error \n", typec->id);
		}
	}
	return ret;
}

static void cs32g053h_typec_register_port_altmodes(struct cs32g053h *typec)
{
	/* All PD capable CrOS devices are assumed to support DP altmode. */
	typec->p_altmode[ALTMODE_DP].svid = USB_TYPEC_DP_SID;
	typec->p_altmode[ALTMODE_DP].mode = USB_TYPEC_DP_MODE;
	typec->state.alt = NULL;
	typec->state.mode = TYPEC_STATE_USB;
	typec->state.data = NULL;

	typec->partner = NULL;
	typec->desc.accessory = TYPEC_ACCESSORY_NONE;
	typec->desc.usb_pd = 0;
}

static int cs32g053h_event_handler(struct cs32g053h *cs, u8 clear_irq)
{
	int ret = 0;
	int num = 0;
	u8 data_control[5] = {0};

	dev_info(cs->dev,"%s clear_irq=%d\n",__func__,clear_irq);

	mutex_lock(&cs->lock);

	if (clear_irq) {

		data_control[0] = 4;
		data_control[1] = I2C_INT_ACK_BIT;
		data_control[2] = 0;
		data_control[3] = 0;
		data_control[4] = 0;

		if (cs->id == 0) {
			data_control[3] = 0;
			dev_info(cs->dev," this is typec0 clear_irq\n");
		} else if(cs->id == 1) {
			data_control[3] = 1;
			dev_info(cs->dev," this is typec1 clear_irq\n");
		} else {
			dev_info(cs->dev,"the typec port is error!\n");
		}

		ret = cs32g053h_block_write(cs, CS_DATA_CONTROL, data_control, 5);
	}

	while(num++<5){
		ret = cs32g053h_block_read(cs, CS_DATA_STATUS, cs->data_status, 6);
		if((ret == 0)&&(cs->data_status[1]!=0xff)&&(cs->data_status[2]!=0xff))
			break;
	}

	dev_info(cs->dev, "typec port(%d):data status = 0x%x , 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
			cs->id, cs->data_status[0], cs->data_status[1], cs->data_status[2], cs->data_status[3], cs->data_status[4], cs->data_status[5]);

	cs32g053h_typec_port_update(cs);

	if (cs->data_status[2] & HPD_IRQ) {
		data_control[0] = 2;
		data_control[1] = 0;
		data_control[2] = HPD_IRQ_ACK;

		if (cs->id == 0) {
			data_control[3] = 0;
			dev_info(cs->dev," this is typec0 HPD_IRQ\n");
		} else if(cs->id == 1) {
			data_control[3] = 1;
			dev_info(cs->dev," this is typec1 HPD_IRQ\n");
		} else {
			dev_info(cs->dev,"the typec port is error!\n");
		}

		data_control[4] = 0;
		ret = cs32g053h_block_write(cs, CS_DATA_CONTROL, data_control, 5);

		if (ret != 0 ) {
			goto i2c_err;
		}
	}

i2c_err:
	if (ret != 0 ) {
		dev_err(cs->dev, "typec port(%d):i2c transfer error = %d, disable irq\n", cs->id, ret);
		ret = -ESHUTDOWN;
	}

	mutex_unlock(&cs->lock);
	return ret;
}

static int cs32g053h_typec_parse_port_props(struct typec_capability *cap,
						struct fwnode_handle *fwnode,
						struct device *dev)
{
	const char *buf;
	int ret;
	memset(cap, 0, sizeof(*cap));
	ret = fwnode_property_read_string(fwnode, "power-role", &buf);
	if (ret) {
		dev_err(dev, "power-role not found: %d\n", ret);
		return ret;
	}
	ret = typec_find_port_power_role(buf);
	if (ret < 0)
		return ret;
	cap->type = ret;
	ret = fwnode_property_read_string(fwnode, "data-role", &buf);
	if (ret) {
		dev_err(dev, "data-role not found: %d\n", ret);
		return ret;
	}
	ret = typec_find_port_data_role(buf);
	if (ret < 0)
		return ret;
	cap->data = ret;
	ret = fwnode_property_read_string(fwnode, "try-power-role", &buf);
	if (ret) {
		dev_err(dev, "try-power-role not found: %d\n", ret);
		return ret;
	}
	ret = typec_find_power_role(buf);
	if (ret < 0)
		return ret;
	cap->prefer_role = ret;
	cap->fwnode = fwnode;
	return 0;
}

static int cs32g053h_typec_get_switch_handles(struct cs32g053h *typec,
					 struct fwnode_handle *fwnode,
					 struct device *dev)
{
	typec->mux = typec->port->mux;
	typec->ori_sw = typec->port->sw;
	typec->role_sw = fwnode_usb_role_switch_get(fwnode);
	if (IS_ERR(typec->role_sw))
		return dev_err_probe(dev, PTR_ERR(typec->role_sw), "USB role switch handle is error\n");

	if (!typec->role_sw || !typec->mux || !typec->ori_sw)
		dev_warn(dev, "mux or switch or role switch has not found\n");

	return 0;
}

static void cs32g053h_unregister_ports(struct cs32g053h *typec)
{
	if (typec->port) {
		typec->state.alt = NULL;
		typec->state.mode = TYPEC_STATE_USB;
		typec->state.data = NULL;
		usb_role_switch_set_role(typec->role_sw, USB_ROLE_NONE);
		typec_switch_set(typec->ori_sw, TYPEC_ORIENTATION_NONE);
		typec_mux_set(typec->mux, &typec->state);
		usb_role_switch_put(typec->role_sw);
		typec_unregister_port(typec->port);
	}
}

static irqreturn_t cs32g053h_irq_handle(int irq, void *data)
{
	struct cs32g053h *cs = (struct cs32g053h *)data;
	int ret;

	ret = cs32g053h_event_handler(cs, 1);

	if (ret == -ESHUTDOWN)
		disable_irq_nosync(irq);

	return IRQ_HANDLED;
}

static irqreturn_t cs32g053h_irq_cs_handle(int irq, void *data)
{
	struct list_head *item, *tmp;
	struct i2c_client *client, *cl;
	struct cs32g053h *cs = data, *rt;

	/* handle own cs event */
	cs32g053h_irq_handle(irq, cs);

	/* polling other cs event which without irq */
	client = to_i2c_client(cs->dev);
	mutex_lock(&cs_lock);
	list_for_each_safe(item, tmp, &cs_list) {
		rt = list_entry(item, struct cs32g053h, list);
		cl = to_i2c_client(rt->dev);

		if (client->adapter == cl->adapter && cs != rt)
			cs32g053h_irq_handle(irq, rt);
	}
	mutex_unlock(&cs_lock);

	return IRQ_HANDLED;
}

static int cs32g053h_init_ports(struct cs32g053h *typec)
{
	struct device *dev = typec->dev;
	struct fwnode_handle *fwnode;

	int ret;
	int nports;

	nports = device_get_child_node_count(dev);
	if (nports == 0) {
		dev_err(dev, "No port entries found.\n");
		return -ENODEV;
	}

	if (nports > 1) {
		dev_err(dev, "More ports listed than can be supported.\n");
		return -EINVAL;
	}

	device_for_each_child_node(dev, fwnode) {
		ret = cs32g053h_typec_parse_port_props(&typec->caps, fwnode, dev);
		if (ret < 0)
			goto unregister_ports;

		typec->port = typec_register_port(dev, &typec->caps);
		if (IS_ERR(typec->port)) {
			dev_err(dev, "Failed to register port\n");
			ret = PTR_ERR(typec->port);
			goto unregister_ports;
		}

		ret = cs32g053h_typec_get_switch_handles(typec, fwnode, dev);
		if (ret) {
			dev_err(dev, "No switch control for port \n");
			goto unregister_ports;
		}
	}

	return ret;

unregister_ports:
	cs32g053h_unregister_ports(typec);
	return ret;
}

static ssize_t disable_cs_pd_interrupt_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cs32g053h *cs = i2c_get_clientdata(client);
	const char *p;

	if (cs->pd_int_disabled == 1)
		p = "disabled";
	else
		p = "enabled";

	return sysfs_emit(buf, "%s\n", p);
}

static ssize_t disable_cs_pd_interrupt_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cs32g053h *cs = i2c_get_clientdata(client);
	bool value;
	int ret;

	ret = strtobool(buf, &value);
	if (!ret) {
		cs->pd_int_disabled = value;

		if (value)
			disable_irq(client->irq);
		else
			enable_irq(client->irq);

		dev_alert(dev, "Only for USB Compliance test, PD int is %s\n",
				value ? "disabled" : "enabled");
	}

	if (!ret)
		return count;

	return ret;
}

DEVICE_ATTR_RW(disable_cs_pd_interrupt);

static bool is_cs_irq_shared_deivces_all_bound(struct cs32g053h *cs)
{
	struct i2c_client *client;
	struct fwnode_handle *child, *gchild;
	int all_bound = 1, is_cs_node = 0;

	device_for_each_child_node(cs->dev->parent, child) {
		is_cs_node = 0;
		fwnode_for_each_child_node(child, gchild) {
			if (fwnode_property_present(gchild, "power-role")
			&& fwnode_property_present(gchild, "data-role")
			&& fwnode_property_present(gchild, "try-power-role")) {
				is_cs_node = 1;
				break;
			}
		}
		if (!is_cs_node)
			continue;

		client = i2c_find_device_by_fwnode(child);
		if (!client || !device_is_bound(&client->dev)) {
			all_bound = 0;
			break;
		}
	}

	return all_bound;
}

static void cs_request_irq_work_fn(struct work_struct *work)
{
	struct cs32g053h *cs = container_of(work, struct cs32g053h, irq_work.work);
	struct i2c_client *client = to_i2c_client(cs->dev);
	int ret;

	if (client->irq <= 0)
		return;

	if (!is_cs_irq_shared_deivces_all_bound(cs)) {
		schedule_delayed_work(&cs->irq_work, msecs_to_jiffies(5));
		dev_dbg(cs->dev, "irq work scheduled\n");
		return;
	}

	irq_set_status_flags(client->irq, IRQ_DISABLE_UNLAZY);
	ret = request_threaded_irq(client->irq, NULL,
				   cs32g053h_irq_cs_handle,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
				   dev_name(&client->dev), cs);
	if (ret == -EBUSY) {
		/* irq already request in other cs, do nothing here */
		dev_info(&client->dev, "irq %d already registerd", client->irq);
	} else if (ret < 0) {
		dev_err(&client->dev, "request irq %d failed %d", client->irq, ret);
	} else {
		/* succeed! remove from polling list and handle separately. */
		mutex_lock(&cs_lock);
		list_del_init(&cs->list);
		mutex_unlock(&cs_lock);
	}
}

static int cs32g053h_probe(struct i2c_client *client)
{
	struct cs32g053h *cs;
	int ret;

	dev_dbg(&client->dev, "IRQ %d supplied\n", client->irq);
	cs = devm_kzalloc(&client->dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;

	ret = device_property_read_u32(&client->dev, "id", &cs->id);
	if (ret < 0)
		return ret;

	cs32g053h_typec_register_port_altmodes(cs);

	mutex_init(&cs->lock);

	cs->dev = &client->dev;
	cs->regmap = devm_regmap_init_i2c(client, &cs32g053h_regmap_config);

	if (IS_ERR(cs->regmap))
		return PTR_ERR(cs->regmap);
	if (i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		cs->i2c_protocol = true;
	} else {
		dev_err(&client->dev, "i2c check functionality failed.");
		return -ENODEV;
	}

	i2c_set_clientdata(client, cs);

	ret = cs32g053h_init_ports(cs);
	if (ret)
		return -EPROBE_DEFER;

	ret = cs32g053h_event_handler(cs, 0);
	if (ret) {
		dev_err(&client->dev, "typec event handling error %d", ret);
		return ret;
	}
	/*
	* TBD Check do anything about in init schedule like enable interrupt
	* or check the firmware running status before request irq
	*/

	INIT_LIST_HEAD(&cs->list);
	mutex_lock(&cs_lock);
	list_add(&cs->list, &cs_list);
	mutex_unlock(&cs_lock);

	INIT_DELAYED_WORK(&cs->irq_work, cs_request_irq_work_fn);
	if (client->irq)
		schedule_delayed_work(&cs->irq_work, msecs_to_jiffies(5));

	ret = device_create_file(&client->dev, &dev_attr_disable_cs_pd_interrupt);
	if (ret < 0)
		dev_err(&client->dev, "Failed to create sysfs attribute, ret = %d\n",
			ret);

	device_set_wakeup_capable(&client->dev, true);

	return ret;
}

static void cs32g053h_remove(struct i2c_client *client)
{
	struct cs32g053h *cs = i2c_get_clientdata(client);

	device_remove_file(&client->dev, &dev_attr_disable_cs_pd_interrupt);

	if (cs->partner) {
		typec_unregister_partner(cs->partner);
		cs->partner = NULL;
	}

	if (cs->role_sw) {
		usb_role_switch_put(cs->role_sw);
	}

	if (cs->port) {
		typec_unregister_port(cs->port);
	}

	if (client->irq > 0)
		free_irq(client->irq, cs);

	mutex_lock(&cs_lock);
	list_del(&cs->list);
	mutex_unlock(&cs_lock);
}

static const struct of_device_id cs32g053h_of_match[] = {
	{ .compatible = "chipsea,cs32g053h", },
	{}
};

static const struct acpi_device_id cs32g053h_acpi_match[] = {
	{ "CIXH2012" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, cs32g053h_acpi_match);

static const struct i2c_device_id cs32g053h_id[] = {
	{ "cs32g053h" },
	{ }
};

#ifdef CONFIG_PM_SLEEP
static int cs32g053h_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	disable_irq(client->irq);
	if (device_may_wakeup(dev)) {
		enable_irq_wake(client->irq);
		dev_info(&client->dev, "enable wake");
	}

	return 0;
}

static int cs32g053h_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (device_may_wakeup(dev)) {
		disable_irq_wake(client->irq);
		dev_info(&client->dev, "enable irq");
	}
	enable_irq(client->irq);

	return 0;
}

static int cs32g053h_restore(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cs32g053h *cs = i2c_get_clientdata(client);
	int ret = 0;

	if (device_may_wakeup(dev)) {
		disable_irq_wake(client->irq);
		dev_info(&client->dev, "enable irq");
	}
	enable_irq(client->irq);

	/* Re-trigger PD event if PD event occurs during std */
	ret = cs32g053h_event_handler(cs, 0);
	if (ret) {
		dev_err(&client->dev, "typec event handling error %d", ret);
		return ret;
	}

	return ret;
}
#endif

static void cs32g053h_shutdown(struct i2c_client *client)
{
	if (client->irq) {
		disable_irq(client->irq);
		if (device_may_wakeup(&client->dev)) {
			enable_irq_wake(client->irq);
			dev_info(&client->dev, "enable wake");
		}
	}
}

static const struct dev_pm_ops cs32g053h_pm_ops = {
	.suspend = cs32g053h_suspend,
	.resume = cs32g053h_resume,
	.freeze = cs32g053h_suspend,
	.thaw = cs32g053h_resume,
	.restore = cs32g053h_restore,
};

MODULE_DEVICE_TABLE(i2c, cs32g053h_id);
static struct i2c_driver cs32g053h_i2c_driver = {
	.driver = {
		.name = "cs32g053h",
		.of_match_table = cs32g053h_of_match,
		.acpi_match_table = cs32g053h_acpi_match,
		.pm = &cs32g053h_pm_ops
	},
	.probe = cs32g053h_probe,
	.remove = cs32g053h_remove,
	.id_table = cs32g053h_id,
	.shutdown = cs32g053h_shutdown
};

module_i2c_driver(cs32g053h_i2c_driver);
MODULE_AUTHOR("Finn Zhang <Finn.Zhang@cixtech.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CHIPSEA CS32G053H USB PD Controller Driver");
