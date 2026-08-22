"""Reference schematics for the three pinled boards (HARDWARE.md is the
source; the hardware repo remains the layout authority)."""
import schemdraw
import schemdraw.elements as elm
import schemdraw.logic as logic

def reg165():
    # Fresh IcPin objects every call: schemdraw mutates pins during layout,
    # and a shared list renders later instances mirrored.
    return dict(pins=[
        elm.IcPin(name='A..H', side='left', slot='2/2'),
        elm.IcPin(name='SER', side='left', slot='1/2'),
        elm.IcPin(name='CLK', side='bottom', slot='1/2'),
        elm.IcPin(name='/PL', side='bottom', slot='2/2'),
        elm.IcPin(name='QH', side='right', slot='1/1')],
        edgepadH=1.0, edgepadW=.5)

# =================================================================== LAMP ===
with schemdraw.Drawing(show=False) as d:
    d.config(fontsize=11)
    d += elm.Label().label('pinled — Lamp board (per-socket PCB, replaces one bulb)',
                           fontsize=15).at((0, 11.8))

    # --- socket / tap / phantom load (left half) ---
    d += elm.Dot(open=True).at((0, 8)).label('SOCKET +\n5–20 V AC/DC', loc='left', fontsize=9)
    d += elm.Line().right(2)
    t1 = d.add(elm.Dot())
    d += elm.Line().right(2.5)
    d += elm.Dot(open=True).label('TAP → J3\n(to module channel)', loc='right', fontsize=9)

    d += elm.Switch().at(t1.center).down(1.6).label('JP1', loc='left', ofst=(.4,-.5), fontsize=9)
    d += elm.Resistor().down(2.2).label('R1 470 Ω 2 W\nphantom load', loc='left', ofst=(.55,.35), fontsize=9)
    b1 = d.add(elm.Dot())
    d += elm.Line().at(b1.center).left(2)
    d += elm.Dot(open=True).label('SOCKET −', loc='left', fontsize=9)
    d += elm.Line().at(b1.center).down(0.5)
    d += elm.Ground().label('machine\nlamp return', loc='bottom', fontsize=8)
    d += elm.Label().at((-1.2, 5.1)).label('fit JP1 for SCR machines /\nunregulated rails only', fontsize=8, halign='left')

    # --- WS2812 (right half) ---
    ws = d.add(elm.Ic(theta=0, pins=[
        elm.IcPin(name='DIN', side='left', slot='1/1'),
        elm.IcPin(name='DOUT', side='right', slot='1/1'),
        elm.IcPin(name='VDD', side='top', slot='1/1'),
        elm.IcPin(name='GND', side='bottom', slot='1/1')],
        edgepadW=.7, edgepadH=.5).at((9.5, 7.2)).label('D1\nWS2812B-5050', loc='bottom', ofst=(1.9, -0.3), fontsize=10))

    d += elm.Dot(open=True).at((6.4, 10.6)).label('5 V string (J4.1)', loc='left', fontsize=9)
    d += elm.Line().tox(ws.VDD[0])
    d += elm.Line().toy(ws.VDD)
    # decoupling on the far side of the pixel, where nothing else routes
    cnode = d.add(elm.Dot().at((ws.VDD[0], 10.6)))
    d += elm.Line().at(cnode.center).right(1.8)
    d += elm.Capacitor().down(1.1).label('C1\n100 nF', loc='left', ofst=(.45,0), fontsize=9)
    d += elm.Ground()

    d += elm.Dot(open=True).at((5.6, ws.DIN[1])).label('DIN (J4.3)', loc='left', fontsize=9)
    d += elm.Line().to(ws.DIN)
    d += elm.Line().at(ws.DOUT).right(1.5)
    d += elm.Dot(open=True).label('DOUT (J5.3)\nnext lamp board', loc='right', fontsize=9)
    d += elm.Line().at(ws.GND).down(0.5)
    d += elm.Ground().label('string GND (J4.2 / J5.2)', loc='bottom', fontsize=8)

    d += elm.Label().at((0, 1.0)).label(
        'Tap and string grounds meet only at the single-point tie (HW-6).  JP1+R1 do the removed bulb\'s\n'
        'two electrical jobs: SCR holding current and unregulated-rail loading (front-end checklist).',
        fontsize=9, halign='left')
    d.save('docs/schematics/lamp-board.svg')
print('lamp ok')

# ============================================================== EXPANSION ===
with schemdraw.Drawing(show=False) as d:
    d.config(fontsize=10)
    d += elm.Label().label('pinled — Driver expansion board (16-channel sense module, chainable ×1..8)',
                           fontsize=15).at((0, 15.2))

    # --- one front-end channel, left to right on one row (y=12) ---
    d += elm.Dot(open=True).at((0, 12)).label('CH0 TAP\n(lamp board J3)', loc='left', fontsize=9)
    d += elm.Resistor().right().label('R_a', loc='top', fontsize=9)
    div = d.add(elm.Dot())
    d += elm.Resistor().at(div.center).down().label('R_b', loc='right', fontsize=9)
    d += elm.Ground()
    d += elm.Diode().at(div.center).right().label('D  half-wave', loc='top', fontsize=9)
    dn = d.add(elm.Dot())
    d += elm.Zener().at(dn.center).down().label('3V3\nclamp', loc='right', fontsize=9)
    d += elm.Ground()
    d += elm.Line().at(dn.center).right(0.7)
    d += elm.Resistor().right().label('R_g gate', loc='top', fontsize=9)
    fet = d.add(elm.NFet(bulk=False).anchor('gate').label('Q1\n2N7000', loc='right', ofst=(0.9,-0.9), fontsize=9))
    d += elm.Ground().at(fet.source)
    d += elm.Resistor().at(fet.drain).up(1.6).label('R_d\npull-up', loc='right', ofst=(.3,0), fontsize=9)
    d += elm.Vdd().label('3V3', fontsize=9)
    drain = d.add(elm.Dot().at(fet.drain))
    inv = d.add(logic.SchmittNot().right().anchor('in1').at((fet.drain[0]+2.2, fet.drain[1]))
                .label('U3 74LVC14 (3 per module)', loc='bottom', ofst=(1.2, -0.9), fontsize=9))
    d += elm.Line().at(drain.center).to(inv.in1)
    d += elm.Line().at(inv.out).right(0.8)
    d += elm.Dot(open=True).label('→ U1.A (ch0)', loc='right', fontsize=9)
    d += elm.Label().at((0, 9.0)).label(
        'front end ×16, identical.  FET inverts, Schmitt inverts back: lamp on = logic high (HW-1).',
        fontsize=9, halign='left')

    # --- LDO, clear at top right ---
    ldo = d.add(elm.Ic(theta=0, pins=[
        elm.IcPin(name='IN', side='left', slot='1/1'),
        elm.IcPin(name='OUT', side='right', slot='1/1'),
        elm.IcPin(name='GND', side='bottom', slot='1/1')],
        edgepadW=.6, edgepadH=.4).at((16.5, 13.4)).label('U4 3V3 LDO\n(local, HW-8)', loc='top', ofst=.2, fontsize=9))
    d += elm.Dot(open=True).at((14.6, ldo.IN[1])).label('5 V\n(J1.1↔J2.1)', loc='left', fontsize=9)
    d += elm.Line().to(ldo.IN)
    d += elm.Line().at(ldo.OUT).right(0.7)
    d += elm.Vdd().label('3V3 (logic only)', fontsize=9)
    d += elm.Ground().at(ldo.GND)

    # --- shift registers + chain (pattern that rendered well) ---
    u2 = d.add(elm.Ic(**reg165()).right().at((5.5, 4.5)).label('U2 74LVC165\nch 8–15', loc='top', ofst=.2, fontsize=9))
    u1 = d.add(elm.Ic(**reg165()).right().at((11.5, 4.5)).label('U1 74LVC165\nch 0–7', loc='top', ofst=.2, fontsize=9))
    d += elm.Wire('c', k=1).at(u2.QH).to(u1.SER).label('QH→SER', fontsize=8, ofst=(0,-.25))

    d += elm.Dot(open=True).at((0.4, u2.SER[1])).label('J2.3 DATA\n(downstream in)', loc='left', fontsize=9)
    j2 = d.add(elm.Line().right(1.6))
    d += elm.Line().to(u2.SER)
    d += elm.Resistor().at(j2.end).down(2.2).label('R1 10 k', loc='right', fontsize=9)
    d += elm.Ground()
    d += elm.Resistor().at(u1.QH).right().label('33 Ω (HW-5)', loc='top', fontsize=9)
    d += elm.Dot(open=True).label('J1.3 DATA\n(toward master)', loc='right', fontsize=9)

    yclk, ypl = 1.4, 0.7
    d += elm.Line().at(u2.CLK).toy(yclk)
    c2 = d.add(elm.Dot())
    d += elm.Line().at(u1.CLK).toy(yclk)
    c1n = d.add(elm.Dot())
    d += elm.Line().at(c2.center).tox(1.0)
    d += elm.Dot(open=True).label('CLK\n(J1.4↔J2.4)', loc='left', fontsize=8)
    d += elm.Line().at(c2.center).to(c1n.center).label('bussed', loc='bottom', fontsize=8)
    d += elm.Line().at(u2['/PL']).toy(ypl)
    p2 = d.add(elm.Dot())
    d += elm.Line().at(u1['/PL']).toy(ypl)
    p1n = d.add(elm.Dot())
    d += elm.Line().at(p2.center).tox(1.0)
    d += elm.Dot(open=True).label('/PL\n(J1.5↔J2.5)', loc='left', fontsize=8)
    d += elm.Line().at(p2.center).to(p1n.center)

    d += elm.Label().at((13.8, 9.0)).label(
        '0.1 µF at every IC VCC.\nJ1/J2 not interchangeable (HW-13).\nNo addressing — modules identical.',
        fontsize=9, halign='left')
    d.save('docs/schematics/expansion-board.svg')
print('expansion ok')

# =================================================================== BASE ===
with schemdraw.Drawing(show=False) as d:
    d.config(fontsize=10)
    d += elm.Label().label('pinled — Base board (ESP32-S3 + first 16 channels)',
                           fontsize=15).at((0, 19.4))

    # --- power row (top) ---
    d += elm.Dot(open=True).at((0, 18)).label('5 V in (one buck at the\ncontroller if fed from 12 V /\nsolenoid rail)', loc='left', fontsize=9)
    d += elm.Diode().right().label('D1 Schottky (HW-7)', loc='top', fontsize=9)
    pw = d.add(elm.Dot())
    d += elm.Capacitor(polar=True).at(pw.center).down(1.6).label('C1 bulk', loc='right', fontsize=9)
    d += elm.Ground()
    d += elm.Line().at(pw.center).right(1.6)
    r5 = d.add(elm.Dot())
    d += elm.Label().at(r5.center).label('5 V rail → harness J2.1, strip, U3', loc='top', ofst=.3, fontsize=9)
    ldo = d.add(elm.Ic(theta=0, pins=[
        elm.IcPin(name='IN', side='left', slot='1/1'),
        elm.IcPin(name='OUT', side='right', slot='1/1'),
        elm.IcPin(name='GND', side='bottom', slot='1/1')],
        edgepadW=.6, edgepadH=.4).anchor('IN').at((r5.center[0]+2.2, r5.center[1])).label('U2 3V3 LDO', loc='top', ofst=.2, fontsize=9))
    d += elm.Line().at(r5.center).to(ldo.IN)
    d += elm.Line().at(ldo.OUT).right(0.7)
    d += elm.Vdd().label('3V3 (S3 + logic)', fontsize=9)
    d += elm.Ground().at(ldo.GND)

    # --- ESP32 (pins ordered so every route is collision-free) ---
    s3 = d.add(elm.Ic(theta=0, pins=[
        elm.IcPin(name='GPIO8', side='right', slot='6/6', anchorname='led'),
        elm.IcPin(name='GPIO9', side='right', slot='5/6', anchorname='miso'),
        elm.IcPin(name='GPIO18', side='right', slot='4/6', anchorname='clk'),
        elm.IcPin(name='GPIO17', side='right', slot='3/6', anchorname='pl'),
        elm.IcPin(name='GPIO39', side='left', slot='2/6', anchorname='px'),
        elm.IcPin(name='GPIO0', side='left', slot='1/6', anchorname='btn')],
        edgepadH=1.0, edgepadW=1.0).at((3.0, 9.5)).label('U1 ESP32-S3\n(bare module)', loc='top', ofst=.2, fontsize=10))

    # LED path: GPIO8 row aligned straight into U3.A
    g08 = d.add(elm.Ic(theta=0, pins=[
        elm.IcPin(name='A', side='left', slot='2/2'),
        elm.IcPin(name='B', side='left', slot='1/2'),
        elm.IcPin(name='Y', side='right', slot='1/1'),
        elm.IcPin(name='VCC', side='top', slot='1/1'),
        elm.IcPin(name='GND', side='bottom', slot='1/1')],
        edgepadW=.6, edgepadH=.4).anchor('A').at((s3.led[0]+3.2, s3.led[1]))
        .label('U3 SN74AHCT1G08\n(inputs tied = buffer, HW-9)', loc='bottom', ofst=(0, -1.35), fontsize=9))
    d += elm.Line().at(s3.led).to(g08.A)
    d += elm.Line().at(g08.B).left(0.5)
    bstub = d.add(elm.Dot())
    d += elm.Wire('|-').at(bstub.center).to(g08.A)
    d += elm.Vdd().at(g08.VCC).label('5 V', fontsize=9)
    d += elm.Ground().at(g08.GND)
    d += elm.Resistor().at(g08.Y).right().label('R2 300–500 Ω\n(at first pixel)', loc='top', fontsize=9)
    d += elm.Dot(open=True).label('LED DATA →\nstring (5 V)', loc='right', fontsize=9)

    # left-side: status pixel + button
    d += elm.Line().at(s3.px).left(1.2)
    d += elm.Dot(open=True).label('status WS2812\n(HW-15; pwr-en GPIO38)', loc='left', fontsize=9)
    d += elm.Line().at(s3.btn).left(0.8)
    sw = d.add(elm.Switch().left().label('SW1 BOOT (HW-16)', loc='bottom', fontsize=9))
    d += elm.Ground().at(sw.end)

    # registers below
    u6 = d.add(elm.Ic(**reg165()).right().at((6.0, 3.4)).label('U6 74LVC165\nch 8–15', loc='top', ofst=.2, fontsize=9))
    u5 = d.add(elm.Ic(**reg165()).right().at((12.0, 3.4)).label('U5 74LVC165\nch 0–7', loc='top', ofst=.2, fontsize=9))
    d += elm.Wire('c', k=1).at(u6.QH).to(u5.SER).label('QH→SER', fontsize=8, ofst=(0,-.25))

    # MISO: its own corridor — right of U5, up, across BELOW U3 (a straight
    # run on the pin row would slice through U3's box), then up the gap
    # between the S3 and U3 into GPIO9. Crossings with CLK//PL rows are
    # plain wire crossings, no junctions.
    ymiso = 7.4
    xgap = s3.miso[0] + 1.6
    d += elm.Line().at(u5.QH).right(1.2)
    m1 = d.add(elm.Dot())
    d += elm.Resistor().at(m1.center).down(1.8).idot().label('R3 10 k\n(MISO)', loc='right', ofst=(.5, 0), fontsize=8)
    d += elm.Ground()
    d += elm.Line().at(m1.center).toy(ymiso)
    d += elm.Line().tox(xgap)
    d += elm.Line().toy(s3.miso[1])
    d += elm.Line().to(s3.miso)

    # CLK / PL: source-termination resistors then bus rails below registers
    # CLK and /PL: source-terminate at the S3, then run FAR RIGHT of U5 and
    # down to bus rails below the registers — no crossings with data wires.
    yclk, ypl = -0.3, -1.0
    xbus = 17.2
    d += elm.Resistor().at(s3.clk).right(1.2).label('R4 33–100 Ω', loc='top', fontsize=8)
    ck = d.add(elm.Line().tox(xbus))
    d += elm.Line().at(ck.end).toy(yclk)
    ckd = d.add(elm.Dot())
    d += elm.Resistor().at(s3.pl).right(1.2).label('R5 33–100 Ω', loc='bottom', fontsize=8)
    pl = d.add(elm.Line().tox(xbus + 0.7))
    d += elm.Line().at(pl.end).toy(ypl)
    pld = d.add(elm.Dot())
    d += elm.Line().at(u6.CLK).toy(yclk)
    c6 = d.add(elm.Dot())
    d += elm.Line().at(u5.CLK).toy(yclk)
    c5 = d.add(elm.Dot())
    d += elm.Line().at(c6.center).to(ckd.center).label('CLK bus → J2.4', loc='bottom', fontsize=8)
    d += elm.Line().at(u6['/PL']).toy(ypl)
    p6 = d.add(elm.Dot())
    d += elm.Line().at(u5['/PL']).toy(ypl)
    p5 = d.add(elm.Dot())
    d += elm.Line().at(p6.center).to(pld.center).label('/PL bus → J2.5', loc='bottom', fontsize=8)

    # downstream data in
    d += elm.Dot(open=True).at((0.6, u6.SER[1])).label('J2.3 DATA\n(downstream in)', loc='left', fontsize=8)
    j2 = d.add(elm.Line().right(1.4))
    d += elm.Line().to(u6.SER)
    d += elm.Resistor().at(j2.end).down(2.0).label('R6 10 k', loc='right', fontsize=8)
    d += elm.Ground()

    d += elm.Label().at((0, 6.2)).label(
        'front end ×16 → U5/U6 inputs\n(identical to the expansion sheet:\ndivider, half-wave D, 3V3 clamp,\n2N7000, 74LVC14)',
        fontsize=9, halign='left')
    d += elm.Label().at((6.0, -1.8)).label(
        'J2 (downstream, 5-pin JST-SH): 1 VCC 5 V · 2 GND · 3 DATA in · 4 CLK · 5 /PL — one SPI transaction reads 16·N bits (HW-4: ≤8 modules).\n'
        'Single-point ground tie to machine lamp return (HW-6). Strip power sized separately (~60 mA/LED white), never from U2.',
        fontsize=8, halign='left')
    d.save('docs/schematics/base-board.svg')
print('base ok')
