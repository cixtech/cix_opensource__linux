EC Fan Control Usage
====================

Prerequisite
------------

Fan control is available **only when the EC ACPI node is present and the
corresponding EC methods exist** (for example: `SFAT`, `SFMT`, `SFMN`, `SFPF`,
`SFPW`, `GFPW`). If these methods are missing in firmware, fan sysfs operations
will fail.

How to verify prerequisites
---------------------------

1. Check whether the EC class node exists:

::

  ls /sys/class/cix/ec

2. Check whether fan-related ASL methods are exported by firmware:

::

  cat /sys/class/cix/ec/asl_methods | grep -E "^(SFAT|SFMT|SFMN|SFPF|SFPW|GFPW) "

3. Optional: verify all required methods are present:

::

  for m in SFAT SFMT SFMN SFPF SFPW GFPW; do
    grep -q "^${m} " /sys/class/cix/ec/asl_methods && echo "[OK] ${m}" || echo "[MISS] ${m}"
  done

If any required method is missing, fan control interfaces should be considered
unsupported on the current firmware build.

Node paths
----------

- `/sys/class/cix/ec/fan/fan_mode`
- `/sys/class/cix/ec/fan/fan_available_mode`
- `/sys/class/cix/ec/fan/fan_pwm`

Fan mode
--------

Read current mode:

::

  cat /sys/class/cix/ec/fan/fan_mode

Read available modes:

::

  cat /sys/class/cix/ec/fan/fan_available_mode

Set mode:

::

  echo mute > /sys/class/cix/ec/fan/fan_mode
  echo normal > /sys/class/cix/ec/fan/fan_mode
  echo manual > /sys/class/cix/ec/fan/fan_mode
  echo performance > /sys/class/cix/ec/fan/fan_mode

PWM control
-----------

Read PWM:

::

  cat /sys/class/cix/ec/fan/fan_pwm

Write PWM (format: `duty type index`):

::

  echo "40 0 0" > /sys/class/cix/ec/fan/fan_pwm
