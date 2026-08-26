# JACK server conflicts

## Goal

The Companion must start correctly when a JACK server is already there.
The Companion must not stop a JACK server that belongs to a different
program.

## Facts

These three facts come from tests on a Mac.

1. Our `jackd` stops immediately if a JACK server with the name `default`
   is already active. But `jackd-launch` continues. Its ready test at line
   133 gets an answer from the other server. Then line 140 loads
   `netmanager` into that other server.
2. `jackbridge-ctl stop` and the installer script stop processes with the
   pattern `jackd .*coreaudio`. This pattern also finds the JACK server of
   a different program. MOD Desktop on this Mac uses such a server.
   The menu item "Restart JackBridge" thus can stop a different program.
3. macOS shows the environment of a process of the same user. The command
   `ps -Eww -p <pid>` shows it. Thus we can put a mark on our JACK server.
   The name of the server stays `default`.

## Scope

### 1. Put a mark on our JACK server

Export the variable `JACKBRIDGE_INSTANCE` in `jackd-launch`. Do this before
`jackd` starts. Give it a new value at each start. Read the value again
with `ps -Eww`. Clients of the JACK server do not see this mark.

### 2. Test for a server before you start one

Do this test in `jackd-launch` before `jackd` starts. `jack_lsp` gives an
answer only if a server is there. Then find the process of that server and
read its mark. There are three results:

- No server. This is the usual condition.
- A server with our mark. Our server continued after a crash.
- A server without our mark. The server belongs to a different program.

### 3. Do the correct operation for each result

- No server: start `jackd` as usual.
- Our server: the menu bar app shows a dialog. The dialog has the buttons
  "Quit Other Server" and "Cancel". If the user selects the first button,
  `jackbridge-ctl` stops the old server. Then JackBridge starts.
  If the user selects "Cancel", JackBridge does not start.
- A different program's server: do not stop it. Do not load `netmanager`
  into it. Write the condition to a status file. Then wait and do the test
  again. If the other server stops, start as usual.

### 4. Make the stop commands safe

Change `jackbridge-ctl` and the installer script. Do not use the pattern
`jackd .*coreaudio`. Stop only a process with our mark. This is also
necessary for the menu item "Restart JackBridge".

### 5. Show the condition in the menu

The menu shows a different line for each condition. Examples:
"A different program uses JACK" and "JackBridge waits for JACK".

## Acceptance criteria

- Stop `jackd-launch` but keep its `jackd`. Then start the LaunchAgent.
  The app shows the dialog. Select "Quit Other Server". JackBridge starts.
- Start a JACK server with `jackd -d dummy`. Then start JackBridge.
  JackBridge does not load `netmanager` into that server. The menu shows
  the correct line. Stop the other server. JackBridge starts without help.
- Start a JACK server with `jackd -d dummy`. Then select "Restart
  JackBridge" in the menu. The other server continues to operate.
- `jackd-launch` does not start again and again in this condition. The log
  gets not more than one message each 10 seconds.

## Constraints

Do not change the name of the JACK server. Other programs must connect to
the server with the name `default`. Do not stop a process that we did not
start. Keep the start and stop operations in `jackbridge-ctl`. The app only
shows the dialog and calls that command.

## Not in this plan

- Use of a JACK server of a different program for audio.
- A test that the JACK2 at `$JACK_PREFIX` is our fork.
- Marks that use the JACK metadata interface. That interface does not
  operate in this build of JACK2.
