EC sysfs Node Layout
====================

After the driver is loaded, the following nodes are created:

- `/sys/class/cix`
- `/sys/class/cix/ec`

Nodes under `ec`
----------------

- `asl_methods`: read-only, lists callable EC ASL methods and argument count
- `asl_call`: read/write, manually invokes a specific ASL method

Subdirectories under `ec`
-------------------------

- `fan/`
  - `fan_mode`: read/write, fan mode control
  - `fan_available_mode`: read-only, available fan modes
  - `fan_pwm`: read/write, fan PWM control
- `light/`
  - `kbl`: read/write, keyboard backlight info/control
  - `lss`: read-only, light sensor info
- `gpio/`
  - `gpio_count`: read-only, total gpio count
  - `gpio_read`: read/write, read selected gpio value
  - `gpio_write`: write-only, write gpio value with `"<gpio> <value>"`
