# Quick Recovery After You Connect the Cable

Status: design. Not implemented.

## 1. Purpose

This document explains how JackBridge could start quickly after you connect the
ethernet cable again. It also tells why this function has no cost when you do
not use JackBridge.

## 2. The Problem

netJACK2 needs a wired link. If there is no cable, the `ExecCondition` in
`pi-stomp-jackbridge.service` stops the unit. The unit becomes inactive. This
is correct behaviour.

The Mac asks the pi again after a delay. The delay becomes longer at each
attempt. The maximum delay is 300 seconds. You can therefore wait 5 minutes
after you connect the cable. This is too slow.

## 3. The Rules

1. The recovery must be fast.
2. The recovery must have no cost when JackBridge is off.
3. The Mac must stay the source of truth.

Do not use a timer. Do not use a poll loop. Use an event.

## 4. How It Works

NetworkManager sends an event when an interface gets a carrier. A dispatcher
script on the pi receives this event.

1. The Mac writes `/run/jackbridge.wanted` when it asks the pi to start.
2. NetworkManager starts the dispatcher script at each carrier event.
3. The script looks for `/run/jackbridge.wanted`.
4. If the file is absent, the script stops immediately.
5. If the file is present, the script starts `pi-stomp-jackbridge.service`.
6. The `ExecCondition` makes the final check. If there is no wired link, the
   unit becomes inactive again.

Step 4 keeps rule 2. Step 5 keeps rule 1. The file keeps rule 3.

## 5. Why There Is No Cost

The script runs only at a carrier event. A carrier event is rare.

If you do not use JackBridge, the file is absent. The script stops after one
test. No process stays in memory. No timer runs. No packet moves.

The file is in `/run`. The file goes away at each boot. After a boot, the pi
does not start JackBridge until the Mac asks again.

## 6. Limits

The dispatcher is a second way to start the unit. Only the Mac writes the
file. Thus the pi starts the unit only if the Mac asked before. The Mac stays
the source of truth.

The `ExecCondition` stays necessary. A carrier can go up before the interface
has an IPv4 address. In that condition the unit becomes inactive again. The
Mac then asks again after the usual delay.
