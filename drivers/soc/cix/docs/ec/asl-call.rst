Manual ASL Method Invocation
============================

Node paths
----------

- `/sys/class/cix/ec/asl_methods` (read-only)
- `/sys/class/cix/ec/asl_call` (read/write)

List available methods
----------------------

Read the method list:

::

  cat /sys/class/cix/ec/asl_methods

Output format:

::

  METHOD_NAME ARGC

`ARGC` is the argument count. If not available, it is shown as `?`.

Invoke a method
---------------

Call format:

::

  echo "METHOD [arg1 arg2 ...]" > /sys/class/cix/ec/asl_call

Read result:

::

  cat /sys/class/cix/ec/asl_call

Argument parsing rules
----------------------

- Decimal integers are supported, for example `10`
- Hex integers are supported, for example `0x1A`
- If a token cannot be parsed as a number, it is passed as a string argument

Error cases
-----------

`asl_call` returns `error:` for cases such as:

- Method does not exist
- Argument count mismatch (when `ARGC` is available)
- ACPI method execution failure
