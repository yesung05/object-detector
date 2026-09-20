"""Render the paper workflow and re-layout existing S34 evidence panels."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

BASE = Path(__file__).resolve().parents[1]
OUT = BASE / 'figures' / 'ver2'
OUT.mkdir(exist_ok=True)
FONT = 'C:/Windows/Fonts/malgun.ttf'

def font(size):
    return ImageFont.truetype(FONT, size)

im = Image.new('RGB', (1500, 1080), 'white')
d = ImageDraw.Draw(im)

def box(rect, text, fill='#f1f4f7'):
    d.rounded_rectangle(rect, radius=12, fill=fill, outline='#263746', width=3)
    size = 27
    while size > 17:
        selected_font = font(size)
        b = d.multiline_textbbox((0, 0), text, font=selected_font, spacing=8, align='center')
        if b[2] - b[0] <= rect[2] - rect[0] - 28 and b[3] - b[1] <= rect[3] - rect[1] - 20:
            break
        size -= 1
    d.multiline_text(((rect[0]+rect[2]-(b[2]-b[0]))/2-b[0], (rect[1]+rect[3]-(b[3]-b[1]))/2-b[1]), text, font=selected_font, spacing=8, align='center', fill='#172632')

def arrow(points, color='#263746'):
    d.line(points, fill=color, width=4)
    x,y=points[-1]; px,py=points[-2]
    if x==px:
        sign=1 if y>py else -1
        tri=[(x,y),(x-9,y-sign*16),(x+9,y-sign*16)]
    else:
        sign=1 if x>px else -1
        tri=[(x,y),(x-sign*16,y-9),(x-sign*16,y+9)]
    d.polygon(tri,fill=color)

box((490,30,1010,115),'Video Input · Surface ROI Setup')
box((30,185,430,300),'Reference Registration\nAverage of 5 Unoccluded Samples')
box((490,185,1010,300),'Current Surface Sampling\n96 × 96 RGB')
box((1070,185,1470,300),'Person Localization\nOccupancy · Occlusion Mask')
box((490,380,1010,495),'Reference Comparison · Brightness Correction\nChange Mask · Component Filtering')
box((490,600,1010,735),'Alert Policy\nHold During Occupancy · Defer Under Occlusion\nVacancy Grace · Persistence Check')
box((1070,380,1470,495),'Selective AI Inspection\nReuse for Similar Candidates', '#eaf4ee')
box((490,850,1010,965),'Operator Dashboard\nNormal · Inspection Required\nOccupied · Occluded')
arrow([(750,115),(750,185)])
arrow([(490,72),(230,72),(230,185)])
arrow([(1010,72),(1270,72),(1270,185)])
arrow([(750,300),(750,380)])
arrow([(230,300),(230,435),(490,435)])
arrow([(1070,250),(1040,250),(1040,355),(780,355),(780,380)])
arrow([(750,495),(750,600)])
arrow([(1010,438),(1070,438)], '#377451')
arrow([(1270,495),(1270,650),(1010,650)], '#377451')
arrow([(1470,250),(1485,250),(1485,760),(1035,760),(1035,705),(1010,705)])
arrow([(750,735),(750,850)])
d.text((1115,550),'Object Metadata',font=font(24),fill='#377451')
d.text((300,1005),'Rule-based alert decision with AI-assisted candidate metadata',font=font(25),fill='#263746')
im.save(OUT/'monitoring-workflow.png')
im.save(OUT/'monitoring-workflow.jpg',quality=95,subsampling=0)

# Preserve image content from the inspected four-panel figure; replace only labels/layout.
src=Image.open(BASE/'figures/s34-mask-sample/s34-surface-mask-pipeline.png').convert('RGB')
canvas=Image.new('RGB',(1800,480),'white')
draw=ImageDraw.Draw(canvas)
regions=[(0,54,800,394),(800,54,1600,394),(800,504,1600,844)]
titles=['(a) Original Frame and Change Candidate','(b) Reference Surface','(c) Final Change Mask']
notes=['S34 · Video time: 141 s','Average of 5 unoccluded samples','1 connected component · 57 cells']
for i,(region,title,note) in enumerate(zip(regions,titles,notes)):
    tile=src.crop(region)
    tile.thumbnail((580,340),Image.Resampling.LANCZOS)
    canvas.paste(tile,(i*600+(600-tile.width)//2,65+(340-tile.height)//2))
    draw.text((i*600+20,15),title,font=font(28),fill='black')
    draw.text((i*600+20,425),note,font=font(23),fill='#263746')
canvas.save(OUT/'s34-three-panels.png')
canvas.save(OUT/'s34-three-panels.jpg',quality=95,subsampling=0)
print(OUT)
