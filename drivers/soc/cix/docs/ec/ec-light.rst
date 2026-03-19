EC Backlight and Light Sensor Usage
===================================

Node paths
----------

- `/sys/class/cix/ec/light/kbl`
- `/sys/class/cix/ec/light/lss`

Keyboard backlight (`kbl`)
--------------------------

Read (`percent enable`):

::

  cat /sys/class/cix/ec/light/kbl

Set backlight level (write percentage only):

::

  echo 30 > /sys/class/cix/ec/light/kbl

Light sensor (`lss`)
--------------------

Read (`type value`):

::

  cat /sys/class/cix/ec/light/lss
