import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c
from esphome.const import CONF_ID
from esphome.components import switch

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["switch"]
CODEOWNERS = ["@pysn2012"]

CONF_BACKLIGHT = "backlight"

aw9106_ns = cg.esphome_ns.namespace("aw9106")
Aw9106Component = aw9106_ns.class_(
    "Aw9106Component", cg.Component, i2c.I2CDevice
)
Aw9106Backlight = aw9106_ns.class_(
    "Aw9106Backlight", switch.Switch, cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Aw9106Component),
            # LCD 电源/背光开关（reg 0x23/0x24，0x80=开）
            cv.Optional(CONF_BACKLIGHT): switch.switch_schema(
                Aw9106Backlight,
                entity_category=cv.ENTITY_CATEGORY_CONFIG,
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x5B))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
    if backlight := config.get(CONF_BACKLIGHT):
        sw = await switch.new_switch(backlight)
        cg.add(sw.set_parent(var))
