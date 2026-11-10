"""Untrusted last-known network addresses keyed by verified peer fingerprint."""
import fcntl
import ipaddress
import json
import os
from .profiles import ProfileStore,_peer,_unique_object


def _record(name,address,port):
    if (not isinstance(name,str) or not name.strip() or len(name)>64 or any(ord(c)<32 for c in name) or
        not isinstance(address,str) or len(address)>128 or type(port) is not int or not 1<=port<=65535):
        raise ValueError('invalid endpoint')
    try:ip=ipaddress.ip_address(address)
    except ValueError as exc:raise ValueError('numeric address required') from exc
    if ip.is_unspecified or ip.is_multicast or (ip.version==6 and ip.is_link_local and '%' not in address):raise ValueError('invalid endpoint address')
    return {'name':name,'address':address,'port':port}


class EndpointStore(ProfileStore):
    """Non-secret address hints; pin authentication is always independently required."""
    def _read(self):
        try:
            if self.path.is_symlink():raise ValueError('endpoint file must not be a symlink')
            with self.path.open('rb') as stream:raw=stream.read(65537)
        except FileNotFoundError:return {}
        if len(raw)>65536:raise ValueError('endpoint file too large')
        try:
            obj=json.loads(raw,object_pairs_hook=_unique_object)
            if not isinstance(obj,dict) or set(obj)!={'version','peers'} or type(obj['version']) is not int or obj['version']!=1 or not isinstance(obj['peers'],dict) or len(obj['peers'])>128:
                raise ValueError('invalid endpoint document')
            result={}
            for pin,item in obj['peers'].items():
                key=_peer(pin)
                if key in result or not isinstance(item,dict) or set(item)!={'name','address','port'}:raise ValueError('invalid endpoint record')
                result[key]=_record(**item)
            return result
        except (TypeError,ValueError,UnicodeDecodeError,json.JSONDecodeError) as exc:raise ValueError('invalid endpoints; existing file preserved') from exc

    def get(self,peer):return self._read().get(_peer(peer))
    def all(self):return self._read()

    def update(self,peer,name,address,port):
        key=_peer(peer);value=_record(name,address,port)
        self.path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
        flags=os.O_CREAT|os.O_RDWR|getattr(os,'O_NOFOLLOW',0)
        fd=os.open(self.path.with_name(self.path.name+'.lock'),flags,0o600)
        with os.fdopen(fd,'a+b') as lock:
            fcntl.flock(lock.fileno(),fcntl.LOCK_EX);items=self._read()
            if key not in items and len(items)>=128:raise ValueError('too many endpoints')
            items[key]=value
            payload=json.dumps({'version':1,'peers':items},ensure_ascii=False,sort_keys=True,separators=(',',':')).encode()
            if len(payload)>65536:raise ValueError('endpoint file too large')
            self._write(payload)
