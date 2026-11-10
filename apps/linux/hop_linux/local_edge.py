"""Read-only local edge approach detection; never captures or injects input.

This conservative sampler requires a movement from inside a monitor to its
outermost pixel. It can miss pushes made while already at the edge. A corner
approach is ignored if it could mean two different directions.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class EdgeApproach:
    screen_id: int
    direction: str
    x: int
    y: int


class EdgeSampler:
    def __init__(self, screens):
        self.screens=tuple(screens)
        if not self.screens or len(self.screens)>64:raise ValueError('invalid screens')
        ids=set()
        for screen in self.screens:
            if len(screen)!=5 or any(type(v) is not int for v in screen):raise ValueError('invalid screen')
            i,x,y,w,h=screen
            if i<=0 or i in ids or x<0 or y<0 or w<2 or h<2:raise ValueError('invalid screen geometry')
            ids.add(i)
        self.previous=None

    def sample(self, x, y):
        if type(x) is not int or type(y) is not int:raise ValueError('invalid pointer position')
        previous=self.previous;self.previous=(x,y)
        if previous is None or previous==(x,y):return None
        matches=[]
        for i,sx,sy,w,h in self.screens:
            if not (sx<=x<sx+w and sy<=y<sy+h and sx<=previous[0]<sx+w and sy<=previous[1]<sy+h):continue
            for direction,at_edge,moving_out in (
                ('left',x==sx,x<previous[0]),('right',x==sx+w-1,x>previous[0]),
                ('up',y==sy,y<previous[1]),('down',y==sy+h-1,y>previous[1])):
                if at_edge and moving_out:
                    nx=round((x-sx)*65535/(w-1));ny=round((y-sy)*65535/(h-1))
                    matches.append(EdgeApproach(i,direction,nx,ny))
        return matches[0] if len(matches)==1 else None
