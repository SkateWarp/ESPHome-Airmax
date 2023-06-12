# ESPHome Custom Component for Airmax Air Conditioner

This repository contains a custom component for ESPHome, specifically designed for the Airmax brand of air conditioners. It has been tested and verified to work with the AWTBE12-C2 model, but it may also be compatible with other models that use the same WiFi adapter.

## Features

The custom component is currently hard-coded to keep the beeper always off and the display always off. As a novice in this field, I've created this custom component to the best of my abilities and understanding.

- Polling every 5 seconds
- Ability to change mode between cool, dry, fan only, and off
- Control over fan speed with Auto, Low, Medium, High, Turbo, and Quiet settings
- Ability to turn the vertical swing on and off

## Compatibility

This custom component is based on the TCL UART protocol for air conditioners. As such, it may also work with other brands that use the same protocol, such as TCL, iQool, and Pioneer.

## Contributions

Contributions to improve this custom component are welcome. If you have a different model of Airmax air conditioner, or a model from a different brand that uses the TCL UART protocol, and you're able to test and verify the functionality of this custom component, please share your findings.

## Disclaimer

Please note that this custom component is provided as-is, and while it has been tested and found to work with the specified model, there is no guarantee that it will work with all models or brands. Please use at your own risk.

## Code

Here is the code for the custom component. You can copy and paste it into your own ESPHome configuration:

```cpp
climate:
  - platform: custom
    lambda: |-
      auto tclac = new TCL();
      App.register_component(tclac);
      return {tclac};
    climates:
      - name: "TCL"    
  ```
      
## Thank you

xaxexa for his work on [ESPHomeTCL](https://github.com/xaxexa/ESPHome-TCLAC) repo.

htmltiger for his invaluable [contribution](https://github.com/htmltiger/tcl-electriq-split-ac) for decoding the protocol. 
