import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from ..climate import TclClimate

CONF_TCL_CLIMATE_ID = "tcl_climate_id"
CONF_DEEP_SLEEP = "deep_sleep"
CONF_CLEAN_FILTER = "clean_filter"

BINARY_SENSORS = {
    CONF_DEEP_SLEEP: binary_sensor.binary_sensor_schema(
        icon="mdi:sleep",
    ),
    CONF_CLEAN_FILTER: binary_sensor.binary_sensor_schema(
        icon="mdi:air-filter",
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
            for key, schema in BINARY_SENSORS.items()
        }
    ),
    cv.has_at_least_one_key(*BINARY_SENSORS),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TCL_CLIMATE_ID])

    for key in BINARY_SENSORS:
        if conf := config.get(key):
            entity = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_binary_sensor")(entity))
