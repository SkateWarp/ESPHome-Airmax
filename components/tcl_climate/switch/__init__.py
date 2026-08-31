import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv

from ..climate import TclClimate, tcl_climate_ns

CONF_TCL_CLIMATE_ID = "tcl_climate_id"
CONF_DISPLAY = "display"
CONF_BEEP = "beep"
CONF_HEALTH = "health"
CONF_RESTORE_STATE = "restore_state"
CONF_IGNORE_APPLIANCE_STATE = "ignore_appliance_state"

APPLIANCE_STATE_POLICY_SCHEMA = {
    cv.Optional(CONF_IGNORE_APPLIANCE_STATE, default=False): cv.boolean,
}

TclSwitch = tcl_climate_ns.class_(
    "TclSwitch",
    switch.Switch,
    cg.Parented.template(TclClimate),
)
TclSwitchType = tcl_climate_ns.enum("TclSwitchType", is_class=True)

SWITCH_TYPES = {
    CONF_DISPLAY: TclSwitchType.DISPLAY_CONTROL,
    CONF_BEEP: TclSwitchType.BEEP_CONTROL,
    CONF_HEALTH: TclSwitchType.HEALTH_CONTROL,
    CONF_RESTORE_STATE: TclSwitchType.RESTORE_STATE_CONTROL,
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_TCL_CLIMATE_ID): cv.use_id(TclClimate),
            cv.Optional(CONF_DISPLAY): switch.switch_schema(
                TclSwitch,
                icon="mdi:led-on",
                entity_category="config",
                default_restore_mode="RESTORE_DEFAULT_ON",
                block_inverted=True,
            ).extend(APPLIANCE_STATE_POLICY_SCHEMA),
            cv.Optional(CONF_BEEP): switch.switch_schema(
                TclSwitch,
                icon="mdi:volume-high",
                entity_category="config",
                default_restore_mode="RESTORE_DEFAULT_OFF",
                block_inverted=True,
            ),
            cv.Optional(CONF_HEALTH): switch.switch_schema(
                TclSwitch,
                icon="mdi:air-filter",
                entity_category="config",
                default_restore_mode="RESTORE_DEFAULT_ON",
                block_inverted=True,
            ).extend(APPLIANCE_STATE_POLICY_SCHEMA),
            cv.Optional(CONF_RESTORE_STATE): switch.switch_schema(
                TclSwitch,
                icon="mdi:backup-restore",
                entity_category="config",
                default_restore_mode="RESTORE_DEFAULT_ON",
                block_inverted=True,
            ),
        }
    ),
    cv.has_at_least_one_key(*SWITCH_TYPES),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TCL_CLIMATE_ID])

    for key, switch_type in SWITCH_TYPES.items():
        if conf := config.get(key):
            entity = await switch.new_switch(conf, switch_type)
            await cg.register_parented(entity, parent)
            cg.add(getattr(parent, f"set_{key}_switch")(entity))
            if key in (CONF_DISPLAY, CONF_HEALTH):
                cg.add(
                    getattr(parent, f"set_{key}_ignore_appliance_state")(
                        conf[CONF_IGNORE_APPLIANCE_STATE]
                    )
                )
