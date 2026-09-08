#!/usr/bin/env python3
"""Compare two native punctual-light captures whose material axes differ by90°.

Run anisotropy_axis_x_local and anisotropy_axis_y_local with the BRDF display
validation profile. An elongated highlight must rotate with its material; a
binding failure, ignored angle or isotropic BRDF fails the spatial moments.
"""
import json, math, sys
from pathlib import Path
from check_clearcoat_fixture import capture_path, half_rgba


def moments(run):
    item,path=capture_path(run,'hdr_pre_transmission')
    width,height=item['width'],item['height']
    assert (width,height)==(129,129)
    pixels=half_rgba(path,width,height)
    weights=[sum(p[:3])/3 for p in pixels]
    assert max(weights)>1e-3, 'empty highlight'
    # Restrict to the front face and remove any constant environment response.
    floor=min(weights[y*width+x] for y in range(16,113) for x in range(16,113))
    values=[(x-64,y-64,max(0,weights[y*width+x]-floor)) for y in range(16,113) for x in range(16,113)]
    total=sum(w for x,y,w in values)
    cx=sum(x*w for x,y,w in values)/total;cy=sum(y*w for x,y,w in values)/total
    xx=sum((x-cx)**2*w for x,y,w in values)/total
    yy=sum((y-cy)**2*w for x,y,w in values)/total
    return {'variance_x':xx,'variance_y':yy,'ratio':xx/yy,'total':total,'center':[cx,cy]}

x,y=(moments(Path(p)) for p in sys.argv[1:3])
assert max(x['ratio'],1/x['ratio'])>1.5,(x,y)
assert max(y['ratio'],1/y['ratio'])>1.5,(x,y)
assert (x['ratio']-1)*(y['ratio']-1)<0,(x,y)
assert abs(math.log(x['ratio']*y['ratio']))<.15,(x,y)
assert abs(x['total']-y['total'])/max(x['total'],y['total'])<.05,(x,y)
print(json.dumps({'status':'PASS','axis_x':x,'axis_y':y},indent=2))
