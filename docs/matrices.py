# Calcula las matrices Rec.709 -> espacio de salida usadas en src/RawDecoder.cpp.
# Uso: python docs/matrices.py   (requiere numpy)
import numpy as np
def xy2XYZ(x,y): return np.array([x/y,1,(1-x-y)/y])
def rgb2xyz(prim, w):
    P=np.array([xy2XYZ(*p) for p in prim]).T; S=np.linalg.solve(P, xy2XYZ(*w)); return P*S
D65=(0.3127,0.3290); D60=(0.32168,0.33767)
rec709=rgb2xyz([(0.64,0.33),(0.30,0.60),(0.15,0.06)],D65)
rec2020=rgb2xyz([(0.708,0.292),(0.170,0.797),(0.131,0.046)],D65)
ap0=rgb2xyz([(0.7347,0.2653),(0.0,1.0),(0.0001,-0.0770)],D60)
ap1=rgb2xyz([(0.713,0.293),(0.165,0.830),(0.128,0.044)],D60)
B=np.array([[0.8951,0.2664,-0.1614],[-0.7502,1.7135,0.0367],[0.0389,-0.0685,1.0296]])
def cat(ws,wd):
    s=B@xy2XYZ(*ws); d=B@xy2XYZ(*wd); return np.linalg.inv(B)@np.diag(d/s)@B
c=cat(D65,D60)
out={'XYZ_D65':rec709,'REC2020':np.linalg.inv(rec2020)@rec709,'ACESCG':np.linalg.inv(ap1)@c@rec709,'ACES2065':np.linalg.inv(ap0)@c@rec709}
for k,m in out.items():
    print(k, 'rowsum', m.sum(1).round(6)); print(',\n'.join('{'+', '.join(f'{v:.10f}f' for v in r)+'}' for r in m))
