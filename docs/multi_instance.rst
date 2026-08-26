Multi-instance launcher
=======================

SIPp can launch several SIPp processes from one CSV configuration file.  This
is useful when a test needs matching groups of UAC and UAS instances.

Use ``-multi`` with a CSV file:

.. code-block:: bash

   ./sipp -multi multi.csv -multi_base_port 5060

``-multi`` is a launcher mode.  When it is present, the launcher accepts only
``-multi`` and ``-multi_base_port``; put normal SIPp options in the CSV
``args`` field instead.  ``-multi_base_port`` cannot be used without
``-multi``.

The CSV format is:

.. code-block:: text

   role,count,args
   uas,2,"-sn uas -p {instance_port} -nostdin"
   uac,2,"-sn uac 127.0.0.1:{instance_port} -m 100 -nostdin"

Blank lines and lines whose first non-whitespace character is ``#`` are
ignored.  The header is optional and, when present, may use any letter case.
A configuration may launch at most 256 child processes in total.

Each row creates ``count`` child processes.  The ``args`` field is split once
into command-line arguments before placeholders are expanded.  This keeps
placeholder values containing spaces or quote characters as a single
argument instead of re-parsing them as shell syntax.

The launcher-only options ``-multi`` and ``-multi_base_port`` (including their
``--`` forms) are forbidden in child arguments.  Validation is performed after
placeholder expansion, so placeholders cannot be used to create a nested
launcher.  This prevents recursive configurations from bypassing the per-file
child-process limit.

The following placeholders are expanded in the ``args`` field:

* ``{role}``: the role column value.
* ``{instance}``: the zero-based instance number within that role.  If a role
  appears in more than one CSV row, numbering continues across those rows.
* ``{base_port}``: the value passed with ``-multi_base_port``.
* ``{instance_port}``: ``base_port + instance``.  Use this to pair UAC and UAS
  rows by instance number.
* ``{port}``: a globally increasing port number for every child process.

The launcher validates that all generated ports stay in the range 1 through
65535.  It waits until all children exit and returns the first non-zero child
exit code.  If a child cannot be forked, children already started by the
launcher are terminated and reaped before the launcher exits with failure.
If all children exit successfully, the launcher exits with zero.

When the launcher receives ``SIGINT``, ``SIGTERM``, or ``SIGHUP``, it forwards
a graceful termination to children, waits briefly, force-terminates any child
that remains, reaps them, and exits with ``128 + signal``.  This also prevents
children from being orphaned when the launcher is stopped by a service manager
or CI timeout.

Child processes always execute the same SIPp process image as the launcher.
The executable path is resolved from operating-system process metadata rather
than caller-controlled ``argv[0]`` or CSV data.  If the current executable
cannot be resolved safely, launcher mode fails closed instead of executing a
fallback path.

All children inherit the launcher's standard input, output, and error streams.
Multiple interactive SIPp screens will therefore interleave on one terminal.
Use ``-nostdin`` for children and redirect the launcher's output when a clean
terminal is needed, for example:

.. code-block:: bash

   ./sipp -multi multi.csv >multi.log 2>&1

A child may also use SIPp's ``-bg`` option when independent backgrounding is
desired; in that case the launcher only supervises the process until that
child backgrounds itself.
