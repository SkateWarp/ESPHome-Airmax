import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from ..climate import TclClimate

CONF_TCL_CLIMATE_ID = "tcl_climate_id"
CONF_FAN_SPEED = "fan_speed"
CONF_FAULT = "fault"
CONF_PROTOCOL_PROFILE = "protocol_profile"

TEXT_SENSORS = {
    CONF_FAN_SPEED: text_sensor.text_sensor_schema(
        icon="mdi:wind-power",
    ),
    CONF_FAULT: text_sensor.text_sensor_schema(
        icon="mdi:alert-circle-outline",
        entity_category="diagnostic",
    ),
    CONF_PROTOCOL_PROFILE: text_sensor.text_sensor_schema(
        icon="mdi:tune-variant",
        entity_category="diagnostic",
    ),
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_TCL_CLIMATE_ID): cv.use_id(TclClimate),
        }
    ).extend(
        {
            cv.Optional(key): schema
            for key, schema in TEXT_SENSORS.items()
        }
    ),
    cv.has_at_least_one_key(*TEXT_SENSORS),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TCL_CLIMATE_ID])

    for key in TEXT_SENSORS:
        if conf := config.get(key):
            entity = await text_sensor.new_text_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_text_sensor")(entity))
