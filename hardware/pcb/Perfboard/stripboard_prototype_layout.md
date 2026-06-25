# Helmet Logger — Stripboard Prototype Layout

A hole-by-hole build plan for prototyping the helmet logger node on a
**solderable-breadboard style stripboard** (22 holes horizontal × 12 holes
vertical). Derived from the authoritative netlist in
`hardware/pcb/SMD/kart_smd.kicad_pcb` and the firmware pin map in
`src/main_logger.cpp`.

## Board topology

```
        C1  C2 C3 C4 C5 C6 C7 C8  C9 ........... C22
row1  [ ●==================  TOP rail (21) ===========]   <- col1 ● = left bus
row2  [ ●  ·  ·  ·  ·  ·  ·  ·  ·  · upper lanes ·   · ]
row3  [ ●  ·  ·  ·  ·  ·  ·  ·  ·  (each col = 4-hole  )]
row4  [ ●  ·  ·  ·  ·  ·  ·  ·  ·   vertical node,      )]
row5  [ ●  ·  ·  ·  ·  ·  ·  ·  ·   rows2-5)            )]
row6  [ ●==================  CENTER rail A (21) =======]
row7  [ ●==================  CENTER rail B (21) =======]
row8  [ ●  ·  ·  ·  ·  ·  ·  ·  ·  lower lanes          )]
row9  [ ●  ·  ·  ·  ·  ·  ·  ·  ·  (each col = 4-hole   )]
row10 [ ●  ·  ·  ·  ·  ·  ·  ·  ·   vertical node,      )]
row11 [ ●  ·  ·  ·  ·  ·  ·  ·  ·   rows8-11)           )]
row12 [ ●==================  BOTTOM rail (21) =========]
       col1 = one bus, all 12 holes (isolated from the 4 rails, which start at C2)
```

- **4 horizontal rails:** top (row1), center-A (row6), center-B (row7),
  bottom (row12) — each 21 holes, spanning C2–C22.
- **Left bus:** col1, all 12 holes, isolated from the rails.
- **Component field:** 4-hole vertical nodes — upper band rows 2–5, lower band
  rows 8–11.

**No track-cutting.** The copper pattern is fixed; build it like a breadboard —
drop parts into the 4-hole nodes and use the rails as buses.

## What lands on the board

Modules mount on female headers (swappable). The MAX98357 module already
contains the amp IC, output LC filter, decoupling, and mode resistor, so none of
those discretes are on the perfboard.

**Modules / connectors:** XIAO ESP32-S3, MAX98357 amp module, battery JST,
momentary wake button. The **GPS module (ATGM336)** connects with 4 flying wires
(VCC, GND, TX, RX) soldered to points on the board — it does not occupy a header
footprint.

**Discrete parts (through-hole substitutions):**

| Ref | Schematic part | Prototype substitution |
|-----|----------------|------------------------|
| Q1 | BC807-25 PNP | **BC327** (TO-92, 800 mA) |
| Q2, Q3 | MMBT3904 NPN | **2N3904** (TO-92) |
| R1, R3, R6 | 10k | 10k 1/4 W |
| R2 | 470 | 470 1/4 W |
| R4, R5 | 1M | 1M 1/4 W |
| C1 | 0.1 uF | 0.1 uF ceramic |

## Rail assignment

Four rails, four power nets, assigned so each band reaches its power with a
1-hole jumper:

| Rail | Net | Why |
|------|-----|-----|
| **row1 (TOP)** | **GND** | Upper-band modules (amp, GPS) put GND on this side |
| **row6 (CTR-A)** | **V_PERIPH** | Feeds amp Vin + GPS VCC, both in the upper band |
| **row7 (CTR-B)** | **+3V3** | Adjacent to XIAO's 3V3 pin (lower band) |
| **row12 (BOTTOM)** | **+BATT** | Adjacent to the sense divider + battery |
| **col1 (left bus)** | **GND spine** | Tie to TOP rail; gives GND anywhere on the left |

Upper band sits between GND (row1) and V_PERIPH (row6) — what amp + GPS need.
Lower band sits between +3V3 (row7) and +BATT (row12) — what the XIAO power pins
and sense divider need.

## Module placement (exact holes)

The XIAO straddles the center; its two pin-rows are 6 hole-pitches apart
(rows 2 and 8).

| Part | Row | Columns -> pins |
|------|-----|-----------------|
| **XIAO top** | row2 | C2=D0, C3=D1, C4=D2, C5=D3, C6=D4, C7=D5, C8=D6 |
| **XIAO bottom** | row8 | C2=5V, C3=GND, C4=3V3, C5=D10, C6=D9, C7=D8, C8=D7 |
| **MAX98357 module** | row3 | C10=LRC, C11=BCLK, C12=DIN, C13=GAIN, C14=SD, C15=GND, C16=Vin |
| **MAX98357 OUT+ anchor** | row8 | C14 (mechanical support pin — see below) |
| **GPS** | — | 4 flying wires, see *GPS wiring* below |

> ⚠️ Confirm the amp pin **order** against your module's silkscreen — vendors
> shuffle them. Columns above assume Adafruit-style amp ordering; if yours
> differs, slide the pin->column mapping.

### GPS wiring (4 flying wires)

| GPS wire | Solder to | = |
|----------|-----------|---|
| VCC | any free **row6** (V_PERIPH) hole | switched peripheral rail |
| GND | any free **row1** or **col1** hole | GND |
| TX  | `(9,C8)` | XIAO **D7 / RX** (lower C8 lane) |
| RX  | `(5,C8)` | XIAO **D6 / TX** (upper C8 lane) |

Both data wires land on column **C8**, the XIAO's D6 (top lane) and D7 (bottom
lane) — no cross-center jumpers needed.

> ⚠️ Confirm the ATGM336 pin order on its own silk; only the 4 nets above matter.

### Amp module mounting (OUT+ support pin)

The amp module is anchored by a second soldered pin on its **OUT+** pad so it is
not held by the input header alone. This pin sits 5 rows below the header, so
the whole module is dropped **one row down**: the input header lands on **row3**
(was row2) and the OUT+ pin lands on **row8, C14** — clear of the **+3V3 rail
(row7)**, where it would otherwise short the Class-D output into the supply.

Dropping the header to row3 changes **nothing electrically**: each upper-band
node is the 4-hole lane spanning rows 2–5, so a pin on row3 is the same node as
row2. All jumpers below remain valid.

> ⚠️ **Reserve `(C14, rows 8–11)` as an isolated node.** It carries OUT+ (the
> Class-D switching output, and where speaker+ taps if wired off the header).
> Nothing else may share that lane — the discrete network must avoid C14.

## Jumper list

**Power — upper band (all 1-hole):**
- amp GND `(row2,C15)->(row1,C15)`
- amp Vin `(row5,C16)->(row6,C16)`
- amp SD -> enable `(row5,C14)->(row6,C14)`
- (GPS VCC/GND are flying wires — see *GPS wiring*)

**Power — lower band:**
- XIAO 3V3 `(row8,C4)->(row7,C4)`
- XIAO GND `(row8,C3)->(row8,C1)` (into col1 GND)
- col1 <-> TOP GND tie: `(row1,C1)->(row1,C2)`
- Battery JST **+** -> any `row12` hole; **-** -> col1.
  Then short wires from the XIAO underside **BAT+ pad -> row12** and
  **BAT- pad -> col1**.

**Signals:**
- I2S: D0->DIN `(row4,C2)->(row4,C12)`, D1->BCLK `(row4,C3)->(row4,C11)`,
  D2->LRC `(row4,C4)->(row4,C10)`
- GPS UART: flying wires direct to the XIAO lanes — see *GPS wiring*.
- Wake button between **D8 `(row8,C7)`** and **GND (col1)**.

## Discrete network — part placement & routing

No cuts; place parts into the lower-band 4-hole nodes (rows 8–11) and wire by
node. Constraints that drive the layout:

- The rails are **never GND** (they are V_PERIPH / +3V3 / +BATT). GND only lives
  on **row1** and **col1**, so each block needs a **GND feed**: an insulated wire
  from the TOP rail down a column that is clear in both bands, landing on row8.
  The only such columns are **C9** (left of the amp) and **C17** (right of it) —
  C10–C16 are blocked by the amp body above.
- Put parts at **rows 9–11** to stay clear of the amp module's bottom edge (row8)
  and the reserved **C14** OUT+ node.
- Resistors may lie *across* one another in 3D — only the leg holes connect.

**Why sense-left / switch-right:** the battery-sense divider feeds the ADC (D9)
through 1M resistors, so keep it short and beside D9/D10 on the left. The switch
only needs a digital enable (D3), which tolerates the one long wire to the right.

### Battery sense — left block (columns C9–C13), divider ratio 2.0

| Part | Holes (row,col) | Connects |
|------|-----------------|----------|
| GND feed | `(1,C9)->(8,C9)` | TOP GND -> C9 lower lane |
| Q3 (NPN) | E=`(9,C9)`, B=`(9,C10)`, C=`(9,C11)` | E=GND (C9 lane) |
| R5 (1M) | `(10,C11)-(10,C12)` | Q3.C <-> SENSE |
| C1 (0.1uF) | `(11,C11)-(11,C12)` | parallel R5 |
| R4 (1M) | `(9,C12)-(12,C12)` | SENSE <-> +BATT rail |
| R6 (10k) | `(10,C10)-(10,C13)` | Q3.B <-> D10 landing (lies over R5) |
| D9 wire | `(8,C6)->(8,C12)` | BATT_SENSE -> SENSE node |
| D10 wire | `(8,C5)->(9,C13)` | VDIF_EN -> R6 |

SENSE node = lower lane **C12**.

### Peripheral switch — right block (columns C16–C22)

| Part | Holes (row,col) | Connects |
|------|-----------------|----------|
| D3 wire | `(5,C5)->(9,C16)` | PERIPH_EN -> R3 (the one long wire) |
| R3 (10k) | `(10,C16)-(10,C18)` | PERIPH_EN <-> Q2.B |
| Q2 (NPN) | E=`(9,C17)`, B=`(9,C18)`, C=`(9,C19)` | E=GND (C17 lane) |
| GND feed | `(1,C17)->(8,C17)` | TOP GND -> C17 lower lane |
| R2 (470) | `(10,C19)-(10,C21)` | Q2.C <-> Q1.B |
| Q1 (PNP) | C=`(9,C20)`, B=`(9,C21)`, E=`(9,C22)` | high-side switch |
| R1 (10k) | `(11,C21)-(11,C22)` | Q1.B <-> Q1.E (+3V3) |
| Q1.E jumper | `(8,C22)-(7,C22)` | Q1.E -> +3V3 rail |
| Q1.C jumper | `(8,C20)-(6,C20)` | Q1.C -> V_PERIPH rail (over +3V3) |

> ⚠️ **Transistor pinouts vary by manufacturer — check YOUR part's datasheet**
> (BC327 ships as both C-B-E and E-B-C depending on the maker; don't assume).
> Orient each part by *function*, not a fixed leg order:
> - **Q1 (PNP): Emitter → +3V3, Collector → V_PERIPH** (high-side switch — emitter
>   on the supply, collector on the load). Getting these reversed makes the rail
>   act as an emitter-follower: ~3.3V unloaded but collapses under any load.
> - **Q2, Q3 (NPN): Emitter → GND**, Collector to the load side, Base via its
>   series resistor.

> ⚠️ Verify the TO-92 leg order (BC327 for Q1, 2N3904 for Q2/Q3) against each
> datasheet before seating them — the pinouts are not all the same.

## Notes

- `V_PERIPH` is the **switched 3V3** rail off the XIAO LDO, shared with the
  ESP32. If the speaker is loud and 3V3 sags during testing, temporarily power
  the amp's Vin from **+BATT** (bypassing Q1) to confirm audio, then decide.
- Confirmed against the XIAO ESP32-S3 pinout: D8 = GPIO7 (RTC-capable, valid for
  `esp_sleep_enable_ext0_wakeup`) and A9 = GPIO8 (ADC1, no clash with D10=GPIO9).

## Reference pin map (firmware <-> schematic)

| Net | XIAO pin | Function |
|-----|----------|----------|
| I2S DIN / BCLK / LRC | D0 / D1 / D2 | MAX98357 audio |
| PERIPH_EN | D3 | Q2->Q1 high-side switch for V_PERIPH |
| GPS UART | D6 (TX) / D7 (RX) | GPS module |
| WAKE_BTN | D8 | Off/wake button, ext0 deep-sleep wake |
| BATT_SENSE | D9 (A9 ADC) | Battery sense divider, ratio 2.0 |
| VDIF_EN | D10 | Q3 gates the sense divider while measuring |
