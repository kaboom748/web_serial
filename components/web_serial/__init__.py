"""web_serial: an in-browser SERIAL HUB (web console/tap, TCP232 raw port,
Web Serial PC bridge, Modbus RTU / DMX512 / NMEA decoders, RS485 de_pin).
Sibling of web_i2c / web_spi / web_onewire. NO build-time graft: the uart
debug callback is a public runtime seam, subscribed in setup().

Several hubs = several ``web_serial:`` blocks, one per uart bus.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.core import CORE
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@web_serial"]
DEPENDENCIES = ["network", "uart"]
AUTO_LOAD = ["socket"]

CONF_UART_ID = "uart_id"
CONF_TCP_PORT = "tcp_port"
CONF_DE_PIN = "de_pin"
CONF_OWNER = "owner"
CONF_GAP_MS = "gap_ms"
CONF_HEAP_FLOOR = "heap_floor"

web_serial_ns = cg.esphome_ns.namespace("web_serial")
WebSerial = web_serial_ns.class_("WebSerial", cg.Component)

MULTI_CONF = True

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebSerial),
        cv.Required(CONF_PORT): cv.port,
        cv.GenerateID(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        # raw network serial port (PuTTY/telnet/Eltima direct); 0 = disabled
        cv.Optional(CONF_TCP_PORT): cv.port,
        # RS485 driver-enable, asserted around every hub write (RS422: omit)
        cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
        # owner: the hub reads RX itself (interactive). false = tap-only guest
        # when OTHER components (modbus, a sensor...) own the reads.
        cv.Optional(CONF_OWNER, default=True): cv.boolean,
        # silence that closes a frame (Modbus RTU: ~4 ms; DMX: 4 ms; lines: 10)
        cv.Optional(CONF_GAP_MS, default=10): cv.int_range(min=1, max=1000),
        # safety floor (bytes): allocations that would drop the largest free
        # block below this are refused. Default: platform-appropriate in C++.
        cv.Optional(CONF_HEAP_FLOOR): cv.int_range(min=1024, max=200000),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # The wser transport module is ESPHome-free: its build guard cannot see
    # USE_SOCKET_IMPL_LWIP_TCP. Toolchain macros (ESP8266) enable it, and
    # this flag is the explicit belt-and-suspenders from the build system.
    if CORE.is_esp8266:
        cg.add_build_flag("-DWSER_TARGET_LWIP")

    # ---- UART transport audit (build-time verdict, replicated from
    # esphome/components/uart selection rules). ESPHome falls back to
    # SOFTWARE serial SILENTLY on ESP8266 when pins match no hardware
    # UART -- nine field resets taught us the cost. The verdict is
    # injected into C++: boot log with consequences + UI badge + the
    # egress quota law (shielded on software, wire-saturating on hw).
    uart_hw = True
    uart_name = "hardware"
    tx_num = rx_num = -1
    try:
        for bus in CORE.config.get("uart", []):
            if str(bus.get("id")) != str(config[CONF_UART_ID]):
                continue
            tx_num = bus.get("tx_pin", {}).get("number", -1) if "tx_pin" in bus else -1
            rx_num = bus.get("rx_pin", {}).get("number", -1) if "rx_pin" in bus else -1
            break
        if CORE.is_esp8266:
            logger_uart = str(CORE.config.get("logger", {}).get("hardware_uart", "UART0"))
            if tx_num == 2 and rx_num in (-1, 8):
                uart_name = "UART1 hardware (TX-only, GPIO2)"
            elif tx_num in (-1, 1) and rx_num in (-1, 3) and "UART0" not in logger_uart:
                uart_name = "UART0 hardware (GPIO1/3)"
            elif tx_num in (-1, 15) and rx_num in (-1, 13) and logger_uart != "UART0":
                uart_name = "UART0-swap hardware (GPIO15/13)"
            else:
                uart_hw = False
                uart_name = "SOFTWARE bit-bang"
        else:
            uart_name = "hardware (pin matrix)"
    except Exception:
        uart_name = "unverified (audit failed)"
    # (emission deplacee apres la creation de var -- voir plus bas)
    # The uart debug-callback machinery (UARTDirection, add_debug_callback,
    # debug_callback_ and the per-byte calls in every backend) is guarded by
    # USE_UART_DEBUGGER and normally only compiled when a ``debug:`` block is
    # present. web_serial IS a debugger: force the seam on.
    cg.add_define("USE_UART_DEBUGGER")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_uart_audit(uart_hw, uart_name, tx_num, rx_num))
    cg.add(var.set_port(config[CONF_PORT]))
    u = await cg.get_variable(config[CONF_UART_ID])
    cg.add(var.set_uart(u))
    if CONF_TCP_PORT in config:
        cg.add(var.set_tcp_port(config[CONF_TCP_PORT]))
    if CONF_DE_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_DE_PIN])
        cg.add(var.set_de_pin(pin))
    cg.add(var.set_owner(config[CONF_OWNER]))
    cg.add(var.set_gap_ms(config[CONF_GAP_MS]))
    if CONF_HEAP_FLOOR in config:
        cg.add(var.set_floor(config[CONF_HEAP_FLOOR]))
