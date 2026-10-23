import sys
from PIL import Image, ImageDraw, ImageFilter
S=2048; K=S/1024
out=sys.argv[1]
img=Image.new('RGBA',(S,S),(0,0,0,0))
bg=Image.new('RGBA',(S,S))
px=bg.load()
d=ImageDraw.Draw(bg)
for y in range(S):
    t=y/S
    d.line([(0,y),(S,y)],fill=(int(22-10*t),int(39-17*t),int(58-21*t),255))
mask=Image.new('L',(S,S),0)
ImageDraw.Draw(mask).rounded_rectangle([0,0,S-1,S-1],radius=int(210*K),fill=255)
img.paste(bg,(0,0),mask)
glow=Image.new('RGBA',(S,S),(0,0,0,0))
ImageDraw.Draw(glow).ellipse([int(250*K),int(250*K),int(774*K),int(774*K)],fill=(246,184,87,120))
glow=glow.filter(ImageFilter.GaussianBlur(int(80*K)))
img=Image.alpha_composite(img,glow)
d=ImageDraw.Draw(img)
cream=(244,233,207,255); amber=(246,184,87,255); pale=(255,224,167,255)
def R(b,r,**k): d.rounded_rectangle([int(v*K) for v in b],radius=int(r*K),**k)
# handle
d.arc([int(400*K),int(150*K),int(624*K),int(330*K)],180,360,fill=cream,width=int(30*K))
# caps
R((372,300,652,350),25,fill=cream)
R((372,740,652,790),25,fill=cream)
# body
R((340,340,684,750),110,fill=amber)
R((420,410,604,690),70,fill=pale)
R((470,470,554,630),40,fill=(255,247,225,255))
# cast arcs
for i,(rad) in enumerate((370,445)):
    box=[int((512-rad)*K),int((545-rad)*K),int((512+rad)*K),int((545+rad)*K)]
    col=cream if i==0 else (244,233,207,150)
    d.arc(box,-38,38,fill=col,width=int(30*K))
    d.arc(box,142,218,fill=col,width=int(30*K))
img=img.resize((512,512),Image.LANCZOS)
img.save(out)
