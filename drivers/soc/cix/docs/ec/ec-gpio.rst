EC GPIO Usage
=============

Overview
--------

EC GPIO is implemented with ACPI ASL methods `WRGP` and `RDGP`.

- The EC class driver provides sysfs helpers under `/sys/class/cix/ec/gpio`
- Linux gpiochip is registered directly by the EC driver and uses EC methods
  from `\_SB.EC0`

Prerequisite
------------

- ACPI EC node `\_SB.EC0` exists
- `\_SB.EC0` implements methods `WRGP` and `RDGP`

GPIO number mapping
-------------------

The driver applies this internal mapping:

- `0 ~ 87`: direct EC GPIO number
- `88 ~ 103`: translated to `gpio + 112`

ACPI ASL binding
----------------

Other ACPI devices can reference EC GPIOs directly through `\_SB.EC0`.

Example:

::

  GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
          "\\_SB.EC0", 0, ResourceConsumer) { 3 }

This binds to EC0 GPIO index `3`.

For this example:

- controller path: `\_SB.EC0`
- pin index: `3`
- Linux driver access method: `WRGP(3, value)` / `RDGP(3)`

User-space access
-----------------

Two access paths are available:

1. gpiochip (libgpiod tools such as `gpioinfo`, `gpioset`, and `gpioget`)
2. EC sysfs helper nodes under `/sys/class/cix/ec/gpio`

Direction note
--------------

The GPIO direction handling in the Linux driver is provided only for framework
compatibility. The actual GPIO direction is determined by hardware/firmware.

Example
-------

::

  gpioinfo
  gpioset gpiochipN 5=1
  gpioget gpiochipN 5

Sysfs helper nodes
------------------

- `/sys/class/cix/ec/gpio/gpio_count` (RO): total gpio count
- `/sys/class/cix/ec/gpio/gpio_read` (RW):
  - write gpio index to select target
  - read returns value of selected gpio
- `/sys/class/cix/ec/gpio/gpio_write` (WO):
  - write format: `<gpio> <value>`

Examples:

::

  cat /sys/class/cix/ec/gpio/gpio_count
  echo 5 > /sys/class/cix/ec/gpio/gpio_read
  cat /sys/class/cix/ec/gpio/gpio_read
  echo "5 1" > /sys/class/cix/ec/gpio/gpio_write
