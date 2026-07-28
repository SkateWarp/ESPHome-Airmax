import esphome.codegen as cg
from esphome.components import climate, uart
import esphome.config_validation as cv
from esphome.const import CONF_UPDATE_INTERVAL

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "sensor", "switch", "text_sensor"]

CONF_SUPPORTS_HEAT = "supports_heat"
CONF_SUPPORTS_HORIZONTAL_SWING = "supports_horizontal_swing"
CONF_PROTOCOL_PROFILE = "protocol_profile"
CONF_STATUS_FRAME_LENGTH = "status_frame_length"
CONF_RESTORE_STATE = "restore_state"
CONF_DEEP_SLEEP_ACTIVE_LOW = "deep_sleep_active_low"
CONF_STATUS_TIMEOUT = "status_timeout"
CONF_INTER_BYTE_TIMEOUT = "inter_byte_timeout"
CONF_TEMPERATURE_SAMPLES = "temperature_samples"

tcl_climate_ns = cg.esphome_ns.namespace("tcl_climate")
TclClimate = tcl_climate_ns.class_(
    "TclClimate",
    climate.Climate,
    cg.PollingComponent,
    uart.UARTDevice,
)
TclProtocolProfile = tcl_climate_ns.enum("TclProtocolProfile", is_class=True)

PROTOCOL_PROFILES = {
    "tcl_35": TclProtocolProfile.PROFILE_TCL_35,
    "electriq_31": TclProtocolProfile.PROFILE_ELECTRIQ_31,
    "pioneer_31": TclProtocolProfile.PROFILE_PIONEER_31,
    "tyjw2_35": TclProtocolProfile.PROFILE_TYJW2_35,
    "tclac_38": TclProtocolProfile.PROFILE_TCLAC_38,
}


def validate_status_frame_length(value):
    if isinstance(value, str) and value.lower() == "auto":
        return 0
    value = cv.int_(value)
    if value not in (61, 65, 68):
        raise cv.Invalid("status_frame_length must be auto, 61, 65, or 68")
    return value


def validate_timing(config):
    if (
        config[CONF_STATUS_TIMEOUT].total_milliseconds
        <= config[CONF_UPDATE_INTERVAL].total_milliseconds
    ):
        raise cv.Invalid("status_timeout must be longer than update_interval")
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(TclClimate)
    .extend(cv.polling_component_schema("500ms"))
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_SUPPORTS_HEAT, default=False): cv.boolean,
            cv.Optional(CONF_SUPPORTS_HORIZONTAL_SWING, default=False): cv.boolean,
            cv.Optional(CONF_PROTOCOL_PROFILE, default="tcl_35"):
                cv.enum(PROTOCOL_PROFILES, lower=True),
            cv.Optional(CONF_STATUS_FRAME_LENGTH, default="auto"):
                validate_status_frame_length,
            cv.Optional(CONF_RESTORE_STATE, default=False): cv.boolean,
            cv.Optional(CONF_DEEP_SLEEP_ACTIVE_LOW, default=True): cv.boolean,
            cv.Optional(CONF_STATUS_TIMEOUT, default="5s"):
                cv.positive_time_period_milliseconds,
            cv.Optional(CONF_INTER_BYTE_TIMEOUT, default="50ms"):
                cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TEMPERATURE_SAMPLES, default=10):
                cv.int_range(min=1, max=20),
        }
    ),
    validate_timing,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "tcl_climate",
    baud_rate=9600,
    data_bits=8,
    parity="EVEN",
    stop_bits=1,
    require_rx=True,
    require_tx=True,
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_supports_heat(config[CONF_SUPPORTS_HEAT]))
    cg.add(var.set_protocol_profile(config[CONF_PROTOCOL_PROFILE]))
    cg.add(var.set_status_frame_size(config[CONF_STATUS_FRAME_LENGTH]))
    cg.add(var.set_restore_state(config[CONF_RESTORE_STATE]))
    cg.add(
        var.set_supports_horizontal_swing(
            config[CONF_SUPPORTS_HORIZONTAL_SWING]
        )
    )
    cg.add(var.set_deep_sleep_active_low(config[CONF_DEEP_SLEEP_ACTIVE_LOW]))
    cg.add(
        var.set_status_timeout(
            config[CONF_STATUS_TIMEOUT].total_milliseconds
        )
    )
    cg.add(
        var.set_inter_byte_timeout(
            config[CONF_INTER_BYTE_TIMEOUT].total_milliseconds
        )
    )
    cg.add(var.set_temperature_samples(config[CONF_TEMPERATURE_SAMPLES]))
