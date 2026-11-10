"""Dedicated X11 selection process. Only a passed private socket is accepted."""
import base64
import json
import mmap
import os
import select
import socket
import sys
from .clipboard import Focus,TEXT,PNG

_MAX_WIRE=24*1024*1024


def main():
    from .x11 import initialize_xlib_threads
    from .clipboard_x11 import ClipboardX11
    if len(sys.argv)!=3:raise ValueError('private actor descriptors required')
    channel=socket.socket(fileno=int(sys.argv[1]));gate=mmap.mmap(int(sys.argv[2]),1)
    initialize_xlib_threads();completed=[];pending=None;epoch=0;buffer=bytearray()
    clip=ClipboardX11(Focus('peer',0,False),lambda target,data:completed.append((target,data)),exclusive_process=True,watch_changes=True)
    def send(message):
        data=json.dumps(message,separators=(',',':')).encode()+b'\n'
        if len(data)>_MAX_WIRE:raise RuntimeError('clipboard IPC message too large')
        channel.sendall(data)
    def cancel_revoked_read():
        nonlocal pending
        if pending and gate[0]!=1:
            clip.cancel_read();pending=None;completed.clear()
    try:
        last_version=clip.local_version();send({'type':'ready','version':last_version})
        while True:
            readable,_,_=select.select([channel],[],[],.01)
            cancel_revoked_read()
            if readable:
                data=channel.recv(65536)
                if not data:return
                parts=data.split(b'\n');buffer.extend(parts[0])
                if len(buffer)>_MAX_WIRE:raise RuntimeError('clipboard IPC message too large')
                for tail in parts[1:]:
                    line=buffer;buffer=bytearray(tail)
                    command=json.loads(line);ident=command.get('id');op=command.get('op')
                    if type(ident) is not int or ident<=0 or not isinstance(op,str):raise ValueError('invalid clipboard command')
                    if op=='focus':
                        value=command.get('epoch')
                        if type(value) is not int or not 0<=value<2**64:raise ValueError('invalid clipboard focus')
                        epoch=value;pending=None;completed.clear();clip.set_focus(Focus('peer',epoch,bool(epoch)))
                        last_version=clip.local_version();send({'id':ident,'ok':True,'version':last_version})
                    elif op=='version':
                        last_version=clip.local_version();send({'id':ident,'ok':True,'version':last_version})
                    elif op=='read':
                        if not epoch or gate[0]!=1:
                            send({'id':ident,'ok':True,'started':False});continue
                        if pending is not None:raise RuntimeError('clipboard read already pending')
                        last_version=clip.local_version()
                        targets=command.get('targets');started=clip.read_preferred('peer',epoch,targets)
                        pending=(ident,epoch,last_version) if started else None
                        send({'id':ident,'ok':True,'started':started,'version':last_version})
                    elif op=='cancel':
                        clip.cancel_read();pending=None;completed.clear();send({'id':ident,'ok':True})
                    elif op=='install':
                        if not epoch or gate[0]!=1 or command.get('epoch')!=epoch:
                            send({'id':ident,'ok':True,'applied':False,'version':clip.local_version()});continue
                        target=command.get('target');version=command.get('version')
                        if target not in (TEXT,PNG):raise ValueError('invalid clipboard target')
                        raw=base64.b64decode(command.get('data',''),validate=True)
                        applied=clip.install_remote_if_unchanged('peer',epoch,target,raw,version,guard=lambda:gate[0]==1)
                        installed_version=clip.last_install_version
                        last_version=clip.local_version();send({'id':ident,'ok':True,'applied':applied,'version':last_version,'installed_version':installed_version})
                    elif op=='stop':send({'id':ident,'ok':True});return
                    else:raise ValueError('unknown clipboard command')
            cancel_revoked_read();clip.pump()
            if completed:
                target,data=completed.pop(0);version=clip.local_version()
                if pending and gate[0]==1 and pending[1]==epoch:
                    if version==pending[2]:
                        send({'type':'read','id':pending[0],'epoch':epoch,'target':target,'data':base64.b64encode(data).decode('ascii'),'version':version})
                    else:send({'type':'read_empty','id':pending[0],'epoch':epoch})
                pending=None
            elif pending and not clip.read_window:
                send({'type':'read_empty','id':pending[0],'epoch':pending[1]});pending=None
            if clip.version!=last_version:
                last_version=clip.version;send({'type':'version','version':last_version})
    finally:
        clip.close();gate.close();channel.close()


if __name__=='__main__':
    try:main()
    except BaseException:sys.exit(1)
