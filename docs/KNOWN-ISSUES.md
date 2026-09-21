# Known issues

Things that are understood, worked around, and worth doing properly.

## A www update needed a restart the caller had to know about (fixed)

**Status: fixed.** The handler restarts itself once the SHA256 verifies, the
way the application update always did, and `Settings.vue` no longer sends a
restart of its own.

`POST /api/system/OTAWWW` used to answer `WWW update complete` as soon as it
had written the partition, while the running server kept serving the old
mount. `WWWVersion` read empty and every page was the built-in **WWW
Recovery** screen, which looks exactly like a failed update.

Correcting what this entry said before: the web interface was never affected.
`Settings.vue` always sent its own restart after a www upload, so a user
updating through the page would not have seen this. The exposure was every
other caller -- scripts, `curl`, anything posting to the endpoint directly --
which is how it was found and how it was tested.

An *interrupted* upload is still worth knowing about: it leaves the partition
damaged and the miner on the recovery page until another upload succeeds. The
API and mining are untouched throughout -- the board kept hashing at 6.2 TH/s
with no web interface at all -- so a second upload fixes it.

Verified by posting a container straight to the API with no client restart:
the answer is now `WWW update complete, rebooting now!`, and the miner returns
on its own at 87 seconds uptime serving the real interface.

**Separately, and not explained:** a large upload to either OTA endpoint
sometimes resets mid-transfer on a BC04. Retrying works.

This was first written up as an Ethernet-specific fault, on the strength of
two resets over Ethernet and two successes over WiFi. That was too small a
sample: an application upload over *WiFi* then reset the same way, and
succeeded on the retry. It is intermittent and not tied to an interface.

Nothing is damaged either way -- a partial write fails its checksum and is
rejected, and mining carries on -- but a www upload interrupted after the
erase leaves the miner on the recovery page until a second upload succeeds.
So: **retry, and check the version afterwards.**


## BC04 Ethernet had to be started after the hashboard (fixed)

**Status: fixed** in `main/network.c` and `main/main.c` -- Ethernet is started
after `init_all_peripherals()` rather than before it. Kept here because the
wrong diagnosis stood for a while and the reasoning is worth not repeating.

A BC04 brings its W5500 up cleanly: link, DHCP lease, DNS, NTP. Then the core
regulator switches on and **70 ms later** every socket command times out, for
good -- *if the controller was already running at that moment*. That last
clause is the whole thing, and it took far too long to test.

**Revisited 2026-09-04, and the original framing was too comfortable.** This
was written up as an ordering problem "and not the hardware fault it looked
like", on the reasoning that a power cycle always brought the part back. That
reasoning was wrong. A device that stops answering on a supply transient and
recovers only on a full power cycle is behaving like a part in latch-up, and
latch-up recovers on a power cycle right up until the run that destroys it.

That board's W5500 has since failed short across the 3.3 V rail -- which is
why the TMP75, the EMC2302 and the I2C pull-ups, all on that rail, went with
it. So the ordering was not merely confusing the software; it may well have
been stressing the part every time. See
[HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) section 2.6, and note what is *not*
established there: no log exists of this board on factory firmware while it was
healthy, so the correlation is not proven on this unit.

**The ordering is the vendor's, not this fork's.** Worth stating plainly,
because the obvious suspicion when a board dies under a third-party firmware is
that the third party moved something. In Baichuan's own published source, the
verbatim import at `878900f`:

```
main.c:293    network_init(&GLOBAL_STATE);
main.c:320    ESP_ERROR_CHECK(init_all_peripherals(&GLOBAL_STATE));
```

The network comes up 27 lines before the hashboard is powered. Every BC04
running factory firmware brings its W5500 up and then drops the core rail
transient on top of it, on every boot. This fork inherited that ordering and
later moved Ethernet after the transient, which reduces the exposure rather
than creating it.

Two things follow. The stress is a property of the product, so any BC04 that
boots with a cable plugged in has been taking it since it left the factory.
And a bench does more power cycles than a rack, so a board being worked on
takes the same event more often -- the cause is the ordering, but the count is
higher during development, and that is worth saying rather than leaving for
someone else to point out.

```
I (10658) vcore: Set ASIC voltage = 4.80V
E (10728) w5500.mac: w5500_send_command(210): send command timeout
E        esp_eth: eth_on_state_changed(151): ethernet mac set link failed
```

Nothing runs in software between those two lines -- the failure lands inside a
plain `vTaskDelay`, before the ASICs are reset or clocked. The interface keeps
its address and stops passing packets, so the miner looks networked and cannot
reach a pool.

Eliminated by test, not by argument:

| Theory | Result |
|---|---|
| SPI clock too fast for the bus | 16 MHz and 8 MHz behave identically |
| Driver task starved during power-up | `volc_delay()` is a plain `vTaskDelay` |
| Supply browning out | 12.30 V to 12.125 V under 85 W -- 1.4 % |
| GPIO or SPI bus conflict | Nothing else uses SPI2; power-on only writes I2C |
| Socket wedged, restart clears it | `esp_eth_stop()` cannot even reset the PHY |

Seventy milliseconds is the switching transient itself, and the driver reports
a link *state* change rather than only failed commands. That was read as the
16 A core regulator resetting or browning out the W5500 -- a hardware fault.

**It is not.** Every experiment above disturbed a controller that was already
running; none initialised one after the transient. Doing that works and keeps
working. Five eliminated hypotheses were mistaken for a complete set, and
"therefore hardware" followed from an argument rather than a measurement.

**What a real fix would look like.** `esp_eth_start()` only reopens socket 0.
It never re-runs `emac_w5500_init()`, which is what resets the chip, writes the
MAC into `SHAR` and puts socket 0 into MACRAW -- so a controller that came back
blank stays blank no matter how many times it is restarted. Recovery has to be
a full `esp_eth_driver_uninstall()` and re-init after the rail settles, which
also means retaining the mac, phy and netif-glue handles that
`example_eth_init()` currently drops on the floor.

**Do not start after a failed stop.** A failed `esp_eth_stop()` means the
driver never emitted `ETH_EVENT_STOP`, so the netif is still attached;
`esp_eth_start()` then re-enters `esp_netif_action_start` and trips
`assert failed: netif_add (netif already added)`, panicking the miner into a
reboot loop for as long as Ethernet is enabled. This was tried on hardware and
cost nine boots.

**Verified** on hardware at 750 MHz, both with WiFi alongside and with WiFi
switched off entirely -- the latter being what a freshly flashed miner is.
Ethernet-only: 6218 GH/s, 50 accepted shares, none rejected, no hardware
errors, the API served over the Ethernet interface, and the WiFi address
correctly gone.

`eth_on` and `wifi_on` are settable through `PATCH /api/system` and reported
by `/api/system/info`, so either can be changed without a factory restore --
which matters, because a factory restore also discards the WiFi credentials
that may be the only remaining way in.

Related, and worth fixing whatever the cause turns out to be: when Ethernet
takes a lease the firmware switches the setup access point off, and never
brings it back if Ethernet later dies. That is how a BC04 ends up holding an
IP address, passing nothing, with no way to reach it.

## Deferring Ethernet left a reboot loop behind it (fixed)

**Status: fixed** in `main/network.c`. Introduced by the fix directly above,
which is the point of writing it down.

Moving Ethernet after the hashboard fixed the W5500, and broke the case where
Ethernet is the only network that works. `network_init()` ends in a loop that
waits for an address from either interface and restarts the miner after five
minutes without one. Once Ethernet is deferred, that loop cannot ever see an
Ethernet address: the interface is not started until `main()` gets past the
call it is blocking inside.

So a BC04 with a good cable in it and an SSID it cannot reach -- an access
point that moved, a password that changed, a neighbour's network it once
joined -- waits five minutes, restarts, and does exactly the same thing again.
For ever, with a working cable plugged into it the whole time. Restarting
cannot help: the restart runs the identical sequence.

The wait is now short and ends in a `return` rather than a restart whenever
Ethernet is deferred: twenty seconds, which is long enough for WiFi to finish
DHCP if it is going to, then on to start the interface that is actually
connected. WiFi keeps retrying in the background, so an access point that is
slow or briefly away still joins afterwards. Nothing changes on a board where
Ethernet was never deferred -- a BC01 takes the same five minutes and the same
restart it always did.

The five-minute backstop is kept, moved to where it can still be satisfied.
It now runs from `network_settle_task()`, started once Ethernet is up, so the
window begins *after* the interface that might supply an address has been
given the chance to. That is the whole difference: the old backstop restarted
a miner that had never tried its cable, and the restart ran the same sequence
again. This one restarts a miner that has tried both, which is a state a
restart might genuinely clear -- a DHCP server that was not up yet, a switch
port still learning, a driver that came up wrong.

It holds its clock at zero for as long as somebody is connected to the setup
access point. That is where a miner with no network gets configured from, and
restarting out from under whoever is doing it is the one thing it must not do.

The same task also finishes the job `network_init()` no longer does. **Every**
deferred path leaves that function early, so the tidy-up at the end of its
wait loop -- drop the setup access point, quieten the WiFi log, tell the
system it is connected -- had stopped running, including on the path shipped
in 2.0.19. A miner that came up on the cable kept an open access point
broadcasting for as long as it was powered, and these images ship with a blank
API password, so that access point was an unauthenticated way in. Putting both
jobs in one task means the tidy-up runs however late the address arrives,
rather than only if it arrives inside some fixed window.

It is a task rather than a loop inside `network_eth_start()` so that main() is
not held for five minutes on a board with no cable in it.

**Not verified on hardware.** The only board here with a W5500 is the BC04,
and it is out for warranty repair with a shorted I2C bus. A BC01 cannot
exercise any of this: `network_init()` forces `eth_on` to 0 on that family, so
`eth_deferred` stays false and every line above is skipped. What a BC01 does
confirm is the part that protects it -- with `eth_deferred` false the timeout
constant is the same `5*60*5` it always was and the new branch is unreachable,
so its behaviour is unchanged. The fix itself needs a BC04 to sign off.

## The self test judges the fan before it checks for main power (fixed)

**Status: fixed** in `main/self_test/self_test.c`, after 2.0.20. Present in
2.0.20 and every earlier release. Affects the
BC04 directly; the BC01 family had the same defect through the USB-PD gate and
that half was fixed separately.

`self_test()` called `test_fan()` at `self_test.c:423`. It called
`test_power_on()`, which reads the regulator's input voltage, at
`self_test.c:480`. So the fan was measured and judged **57 lines before**
anything checked whether the board had its 12 V supply at all.

(Line numbers are as of 2.0.20, where the defect shipped. They have moved
since.)

The threshold already exists and is correct -- `test_power_on()` contains
`if (*vin < 11) ret = ESP_FAIL;` (`self_test.c:183`). It is simply read too
late to be useful. A fan test on an unpowered board fails first, the routine
returns, and the check that would have named the real problem never runs.

`tests_done()` then writes `selftest = 2` to NVS, and `should_test()` refuses
to run again: *"Self test previously failed; not repeating it"*. **The board
permanently records a hardware failure it does not have**, and there is no API
to clear the flag.

### Evidence

A BC04, serial `ALBC04441BF6D262A4`, flashed from the web flasher on
2026-09-04 and self-tested with no 12 V on the XT-30. Alongside a known-good
BC04 from the same firmware:

| | Powered (12 V on XT-30) | Unpowered |
|---|---|---|
| `READ_VIN` | 12.27 V | **3.93 V** |
| Fan 50% | 1642 rpm | 575 rpm |
| Fan 80% | 2292 rpm | 936 rpm |
| Fan 100% | 2693 rpm | 1135 rpm |
| Verdict | pass | `Fan test failed, -1, ESP_FAIL` |

The fan turns at roughly 42% of normal rpm at every step, which is what a fan
on an inadequate rail does. The verdict recorded was "FAN fail". The actual
fault was no main power.

### Confirmed by A/B on the same board

The owner then connected the 12 V supply and restarted. Same board, same
firmware, one variable changed:

| | 12 V absent | 12 V connected |
|---|---|---|
| `READ_VIN` | 3.93 V | **12.22 V** |
| ASICs detected | -- | **all four: Chip 0, 1, 2, 3** |
| Mining | no | **yes, 23 accepted shares** |
| Regulator temp | -- | 57 C, fan PWM 82 |
| Self test verdict | `Fan test failed` | *still* `Self test previously failed; not repeating it` |

**The board reports a permanent hardware failure while mining correctly with
all four ASICs.** That is the defect stated as plainly as it can be: the
verdict is wrong, it is sticky, and nothing the board does afterwards can
clear it.

It also settles what the fan reading meant. A fan that turns at 1135 rpm on an
inadequate rail and passes on a good one is not a faulty fan, and the self
test had no way to say so because it never looked at the supply.

Downstream symptoms on that board, all consistent and all misleading:

* the display stays on the splash screen, because the hashboard never comes up
  and the mining screen is never reached
* Ethernet works and pools can be configured, because the W5500 and the
  controller run from the logic rail and do not need the hashboard supply
* the self test never runs again, so connecting 12 V later does not clear it

### The fix

The supply is read before the fan test. Below `SELF_TEST_MIN_VIN` the self
test reports *"no main power"*, records `selftest = 3` and stops. Three rather
than two, because "could not be tested" and "failed" are different states and
only one of them describes a broken board. `should_test()` reports 3 by saying
what to do rather than announcing a fault that is not there.

Both the gate and `test_power_on()` now use `SELF_TEST_MIN_VIN`, so they
cannot drift apart about what "powered" means.

The result is still recorded rather than left at zero. The self test restarts
the miner afterwards, so retrying on every boot would leave an unpowered board
cycling every few seconds -- exactly what somebody flashing over USB does not
need. Clearing it stays a person's decision.

**Not verified on hardware.** Exercising it means booting a board with its
main supply disconnected, and the only working board here is mining. The
negative case -- that a powered board still self-tests normally -- is equally
untested.

The latch itself is right and should stay. It exists because writing the flag
only on success produced an unrecoverable restart loop on a genuinely faulty
board, with no web interface to see why. The fix is not to remove it but to
stop it recording the wrong verdict, and to add a way to clear it without
reflashing.

### Recovery on an affected board

The flag lives in NVS as `selftest`. **Connect the 12 V supply first**, then
reflash from the browser: the published full image carries a provisioning NVS
with `selftest=0`, so a reflash resets the flag and the test runs again with
power present. Reflashing without the supply connected reproduces the same
false failure.

Reflashing is the only way back. Nothing in the HTTP server reads or writes
`selftest`, so there is no API to clear it -- worth fixing, and not fixed
here.

(An earlier version of this paragraph said the flasher "writes a blank NVS".
It does not: `dist/stay-open-bc04-*-full.bin` carries `devicemodel=BC04` and
`selftest=0` preloaded at `0xe000`. The advice was right for the wrong
reason.)

## Two stratum defects found beside another change (open)

**Status: open.** Both pre-date the change that surfaced them and neither was
introduced by it. Recorded here rather than folded into 2.0.22, because
fixing unrelated memory bugs inside a release is how the last two regressions
in this project got out.

### `error_str` is never freed

`components/stratum/stratum_api.c` allocates `error_str` on the rejection
paths (around lines 224, 239, 245 and 260). Nothing in `main/` or
`components/` frees it.

In normal operation this is nothing -- a few bytes on the rare rejected
share. It matters in exactly the situation the placeholder-address diagnostic
exists for: a pool that refuses every `mining.authorize` on a sixty-second
retry loop leaks steadily and forever. A share-rejection storm leaks faster.

Fix by freeing it in the consumer after use, or by making ownership explicit
rather than implied.

### The settings handler can free a string stratum_task is using

`main/http_server/http_server.c` (around lines 849-852) does
`free(pool_user)` then `pool_user = strdup(...)`, from the HTTP task.
`main/tasks/stratum_task.c` takes a raw pointer to that same string into a
local `username` and holds it across the placeholder check and
`STRATUM_V1_authorize`.

A settings save landing exactly during a stratum reconnect can therefore free
the string mid-use. The window is microseconds and needs a save to coincide
with a reconnect, which is why nobody has hit it -- but it is a genuine
use-after-free, not a theoretical one.

Fix by `strdup`ing `username` after selecting it and freeing it after the
authorize, or by taking `global_parameter_mutex` around the config strings.

**Line numbers are as of 2.0.22 and will move.** Verify against current code
before changing anything.

## The radio came up before the supply it runs on (fixed)

**Status: fixed** in `main/main.c` and `main/device.c`. Affects the **BC01
family only** -- BC01, BC02, BC01_Pro, the boards powered over USB-C PD.
Present in every release up to and including 2.0.19.

`network_init()` brought the WiFi radio up at about **2 seconds**.
`bc01_pd_bringup()` did not negotiate the power contract until about
**12 seconds**, inside `init_all_peripherals()`. For those ten seconds the
board took its largest current step -- WiFi transmit -- on whatever USB-C
offers before a PD contract exists.

With the display module's own USB-C plugged into a computer, that second 5 V
source carries the step and nothing is visible. On its own, which is how a
miner actually runs, a BC01 with a replacement LilyGO module reset at five to
ten seconds and did it again on every boot, indefinitely.

**Our firmware made the miner require a USB connection in order to boot.**

It presented as location-dependent -- fine on the bench, a reboot loop in
another room -- which sent the diagnosis after WiFi signal strength for a
while. It was not the location. The bench had a USB cable in it and the other
room did not. Measured on the bench, controlling for that:

| Condition | Result |
|---|---|
| Module USB-C connected, cold boot | boots, mines |
| Module USB-C removed, already running | keeps running, 78 minutes unbroken |
| Module USB-C removed, software restart | boots fine -- rails never dropped |
| **Module USB-C removed, cold power cycle** | **reboot loop, 12 of 24 polls reachable** |

Only a genuine power-on reproduced it, which is why the first two attempts to
reproduce came back clean and briefly cleared the firmware.

The fix is ordering: `bc01_pd_bringup()` now runs before `network_init()`, so
the contract exists and VBUS is gated on before the radio is started. The
self test already did this, for the same reason -- everything behind the PD
gate is unpowered until the contract exists -- and the rest of the boot needed
it just as much. `bc01_pd_bringup()` is idempotent now, since two callers
reach it and the ladder walks the adapter down to 5 V and back on every run.

Verified on hardware, on the board that failed:

```
I (1609) device: PD Negotiation: Read adapter capability first, 5-15V
I (2709) device: Negotiation success: 15V
I (3409) device: VBUS output enabled
I (3589) wifi:mode : sta ...
```

Cold boot with no USB attached: no reboots across 30 polls, uptime continuous,
mining at 1565 GH/s. The same board, the same cable removed, looped on every
attempt before this change.

**Not a hardware-damage path.** The hashboard is not powered until about 14
seconds and the loop reset at five to ten, so the ASICs and the core regulator
were never energised. See [HARDWARE-SAFETY.md](HARDWARE-SAFETY.md).

**This did not affect the BC04.** That board takes 12 V on an XT-30 and has no
USB-PD path at all; `bc01_pd_bringup()` is gated behind
`device_is_bc01_family()`, which does not include it. Its boot logs contain
zero PD lines, healthy and faulted alike, against six on a BC01.

## A failed temperature read looked like a cold board (fixed)

**Status: fixed** in `components/bc_hal/TMP75.c`, `main/device.c` and
`main/tasks/health_maintennance.c`. Present in every release up to and
including **2.0.19**. Full write-up, including exposure by board and
verification status, in [HARDWARE-SAFETY.md](HARDWARE-SAFETY.md).

`TMP75_read_temperature()` reports a failed I2C read as **-60 C**, and
`read_hash_board_temperature()` stored that and returned `ESP_OK` regardless.
Nothing upstream could tell a dead sensor from a very cold board, and both
consumers of the number ran the wrong way:

* thermal protection compares against 71 C, so `-60 > 71` is false and it
  never tripped
* the fan curve compares against `MIN_FAN_TEMP` (30 C), so `-60` put the fan
  at `MIN_PWM_PERCENT`, **18%**

A sensor or bus failure therefore made the firmware cool the board *less*
while it kept hashing at full power, with nothing watching the heat.

The BC01 is the more exposed board. On a BC04 fan RPM comes over the same I2C
bus, so a total bus loss already aborted the health loop and rebooted -- crude,
but it stopped the mining; the exposure there is a partial failure, the TMP75
at 0x48 dead while the EMC2302 at 0x2e still answers. A BC01 reads RPM from a
pulse counter, so nothing aborts and it would sit at 18% fan, blind.

Inherited from upstream, so stock firmware very likely carries it too. It
ships here, so it is fixed here.

`TMP75_get_temperature()` can now report failure, `read_hash_board_temperature()`
returns an error, and the health loop takes the overheat exit -- power off, fan
100%, restart -- after three consecutive failures, about six seconds. Three
rather than one, because a single dropped transaction is not a dead sensor.

**Not verified on hardware.** The only BC04 here has a shorted I2C bus, and the
whole health loop is gated behind `interface_initalized`, which that board
never sets. It needs a working board with an induced sensor failure.

## Switching off an access point that was never on (fixed)

**Status: fixed** in `components/connect/connect.c`, with a guard in
`main/network.c`.

`wifi_softap_off()` called `ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA))`
unconditionally. With `wifi_on=0` there is no WiFi stack, the call answers
`ESP_ERR_WIFI_NOT_INIT`, and `ESP_ERROR_CHECK` aborts:

```
ESP_ERROR_CHECK failed: esp_err_t 0x3001 (ESP_ERR_WIFI_NOT_INIT)
file: "./components/connect/connect.c" line 256
func: wifi_softap_off
```

On an Ethernet-only miner that is a reboot loop with no way in -- come up, take
a lease, try to tidy away an access point that does not exist, panic, repeat.

The abort was latent for any `wifi_on=0` configuration. What made it reachable
was the settle task added with the Ethernet stall watchdog, which drops the
setup access point once Ethernet has an address. Found the same day by
actually running the Ethernet-only configuration rather than assuming it
worked, which is the argument for testing the fault configurations and not
only the healthy one.

Both softap toggles now treat an absent WiFi stack as a configuration rather
than an error, and the settle task checks `wifi_on` before asking. Verified on
hardware, Ethernet-only on a BC04.

## Peripheral init could strand a board with no way in (fixed)

**Status: fixed** in `main/main.c` and `main/network.c`.

`init_all_peripherals()` talks to the hashboard over I2C and, on a board whose
bus is dead, does not return -- measured at over four minutes on the BC04 here,
with every API call printing `i2c handle not initialized`. Everything after
that call is unreachable, including the Ethernet start, so a miner in that
state with no WiFi credentials has no management interface at all.

A watchdog now starts Ethernet if init has not finished in 90 seconds. It
waits on `interface_initalized` rather than running on a bare timer, because
the reason Ethernet is deferred at all is that a W5500 already running when the
core rail steps up stops answering for good. Ethernet therefore starts either
after the hashboard is up or after it is known not to be coming up, never in
between. A healthy BC04 sets the flag around 35 s.

Verified on the failed BC04, with and without WiFi:

```
E (104159) serpentx: Peripheral init has not finished in 90 s -- the hashboard
                     is not coming up. Starting Ethernet anyway
I (106189) NETWORK: Ethernet Link Up
I (108689) esp_netif_handlers: eth ip: 192.168.50.31
```

A DHCP lease needs a full four-way exchange, so this also established that the
failed board's W5500, SPI bus and PHY all work and the fault is confined to the
I2C and hashboard domain.

## Ant Design is handed CSS variables it cannot read

**Where:** `main/http_server/axe-os/src/pages/App.vue`, the `a-config-provider`
theme block.

The theme is built on CSS custom properties, and the Ant Design tokens are set
to those properties as strings:

```js
colorPrimary: 'var(--ant-primary-color)',
colorInfo:    'var(--ant-primary-color)',
colorBgBase:  'var(--surface-ground)',
```

Ant Design v4 computes its palettes in JavaScript — it derives hover states,
active states, borders and contrasting text from `colorPrimary` before any CSS
is involved. A custom property is opaque to that, so every derived colour comes
out unusable. What reaches the page is whatever its fallback produces, which is
usually close to the surrounding background.

This is not a styling preference. It is a whole class of defects, and it has
produced at least four:

- the selected card tab rendered as an empty box, because Ant Design paints the
  label on an inner `.ant-tabs-tab-btn` and gave it a colour close to the tab's
  own background — the text was present and invisible
- an enabled switch looked identical to a disabled one, so there was no way to
  see whether dual mining was on
- the unofficial-firmware notice in the Firmware Update card rendered as an
  empty yellow box: `colorText` reached `.ant-alert-message` as near-white
  and Ant painted the warning surface from its own static palette, giving
  #f5f7fb on #fffbe6, a contrast ratio of 1.03. Found in September 2026 by
  looking at the page, a release after the notice was added and verified
  present in the compiled asset -- which was never the same question as
  whether it could be read
- anything else derived from the accent colour is suspect until checked

**Current state:** the four known cases are corrected in
`main/http_server/axe-os/src/styles/layout/_antd-fixes.scss`, which sets the
affected properties in ordinary CSS where the variables resolve correctly. That
file is a patch over the cause, not a fix for it, and it will grow every time
another component turns out to derive a colour the same way.

The alert rule there is written for `.ant-alert-warning` and
`.ant-alert-error` as a whole rather than for the notice that exposed it.
Seven other alerts share the markup, and every one of them is rendered only
on a fault -- an unreachable miner, a failed save, a pool that will not
connect -- so they were unreadable exactly when they had something to report,
on screens nobody looks at while the miner is healthy.

**The real fix** is to stop passing custom properties into the token block and
give Ant Design real colour values, driven from the same source as the CSS
variables so the two cannot drift:

- keep the palette in one place as literal values, in TypeScript
- derive the CSS custom properties from it, and pass the same literals to
  `a-config-provider`
- re-provide the tokens when the theme changes, so switching theme updates both
  halves together

The theme selector offers several accents, so whatever replaces this has to
survive a theme change at runtime rather than being read once at start-up.

**Why it was not done at the time:** it lands in the middle of the component
that mounts the whole interface, on a day that had already produced three
firmware releases. Rewriting the theme system while shipping is how something
breaks quietly. It wants a clear run and a careful look at every component that
reads an accent colour.

## The global stylesheet is compiled twice

**Where:** `main/http_server/axe-os/src/pages/App.vue`, line 305.

`main.ts` imports `styles/layout/layout.scss` globally. `App.vue` then imports
the same file again inside a `<style scoped>` block, so every rule in it is
emitted a second time carrying that component's scope attribute.

Two consequences. The CSS payload is roughly double what it needs to be. And
the scoped copies cannot reach anything Ant Design renders in a portal — a
dropdown, a modal — which is the same trap that left the pool logos unstyled
for as long as they were.

The fix is to delete the `@import` from `App.vue`; the global import in
`main.ts` already covers it. It is one line, but it is in the component that
mounts the entire interface, so it wants a build and a look at the running UI
rather than being done in passing.

Found while checking that the corrections in `_antd-fixes.scss` had reached the
device: the same rules appeared twice in the built bundle, once with a scope
attribute and once without.

## Published binaries carried the blob this repository says they do not (fixed)

**Where:** `components/asic/CMakeLists.txt`, `components/asic/asic.c`.

`lt0051.c` was compiled into every build, and it is the only caller of
`components/a/liba.a`, a prebuilt archive with no source. No BC board ever
runs it — the dispatch arms it sits behind are chosen by device model, and no
BC board reports an LT0051 part — but a linker resolves references, not
reachable calls. Compiling the file was enough. Eighty-two sections of that
archive sat at live flash addresses in every published BC01 and BC04 image
from 2.0.4 to 2.0.25, about 20 KB of it.

Three documents said the opposite: `README.md`, `docs/ASIC-ABSTRACTION.md`
and `docs/FINDINGS.md` all described the BM1370 path as blob-free. So the
releases conveyed object code with no corresponding source — the same defect
this project raises against the vendor — while the repository asserted they
did not.

Found by an external review of the link map in September 2026. Not by us, and
not by any of the verification this project had been doing, because none of it
looked at what the linker actually placed.

**The fix:** the driver is behind `CONFIG_STAYOPEN_ASIC_LT0051`, off by
default; the dispatch arms resolve to stubs when it is off. Component `a`
stays in `REQUIRES` because `common.c` includes a header of `#define`s from
it, which contributes no code.

**How it was checked:** the link map, not the source. Before, 82 `liba.a`
contributions at live addresses; after, one `LOAD` line and no member
extracted, and the image 20,608 bytes smaller. Anyone changing this should
re-read the map rather than trusting the Kconfig default.

## An eleventh reject reason corrupted the system module (fixed)

**Where:** `main/system.c`, the reject-reason tally.

The bound was `sizeof(module->rejected_reason_stats)` on an array of ten
68-byte structs, so the limit read 680 rather than 10. The eleventh distinct
reject string wrote past the end, and the field immediately after the array is
the count itself: it took the first four bytes of the message — around half a
billion — and the `qsort` below then ran over that many elements.

It needed eleven *distinct* strings in one session, which sounds unlikely
until you meet a pool that puts the job id in the reject reason, at which
point every stale share is a new string and this is a matter of time rather
than of luck.

## Two endpoints answered anyone who asked (fixed)

**Where:** `main/http_server/http_server.c` (`GET_wifi_scan`),
`main/http_server/theme_api.c`.

Every other endpoint that does something opens with `is_network_allowed()`
and `api_auth_require()`. These two had neither.

`GET /api/system/wifi/scan` returned the list of nearby SSIDs to anyone on the
network — a map of the owner's RF neighbourhood — and being a plain GET, a web
page open in the owner's browser could start a scan cross-origin on the radio
the miner needs for its pool connection.

`POST /api/theme` wrote NVS with no authentication at all. A JSON body sent as
`text/plain` is a CORS simple request and needs no preflight, so any page
could rewrite the stored theme without credentials. Given the entry above —
accent colours rendering alerts unreadable — that is a way to hide the
warnings that report a fault, not merely a cosmetic nuisance.

Both now require a session. The theme GET is deliberately left open: the
interface reads it before anyone signs in, and it discloses only which colours
the owner likes.

## The fan-fault protection could not fire (fixed)

**Where:** `main/tasks/health_maintennance.c`, `components/bc_hal/pwm_fan.c`.

`force_fan_check` is declared false and never assigned, so the check never
runs. It is worse than that: `check_fan_ok()` is passed `max_fan_speed = 0`,
and both of its comparisons are against that value, so the function returns
true for any RPM including zero. Enabling the flag on its own would change
nothing.

On a BC04, `EMC2302_get_fan_speed()` also discards the return values of its
two channel reads and reports `ESP_OK` with 0 RPM when the controller is
absent, so a dead fan controller and a stopped fan are indistinguishable.

**The fix**, in three parts, because one alone would have changed nothing:

`EMC2302_get_fan_speed()` now reports a failed read instead of returning
ESP_OK with zero, so a dead controller and a stopped fan are no longer the
same answer. The caller no longer wraps that read in `ESP_ERROR_CHECK`, which
panicked -- turning a fault the firmware could have acted on into a reboot
loop, and a panic is the one response that guarantees nobody turns the heat
off first. Three unreadable samples now take the same exit as an unreadable
temperature.

The curve check is replaced by a narrower question, because a false positive
here powers down a working miner: a fan being driven at 30% or more and
reporting no rotation at all, for five consecutive checks -- ten seconds at
this loop's period. A stopped or disconnected fan reads zero; a merely slow
one does not, and is left to the over-temperature trip, which is the thing
that actually matters and is known to work.

It only judges a channel that has reported a non-zero speed since boot. A
board with no tachometer reads zero forever, and without that latch this
would power one off ten seconds after it started.

**Corrected 2026-09-11, before release, by a log from a working BC04.** That
latch was a single flag for the whole board rather than one per channel, so
one turning fan vouched for every other channel. A stock BC04 reports
`Fan0: 0 RPM | Fan1: 3688 RPM` -- one fan fitted, or one tachometer not wired
-- and under the original version Fan1 would have proved the tach "works" and
Fan0's zero would then have read as a stall. A shutdown on a healthy miner,
ten seconds in, which is the one outcome this check exists to avoid. Latched
per channel now.

**What this check still cannot see on a BC04.** `read_fan_rpm()` stores
`EMC2302_get_fan_speed()`, and that returns the *larger* of the two channels.
So one dead fan out of two is invisible: the survivor's reading is what
reaches the health loop, and nothing looks wrong. The stall check only ever
sees a board with no working fan at all.

Fixing it means reporting both channels separately and deciding what a
single-fan failure should do, which is a design question rather than a typo --
on a board that may legitimately have one fan fitted, treating a zero as a
fault is exactly the false positive corrected above. Left open deliberately,
and recorded so nobody reads the entry above as more protection than it is.

## A BC04 cannot de-energise its own hashboard once I2C is gone (open, mitigated)

**Where:** `main/device.c`, `power_off_hashboard()`.

The regulator sits on the I2C bus. With the bus dead there is no way to
command the output off, and `power_off_hashboard()` returns `ESP_OK`
regardless of whether `TPS546_set_vout(0)` succeeded, so no caller can even
tell.

The BC01 family has a second lever the firmware does not use as a fallback —
closing the USB-PD gate would cut VBUS behind it — but the BC04 has no gate.

A miner showing an empty bus scan, a board temperature of −60 °C, or a reboot
every four minutes should have its power removed by hand.

**What changed:** `power_off_hashboard()` returned ESP_OK whatever happened,
so every protection path logged "cut off the power" and waited a hundred
seconds without knowing whether it had. It now reports the failure, tries the
USB-PD gate on the BC01 family -- a different bus, so it survives the I2C
failure that took the regulator away -- and, when there is nothing left to
try, says so in as many words: the hashboard may still be powered, remove
power by hand.

That does not fix the BC04, which has no gate. It replaces a silent failure
with a stated one, which is the whole of what this firmware can do on that
board.

**It did not cause the failure of the BC04 in this project's evidence
bundle.** The log shows four I2C devices responding, Ethernet up with an
address, then `vcore power on hashboard, set voltage 480` and the first W5500
error 70 ms later. The bus was healthy when the damage happened, so the
firmware had full control of the regulator; the same board had cleanly
executed `vcore power off hashboard, set voltage 0` on the preceding restart.
The dead board then reported `input_voltage: 0` -- nothing left to switch off.
This limitation is downstream of that fault, not upstream of it.

## The core-voltage cap could be stepped over four ways (fixed)

**Where:** `main/nvs_device.c`, `main/system.c`, `main/http_server/http_server.c`.

The vendor caps a BC04 at 4.80 V across four BM1370s in series, and
`nvs_device.c` applied that — to the startup default, and nowhere else.
`CONFIG_TPS546_VOUT_MAX` on that board is 5.20, and every other path checked
against *that*:

- the boot-mode config lifted `asic_vol_default` to whatever `asicnormalvol`
  held, after a `min..max` check, so one NVS key brought the board up over the
  cap;
- all four voltage fields in the settings handler — `coreVoltage`,
  `coreNormalVoltage`, `coreOverVoltage`, `asicovervdef` — accepted anything
  inside `min..max` and applied it live, storing it as the value the miner
  comes back up on.

So the cap could be exceeded by 8% with a single request, on a four-chip
string, and the miner would keep that setting across restarts.

Whether 1.30 V a chip damages a BM1370 is not something this repository can
answer, and nothing here claims it does. That a cap existed and did not hold
is answerable, and is the defect.

**The fix:** `device_core_voltage_ceiling()` in `device.c` returns the model's
real ceiling — 480 on BC04 and BC08, `asic_vol_max` elsewhere — and all five
sites ask it. Found by an external review, not by us.


## The web interface stopped answering while the miner kept hashing (fixed)

**Where:** `main/http_server/http_server.c`, the server configuration.

Reported by an owner running a BC04 on this firmware: after some hours the
interface became unreachable while the miner carried on mining normally. Their
log caught it, twenty hours into a run -- the websocket carrying it died
moments after the server accepted another log client.

`max_open_sockets` is 8 and `lru_purge_enable` was never set, so it defaulted
to false. At eight held sockets the server refuses new connections instead of
reclaiming the oldest, and it holds them for a long time: a browser tab killed
without a close, a laptop asleep mid-request, a phone out of range. None of
those send a FIN, so the slot stays occupied until the receive timeout expires.
Several of them and the interface is simply gone.

Mining never notices, because the stratum tasks own their own sockets. That is
what makes this so hard to diagnose from outside: the miner is plainly
working, and the web interface has vanished, so the natural suspicion falls on
the network rather than on the miner.

`lru_purge_enable = true` now, which is what it should always have been.

**Not the same thing, and worth separating:** the same owner had set the same
static IP on both the WiFi and Ethernet interfaces, to be reachable at one
address whichever link was up. This firmware runs both interfaces at once --
only the soft-AP is shut down, never the station -- so that puts two MACs on
one address on one subnet, and upstream ARP tables flap between them. The
symptom is intermittent unreachability that looks exactly like the bug above.
A DHCP reservation upstream is the way to get one stable address; assigning it
twice is not.
