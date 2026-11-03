#!/usr/bin/env python3
# Renders panel-preview.png: the panel SVG with Rack's stock knobs and jacks
# placed on top, and a mock-up of the display. Needs a Rack source checkout for
# the component SVGs and Playwright with Chromium.
#   RACK_RES=/path/to/Rack/res python3 tools/preview.py
import base64, math, os, re, subprocess, sys, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.environ.get("RACK_RES", "Rack/res")
LIB = os.path.join(RES, "ComponentLibrary")
PX = 75 / 25.4          # Rack pixels per mm
SCALE = 3.0             # output pixels per Rack pixel


def uri(path):
    return "data:image/svg+xml;base64," + base64.b64encode(open(path, "rb").read()).decode()


def size(path):
    s = open(path).read(3000)
    w = re.search(r'width="([\d.]+)(mm|px)?"', s)
    h = re.search(r'height="([\d.]+)(mm|px)?"', s)
    f = lambda m: float(m.group(1)) * (PX if m.group(2) == "mm" else 1)
    return f(w), f(h)


items = []


def place(name, x_mm, y_mm, angle=None, centered=True):
    p = os.path.join(LIB, name + ".svg")
    w, h = size(p)
    x, y = x_mm * PX, y_mm * PX
    if centered:
        x -= w / 2
        y -= h / 2
    rot = f"transform:rotate({angle}rad);" if angle is not None else ""
    items.append(f'<img src="{uri(p)}" style="position:absolute;left:{x * SCALE}px;top:{y * SCALE}px;'
                 f'width:{w * SCALE}px;height:{h * SCALE}px;{rot}">')


def knob(kind, x, y, v, amin, amax):
    place(kind + "_bg", x, y)
    place(kind, x, y, amin + (amax - amin) * v)


R = 0.83 * math.pi
T = 0.75 * math.pi
knob("RoundBlackKnob", 15.5, 49.5, 0.5, -R, R)
knob("RoundBlackKnob", 45.5, 49.5, 0.5, -R, R)
knob("RoundBlackKnob", 15.5, 70.5, 0.62, -R, R)
knob("RoundBlackKnob", 45.5, 70.5, 0.7071, -R, R)
place("CKSS_1", 11.0, 87.0)
place("CKSS_0", 30.5, 87.0)
place("VCVBezel", 51.0, 87.0)
for x in [10.5, 30.5, 50.5]:
    place("PJ301M", x, 100.8)
    place("PJ301M", x, 117.4)
W_MM = 60.96
for (x, y) in [(15 / PX, 0), (W_MM - 30 / PX, 0), (15 / PX, 128.5 - 15 / PX), (W_MM - 30 / PX, 128.5 - 15 / PX)]:
    place("ScrewSilver", x, y, centered=False)
font = base64.b64encode(open(os.path.join(RES, "fonts", "ShareTechMono-Regular.ttf"), "rb").read()).decode()
panel = uri(os.path.join(ROOT, "res", "TunedPerc.svg"))
Wpx, Hpx = W_MM * PX * SCALE, 128.5 * PX * SCALE
disp = dict(x=4.0 * PX * SCALE, y=13.0 * PX * SCALE, w=52.96 * PX * SCALE, h=26.0 * PX * SCALE, mm=PX * SCALE)

html = f"""<!doctype html><html><head><style>
@font-face {{ font-family: stm; src: url(data:font/ttf;base64,{font}); }}
body {{ margin:0; background:#111; }}
#p {{ position:relative; width:{Wpx}px; height:{Hpx}px; background:url({panel}); background-size:100% 100%; }}
canvas {{ position:absolute; }}
</style></head><body><div id="p">
<canvas id="d" width="{disp['w']}" height="{disp['h']}" style="left:{disp['x']}px;top:{disp['y']}px"></canvas>
{''.join(items)}
</div>
<script>
const D = {json.dumps(disp)};
document.fonts.load('20px stm').then(draw);
function draw() {{
  const c = document.getElementById('d').getContext('2d');
  const mm = D.mm, W = D.w, H = D.h;
  const C = {{bg:'#0B100F', ink:'#EDEADF', soft:'#86918C', dim:'#26332F', hot:'#E9B44C', teal:'#7FC8B4', wave:'#AFB8B2'}};
  const pad = 1.6*mm, fs = 2.4*mm, fss = 1.9*mm;
  const t = (x,y,s,col,al,size) => {{ c.font = size+'px stm'; c.textAlign = al; c.fillStyle = col; c.fillText(s,x,y); }};
  t(pad, pad+fss, 'tom_low_03', C.soft, 'left', fss);
  t(W-pad, pad+fss, 'sure 92%', C.teal, 'right', fss);
  // detected -> tuned
  const y2 = pad + fss + fs*1.9;
  t(pad, y2, 'E2+12c', C.ink, 'left', fs*1.35);
  t(W/2+2*mm, y2, '>', C.soft, 'center', fs*1.2);
  t(W-pad, y2, 'C2', C.hot, 'right', fs*1.35);
  t(W/2+2*mm, y2+fss*1.3, '-4.12 st', C.soft, 'center', fss*0.9);
  // waveform with analysis window and pitch trace
  const wy = y2 + fss*2.2, wh = H - wy - 4.4*mm, wx = pad, ww = W - 2*pad;
  c.fillStyle = '#121A18'; c.fillRect(wx, wy, ww, wh);
  c.fillStyle = 'rgba(127,200,180,0.16)'; c.fillRect(wx+ww*0.08, wy, ww*0.30, wh);
  c.fillStyle = C.wave;
  for (let i = 0; i < ww; i++) {{ const f=i/ww; const a=Math.min(1,Math.exp(-f*3.2)*(0.8+0.2*Math.sin(i*0.35)))*wh*0.46; c.fillRect(wx+i, wy+wh/2-a, 1, Math.max(0.6,2*a)); }}
  c.strokeStyle = C.hot; c.lineWidth = 3; c.beginPath();
  for (let i = 0; i <= 80; i++) {{ const f=i/80; const p=0.55-0.4*Math.exp(-f*9); const x=wx+ww*f, y=wy+wh*p; i?c.lineTo(x,y):c.moveTo(x,y); }}
  c.stroke();
  // bottom line: what is playing now
  t(pad, H-pad*0.8, 'plays D#2', C.ink, 'left', fss);
  t(W-pad, H-pad*0.8, 'oct +0 fine +0c', C.soft, 'right', fss);
  document.title = 'done';
}}
</script></body></html>"""
out_html = os.path.join(ROOT, "preview.html")
open(out_html, "w").write(html)
js = f"""
const {{ chromium }} = require('playwright');
(async () => {{
  const b = await chromium.launch();
  const p = await b.newPage({{ viewport: {{ width: {int(Wpx)}, height: {int(Hpx)} }} }});
  await p.goto('file://{out_html}');
  await p.waitForFunction(() => document.title === 'done');
  await p.locator('#p').screenshot({{ path: '{os.path.join(ROOT, "panel-preview.png")}' }});
  await b.close();
}})();
"""
subprocess.run(["node", "-e", js], check=True)
print("wrote panel-preview.png")
