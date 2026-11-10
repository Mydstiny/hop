"""Strict experimental Hop v2 payload codec, per docs/V2_SESSION.md."""
import struct


class CodecError(ValueError): pass


def _prefix(op): return struct.pack('!BBH', 1, op, 0)
def _unpack(fmt, data):
    if len(data) != struct.calcsize(fmt): raise CodecError('invalid payload length')
    return struct.unpack(fmt, data)
def _header(data):
    if not isinstance(data, bytes) or len(data) < 4: raise CodecError('invalid payload')
    schema, op, reserved = struct.unpack('!BBH', data[:4])
    if schema != 1 or reserved: raise CodecError('invalid schema or reserved field')
    return op, data[4:]


def encode_control(event, **fields):
    if event=='screens':
        screens=fields['screens']
        if not isinstance(screens,(tuple,list)) or len(screens)>64: raise CodecError('invalid screens')
        try:
            body=struct.pack('!HH',len(screens),0)+b''.join(struct.pack('!QiiII',*s) for s in screens)
            payload=_prefix(3)+body
            decode_control(payload)
            return payload
        except (struct.error, TypeError, ValueError) as exc: raise CodecError('invalid screens') from exc
    if event in ('hello','hello_ack'):
        op=1 if event=='hello' else 2
        role,platform,capabilities=fields['role'],fields['platform'],fields['capabilities']
        if role not in (1,2) or platform not in (1,2,3,4) or type(capabilities) is not int or capabilities & ~63 or capabilities < 0: raise CodecError('invalid hello')
        return _prefix(op)+struct.pack('!BBHQ',role,platform,0,capabilities)
    if event=='enter':
        sid,x,y,mods,buttons=(fields[k] for k in ('screen_id','x','y','modifiers','buttons'))
        if not 1<=sid<2**64 or not 0<=x<=65535 or not 0<=y<=65535 or mods&~511 or buttons&~31: raise CodecError('invalid enter')
        return _prefix(16)+struct.pack('!QHHHBB',sid,x,y,mods,buttons,0)
    if event=='entered':
        sid=fields['screen_id']
        if not 1<=sid<2**64: raise CodecError('invalid screen')
        return _prefix(17)+struct.pack('!Q',sid)
    if event in ('release','bye'):
        reason=fields.get('reason',0)
        if reason not in (0,1,2): raise CodecError('invalid reason')
        return _prefix(18 if event=='release' else 31)+struct.pack('!B3x',reason)
    if event=='released': return _prefix(19)
    if event in ('pair_confirm','pair_cancel'): return _prefix(4 if event=='pair_confirm' else 5)
    if event in ('clipboard_ready','clipboard_pause'): return _prefix(21 if event=='clipboard_ready' else 22)
    if event=='heartbeat': return _prefix(30)+struct.pack('!Q',fields['nonce'])
    if event=='request_leave':
        sid,x,y=(fields[k] for k in ('screen_id','x','y'))
        if not 1<=sid<2**64 or not 0<=x<=65535 or not 0<=y<=65535: raise CodecError('invalid leave')
        return _prefix(20)+struct.pack('!QHH',sid,x,y)
    raise CodecError('unknown control')


def decode_control(payload):
    op,body=_header(payload)
    if op in (1,2):
        role,platform,reserved,caps=_unpack('!BBHQ',body)
        if role not in (1,2) or platform not in (1,2,3,4) or reserved or caps&~63: raise CodecError('invalid hello')
        return ('hello' if op==1 else 'hello_ack',dict(role=role,platform=platform,capabilities=caps))
    if op==16:
        sid,x,y,mods,buttons,reserved=_unpack('!QHHHBB',body)
        if not sid or mods&~511 or buttons&~31 or reserved: raise CodecError('invalid enter')
        return ('enter',dict(screen_id=sid,x=x,y=y,modifiers=mods,buttons=buttons))
    if op==17:
        sid,=_unpack('!Q',body)
        if not sid: raise CodecError('invalid screen')
        return ('entered',dict(screen_id=sid))
    if op in (18,31):
        reason,reserved=_unpack('!B3s',body)
        if reason not in (0,1,2) or reserved != b'\0'*3: raise CodecError('invalid reason')
        return ('release' if op==18 else 'bye',dict(reason=reason))
    if op==19:
        _unpack('!',body);return ('released',{})
    if op in (4,5):
        _unpack('!',body);return ('pair_confirm' if op==4 else 'pair_cancel',{})
    if op in (21,22):
        _unpack('!',body);return ('clipboard_ready' if op==21 else 'clipboard_pause',{})
    if op==30:
        nonce,=_unpack('!Q',body);return ('heartbeat',dict(nonce=nonce))
    if op==20:
        sid,x,y=_unpack('!QHH',body)
        if not sid: raise CodecError('invalid screen')
        return ('request_leave',dict(screen_id=sid,x=x,y=y))
    if op==3:
        if len(body)<4: raise CodecError('invalid screens')
        count,reserved=struct.unpack('!HH',body[:4])
        if reserved or count>64 or len(body)!=4+24*count: raise CodecError('invalid screens')
        screens=[]; ids=set()
        for i in range(count):
            sid,x,y,w,h=struct.unpack_from('!QiiII',body,4+24*i)
            if not sid or sid in ids or not 1<=w<=65535 or not 1<=h<=65535 or x+w>2**31-1 or y+h>2**31-1: raise CodecError('invalid screen')
            ids.add(sid);screens.append((sid,x,y,w,h))
        return ('screens',dict(screens=screens))
    raise CodecError('unknown control')


def encode_input(event, **f):
    if event=='move': return _prefix(1)+struct.pack('!QHH',f['screen_id'],f['x'],f['y'])
    if event=='button': return _prefix(2)+struct.pack('!QHHBBBB',f['screen_id'],f['x'],f['y'],f['button'],f['down'],f['buttons'],0)
    if event=='wheel': return _prefix(3)+struct.pack('!ii',f['horizontal'],f['vertical'])
    if event=='key': return _prefix(4)+struct.pack('!HHBBH',f['usage'],f['modifiers'],f['down'],f['repeat'],0)
    raise CodecError('unknown input')


def decode_input(payload):
    op,body=_header(payload)
    if op==1:
        sid,x,y=_unpack('!QHH',body)
        if not sid: raise CodecError('invalid screen')
        return ('move',dict(screen_id=sid,x=x,y=y))
    if op==2:
        sid,x,y,button,down,buttons,reserved=_unpack('!QHHBBBB',body)
        if not sid or button not in (1,2,3,4,5) or down not in (0,1) or buttons&~31 or reserved: raise CodecError('invalid button')
        if bool(buttons & (1 << (button-1))) != bool(down): raise CodecError('invalid button mask')
        return ('button',dict(screen_id=sid,x=x,y=y,button=button,down=down,buttons=buttons))
    if op==3:
        h,v=_unpack('!ii',body)
        if abs(h)>120000 or abs(v)>120000: raise CodecError('invalid wheel')
        return ('wheel',dict(horizontal=h,vertical=v))
    if op==4:
        usage,mods,down,repeat,reserved=_unpack('!HHBBH',body)
        if not usage or mods&~511 or down not in (0,1) or repeat not in (0,1) or (repeat and not down) or reserved: raise CodecError('invalid key')
        return ('key',dict(usage=usage,modifiers=mods,down=down,repeat=repeat))
    raise CodecError('unknown input')


def decode_clipboard_control(payload):
    op,body=_header(payload)
    if op==1:
        ident,fmt,reserved,total=_unpack('!QB3sI',body)
        if not ident or fmt not in (1,2) or reserved != b'\0'*3 or total > (2*1024*1024 if fmt==1 else 16*1024*1024) or (fmt==2 and not total):
            raise CodecError('invalid clipboard start')
        return ('start',dict(transfer_id=ident,format=fmt,total=total))
    if op==2:
        ident,reason,reserved=_unpack('!QB3s',body)
        if not ident or reserved != b'\0'*3: raise CodecError('invalid clipboard cancel')
        return ('cancel',dict(transfer_id=ident,reason=reason))
    if op==3:
        if body:raise CodecError('invalid clipboard initial completion')
        return ('initial_done',{})
    raise CodecError('unknown clipboard control')


def encode_clipboard_control(event, **fields):
    if event=='start':
        payload=_prefix(1)+struct.pack('!QB3xI',fields['transfer_id'],fields['format'],fields['total'])
    elif event=='cancel':
        payload=_prefix(2)+struct.pack('!QB3x',fields['transfer_id'],fields.get('reason',0))
    elif event=='initial_done' and not fields:
        payload=_prefix(3)
    else:raise CodecError('unknown clipboard control')
    decode_clipboard_control(payload)
    return payload


def encode_clipboard_chunk(transfer_id, offset, data):
    if not isinstance(data,bytes):raise CodecError('invalid clipboard data')
    try:payload=_prefix(1)+struct.pack('!QI',transfer_id,offset)+data
    except (struct.error,TypeError) as exc:raise CodecError('invalid clipboard chunk') from exc
    decode_clipboard_chunk(payload)
    return payload


def decode_clipboard_chunk(payload):
    op,body=_header(payload)
    if op!=1 or not 13<=len(body)<=65532: raise CodecError('invalid clipboard chunk')
    ident,offset=struct.unpack('!QI',body[:12])
    if not ident: raise CodecError('invalid transfer')
    return ('chunk',dict(transfer_id=ident,offset=offset,data=body[12:]))
