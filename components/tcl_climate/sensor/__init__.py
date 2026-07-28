import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv

from ..climate import TclClimate

CONF_TCL_CLIMATE_ID = "tcl_climate_id"
CONF_CURRENT = "current"
CONF_SUPPLY_VOLTAGE = "supply_voltage"
CONF_PIPE_IN_TEMPERATURE = "pipe_in_temperature"
CONF_PIPE_OUT_TEMPERATURE = "pipe_out_temperature"
CONF_OUTSIDE_MOTOR = "outside_motor"
CONF_FAN_SPEED_RAW = "fan_speed_raw"
CONF_COMPRESSOR_STATE = "compressor_state"
CONF_VERTICAL_VANE_POSITION = "vertical_vane_position"
CONF_HORIZONTAL_VANE_POSITION = "horizontal_vane_position"

SENSORS = {
    CONF_CURRENT: sensor.sensor_schema(
        unit_of_measurement="A",
        icon="mdi:current-ac",
        accuracy_decimals=1,
        device_class="current",
        state_class="measurement",
        entity_category="diagnostic",
    ),
    CONF_SUPPLY_VOLTAGE: sensor.sensor_schema(
        unit_of_measurement="V",
        icon="mdi:sine-wave",
        accuracy_decimals=0,
        device_class="voltage",
        state_class="measurement",
        entity_category="diagnostic",
    ),
    CONF_PIPE_IN_TEMPERATURE: sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:pipe",
        accuracy_decimals=0,
        device_class="temperature",
        state_class="measurement",
        entity_category="diagnostic",
    ),
    CONF_PIPE_OUT_TEMPERATURE: sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:pipe",
        accuracy_decimals=0,
        device_class="temperature",
        state_class="measurement",
        entity_category="diagnostic",
    ),
    CONF_OUTSIDE_MOTOR: sensor.sensor_schema(
        icon="mdi:fan",
        accuracy_decimals=0,
        state_class="measurement",
        entity_category="diagnostic",
    ),
    CONF_FAN_SPEED_RAW: sensor.sensor_schema(
        icon="mdi:fan",
        accuracy_decimals=0,
        state_class="measurement",
        entity_category="diagnostic",
    ),
    CONF_COMPRESSOR_STATE: sensor.sensor_schema(
        icon="mdi:hvac",
        accuracy_decimals=0,
        entity_category="diagnostic",
    ),
    CONF_VERTICAL_VANE_POSITION: sensor.sensor_schema(
        icon="mdi:arrow-up-down",
        accuracy_decimals=0,
        entity_category="diagnostic",
    ),
    CONF_HORIZONTAL_VANE_POSITION: sensor.sensor_schema(
        icon="mdi:arrow-left-right",
        accuracy_decimals=0,
        entity_category="diagnostic",
    ),
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_TCL_CLIMATE_ID): cv.use_id(TclClimate),
        }
    ).extend({cv.Optional(key): schema for key, schema in SENSORS.items()}),
    cv.has_at_least_one_key(*SENSORS),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TCL_CLIMATE_ID])

    for key in SENSORS:
        if conf := config.get(key):
            entity = await sensor.new_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_sensor")(entity))
