import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import i2c, spi
from esphome.const import CONF_ID, CONF_RESET_PIN

DEPENDENCIES = ["spi", "i2c"]
CODEOWNERS = ["@pysn2012"]

zl38063_ns = cg.esphome_ns.namespace("zl38063")
Zl38063Component = zl38063_ns.class_(
    "Zl38063Component", cg.Component, spi.SPIDevice, i2c.I2CDevice
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
    # 0x19 按键前端（ID 0x11）：原厂在 DSP 复位脉冲前初始化（factory_frontend_init）
    .extend(i2c.i2c_device_schema(0x19))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    await i2c.register_i2c_device(var, config)
    reset = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(var.set_reset_pin(reset))
