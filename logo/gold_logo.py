import sys, numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
FONT=sys.argv[1]; text=sys.argv[2]; out=sys.argv[3]; W=int(sys.argv[4]); H=int(sys.argv[5])
S=4                                        # supersampling
w,h=W*S,H*S
# text mask, fitted to the box
size=int(h*0.78)
while True:
    f=ImageFont.truetype(FONT,size)
    bb=f.getbbox(text)
    if bb[2]-bb[0] <= w*0.92 and bb[3]-bb[1] <= h*0.80: break
    size-=4
m=Image.new('L',(w,h),0); d=ImageDraw.Draw(m)
x=(w-(bb[2]-bb[0]))//2-bb[0]; y=(h-(bb[3]-bb[1]))//2-bb[1]
d.text((x,y),text,font=f,fill=255)
M=np.asarray(m,np.float32)/255
top=y+bb[1]; bot=y+bb[3]
# bevel: height field = blurred mask; light from upper left
hgt=np.asarray(m.filter(ImageFilter.GaussianBlur(S*5)),np.float32)/255
gy,gx=np.gradient(hgt)
L=np.array([-0.55,-0.75,0.36]); L/=np.linalg.norm(L)
nx,ny,nz=-gx*S*22,-gy*S*22,np.ones_like(gx)
n=np.sqrt(nx*nx+ny*ny+nz*nz); nx/=n; ny/=n; nz/=n
diff=np.clip(nx*L[0]+ny*L[1]+nz*L[2],0,1)
Hh=np.array([L[0],L[1],L[2]+1]); Hh/=np.linalg.norm(Hh)
spec=np.clip(nx*Hh[0]+ny*Hh[1]+nz*Hh[2],0,1)**40
# metallic gold bands across the letter height (polished bar look)
t=np.clip((np.arange(h)[:,None]-top)/max(1,bot-top),0,1)*np.ones((1,w))
stops=[(0.00,(255,236,160)),(0.18,(250,210,92)),(0.42,(184,128,26)),(0.50,(122,82,10)),
       (0.58,(196,142,34)),(0.78,(255,214,100)),(1.00,(150,100,18))]
def band(t):
    r=np.zeros(t.shape+(3,),np.float32)
    for (a,ca),(b,cb) in zip(stops,stops[1:]):
        k=((t>=a)&(t<=b))[...,None]; u=((t-a)/(b-a))[...,None]
        r=np.where(k,np.array(ca)*(1-u)+np.array(cb)*u,r)
    return r
base=band(t)
shade=0.55+0.65*diff[...,None]
col=base*shade+spec[...,None]*np.array([255,250,225])*0.9
# fine brushed texture
rng=np.random.default_rng(7)
noise=np.asarray(Image.fromarray((rng.random((h//S,w))*255).astype(np.uint8)).resize((w,h)).filter(ImageFilter.GaussianBlur(1)),np.float32)/255
col*= (0.96+0.08*noise)[...,None]
col=np.clip(col,0,255)
# dark rim just outside the letters and a soft drop shadow, on black
edge=np.asarray(m.filter(ImageFilter.MaxFilter(S*3+1 if (S*3)%2==0 else S*3)),np.float32)/255
sh=Image.fromarray((M*255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(S*6))
sh=np.roll(np.roll(np.asarray(sh,np.float32)/255,S*6,0),S*5,1)
bg=np.zeros((h,w,3),np.float32)
bg+= (np.array([60,40,6])*np.clip(sh,0,1)[...,None])*0.8           # warm shadow glow
rim=np.clip(edge-M,0,1)[...,None]*np.array([70,48,8])
img=bg*(1-M[...,None])+rim*(1-M[...,None])+col*M[...,None]
im=Image.fromarray(np.clip(img,0,255).astype(np.uint8)).resize((W,H),Image.LANCZOS)
im.save(out)
