"""Edge routing over a validated 2D device/display layout.

This pure component does not capture input or switch network sessions. Device
identifiers are verified certificate fingerprints, screen IDs are local names.
"""
from dataclasses import dataclass
from .layout import Layout

@dataclass(frozen=True)
class EdgeRoute:
    device_id:str
    screen_id:str
    x:int
    y:int


def route_edge(layout, device_id, screen_id, direction, x, y):
    if not isinstance(layout,Layout) or layout.validate().errors:raise ValueError('valid non-overlapping layout required')
    if direction not in ('left','right','up','down') or any(type(v) is not int or not 0<=v<=65535 for v in (x,y)):
        raise ValueError('invalid edge position')
    source=next((d for d in layout.devices if d.id==device_id),None)
    screen=next((s for s in source.screens if s.id==screen_id),None) if source else None
    if screen is None:raise ValueError('unknown source display')
    sx,sy,sw,sh=source.rectangle(screen)
    # Use exact integer ratios and half-open spans to avoid corner-only links.
    vertical=direction in ('left','right');axis=y if vertical else x
    pos_num=(sy*65535+axis*max(sh-1,0)) if vertical else (sx*65535+axis*max(sw-1,0))
    matches=[]
    for device in layout.devices:
        for candidate in device.screens:
            if device.id==device_id and candidate.id==screen_id:continue
            tx,ty,tw,th=device.rectangle(candidate)
            touches=((direction=='right' and sx+sw==tx) or (direction=='left' and tx+tw==sx) or
                     (direction=='down' and sy+sh==ty) or (direction=='up' and ty+th==sy))
            if not touches:continue
            low=max(sy,ty) if vertical else max(sx,tx)
            high=min(sy+sh,ty+th) if vertical else min(sx+sw,tx+tw)
            if high<=low or not low*65535<=pos_num<high*65535:continue
            origin=ty if vertical else tx;size=th if vertical else tw
            relative=0 if size<=1 else max(0,min(65535,round((pos_num-origin*65535)/(size-1))))
            nx=0 if direction=='right' else 65535 if direction=='left' else relative
            ny=0 if direction=='down' else 65535 if direction=='up' else relative
            matches.append(EdgeRoute(device.id,candidate.id,nx,ny))
    if len(matches)>1:raise ValueError('ambiguous edge layout')
    return matches[0] if matches else None
