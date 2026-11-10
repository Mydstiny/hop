"""Secret Service storage; never falls back to files or a plaintext store.

All calls are synchronous and must run off GUI/input threads. Secret Service
and an unlocked user collection are required. Python cannot guarantee erasure
of immutable strings or copies made by GI, so secrets must never be logged.
"""
import json
import re
from .tls import VerifiedPeer


class SecretServiceError(RuntimeError): pass

def _unique_object(pairs):
    result={}
    for key,value in pairs:
        if key in result: raise ValueError('duplicate identity field')
        result[key]=value
    return result


class LibSecretBackend:
    def __init__(self):
        try:
            import gi
            gi.require_version('Secret','1')
            from gi.repository import Secret
        except (ImportError,ValueError) as exc:
            raise SecretServiceError('libsecret GI runtime is unavailable') from exc
        self.secret=Secret
        self.schema=Secret.Schema.new('org.hop.InputShare',Secret.SchemaFlags.NONE,
            {'kind':Secret.SchemaAttributeType.STRING,'id':Secret.SchemaAttributeType.STRING})
        if not self.schema: raise SecretServiceError('cannot initialize secret schema')

    def store(self,kind,identity,value):
        try:
            ok=self.secret.password_store_sync(self.schema,{'kind':kind,'id':identity},self.secret.COLLECTION_DEFAULT,'Hop '+kind,value,None)
            if not ok: raise SecretServiceError('secret service did not store item')
        except SecretServiceError:raise
        except Exception as exc:raise SecretServiceError('secret service store failed or collection locked') from exc

    def lookup(self,kind,identity):
        try:return self.secret.password_lookup_sync(self.schema,{'kind':kind,'id':identity},None)
        except Exception as exc:raise SecretServiceError('secret service lookup failed or collection locked') from exc

    def clear(self,kind,identity):
        try:return bool(self.secret.password_clear_sync(self.schema,{'kind':kind,'id':identity},None))
        except Exception as exc:raise SecretServiceError('secret service removal failed or collection locked') from exc

    def list_ids(self,kind):
        """Enumerate metadata only; callers still verify each stored certificate."""
        try:
            service=self.secret.Service.get_sync(self.secret.ServiceFlags.NONE,None)
            if service is None:raise SecretServiceError('secret service unavailable')
            items=service.search_sync(self.schema,{'kind':kind},self.secret.SearchFlags.ALL,None)
            if len(items)>128:raise SecretServiceError('too many stored peers')
            ids=[]
            for item in items:
                if item.get_locked():raise SecretServiceError('secret service collection locked')
                attributes=item.get_attributes()
                if attributes.get('kind')!=kind or not isinstance(attributes.get('id'),str):raise SecretServiceError('invalid stored peer metadata')
                ids.append(attributes['id'])
            return ids
        except SecretServiceError:raise
        except Exception as exc:raise SecretServiceError('secret service search failed or collection locked') from exc


class SecretStore:
    def __init__(self,backend=None, *, tls_library=None):
        from .tls import TLSLibrary
        if tls_library is not None and not isinstance(tls_library,TLSLibrary): raise ValueError('shared TLS library required')
        self.backend=backend if backend is not None else LibSecretBackend()
        self._identity_validator=tls_library.validate_identity if tls_library else None

    @staticmethod
    def _id(fingerprint):
        if not isinstance(fingerprint,str) or not re.fullmatch(r'[0-9a-fA-F]{64}',fingerprint): raise ValueError('invalid fingerprint')
        return fingerprint.lower()

    def store_peer(self,certificate,fingerprint):
        peer=VerifiedPeer(certificate,self._id(fingerprint))
        self.backend.store('peer',peer.fingerprint,peer.certificate.decode('ascii'))

    def get_peer(self,fingerprint):
        pin=self._id(fingerprint);value=self.backend.lookup('peer',pin)
        if value is None:return None
        if not isinstance(value,str) or len(value)>65536: raise SecretServiceError('invalid stored peer')
        try:return VerifiedPeer(value.encode('ascii'),pin)
        except (UnicodeEncodeError,ValueError) as exc:raise SecretServiceError('stored peer does not match pin') from exc

    def remove_peer(self,fingerprint):return self.backend.clear('peer',self._id(fingerprint))

    def list_peers(self):
        try:ids=self.backend.list_ids('peer')
        except SecretServiceError:raise
        except Exception as exc:raise SecretServiceError('cannot list trusted peers') from exc
        if len(ids)>128:raise SecretServiceError('too many stored peers')
        pins=set()
        for value in ids:
            try:pin=self._id(value)
            except ValueError as exc:raise SecretServiceError('invalid stored peer fingerprint') from exc
            if self.get_peer(pin) is None:raise SecretServiceError('stored peer disappeared or is locked')
            pins.add(pin)
        return tuple(sorted(pins))

    def store_identity(self,certificate,private_key):
        validate_pair=self._identity_validator
        if not callable(validate_pair) or not isinstance(certificate,bytes) or not isinstance(private_key,bytes): raise ValueError('validated certificate and key required')
        if not certificate or not private_key or len(certificate)>65536 or len(private_key)>65536 or b'\0' in certificate+private_key: raise ValueError('invalid identity')
        if not validate_pair(certificate,private_key): raise ValueError('identity certificate and key do not match')
        try: value=json.dumps({'version':1,'certificate':certificate.decode('ascii'),'private_key':private_key.decode('ascii')},separators=(',',':'))
        except UnicodeDecodeError as exc:raise ValueError('identity must be ASCII PEM') from exc
        self.backend.store('identity','local',value)

    def get_identity(self):
        validate_pair=self._identity_validator
        if not callable(validate_pair):raise ValueError('identity validator required')
        value=self.backend.lookup('identity','local')
        if value is None:return None
        if not isinstance(value,str) or len(value)>132000:raise SecretServiceError('invalid stored identity')
        try:
            record=json.loads(value,object_pairs_hook=_unique_object)
            if not isinstance(record,dict) or set(record)!={'version','certificate','private_key'} or type(record['version']) is not int or record['version']!=1:raise ValueError('invalid record')
            cert=record['certificate'].encode('ascii');key=record['private_key'].encode('ascii')
            if not cert or not key or len(cert)>65536 or len(key)>65536 or b'\0' in cert+key or not validate_pair(cert,key):raise ValueError('invalid identity')
            return cert,key
        except (TypeError,ValueError,UnicodeError,KeyError,AttributeError) as exc:raise SecretServiceError('invalid stored identity') from exc

    def remove_identity(self):return self.backend.clear('identity','local')
