import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import spi
from esphome.const import CONF_ID, CONF_RESET_PIN

DEPENDENCIES = ["spi"]
CODEOWNERS = ["@pysn2012"]

zl38063_ns = cg.esphome_ns.namespace("zl38063")
Zl38063Component = zl38063_ns.class_(
    "Zl38063Component", cg.Component, spi.SPIDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Zl38063Component),
            # DSP 复位脚：低 10ms -> 高 -> 等 500ms
            cv.Optional(CONF_RESET_PIN, default=19): pins.gpio_output_pin_schema,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema())
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    reset = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(var.set_reset_pin(reset))
