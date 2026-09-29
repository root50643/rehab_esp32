"""Exercise real firmware from a PC already connected to its setup AP.

No history is recorded. The TCP control tests require --unwired-machine;
only use that flag with the machine control wires physically disconnected.
Saved settings are restored in finally (the revision necessarily increases).
"""
import argparse
import json
import re
import secrets
import socket
import struct
import time
import urllib.error
import urllib.request

AP='192.168.4.1'
BASE=f'http://{AP}'
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))

def api(path, body=None):
    request=urllib.request.Request(BASE+path, data=None if body is None else json.dumps(body).encode(),
        headers={} if body is None else {'Content-Type':'application/json'})
    with opener.open(request,timeout=5) as response:
        return json.load(response)

def wait_for(fn, seconds=12):
    deadline=time.monotonic()+seconds
    error=None
    while time.monotonic()<deadline:
        try:
            result=fn()
            if result: return result
        except (OSError,urllib.error.URLError) as exc: error=exc
        time.sleep(.25)
    raise AssertionError(f'Timed out waiting for firmware: {error or "condition"}')

def frame(command,data):
    values=bytes([command,len(data)])+bytes(data)
    return b'\x7a'+values+bytes([255-(sum(values)&255),0x7f])

class Peer:
    """Preserve TCP fragments and coalesced replies between expectations."""
    def __init__(self, conn):
        self.conn=conn
        self.buffer=bytearray()
        self.replies=[]
        conn.settimeout(.5)

    def close(self):
        self.conn.close()

    def send(self, command, payload):
        self.conn.sendall(frame(command,payload))

    def expect(self, command, payload, seconds=8):
        end=time.monotonic()+seconds
        wanted=(command,bytes(payload))
        while time.monotonic()<end:
            if wanted in self.replies:
                self.replies.remove(wanted)
                return
            try: data=self.conn.recv(1024)
            except socket.timeout: continue
            if not data: raise ConnectionError('ESP32 closed the TCP connection before the expected reply')
            self.buffer.extend(data)
            while len(self.buffer)>=5:
                if self.buffer[0]!=0x7a: del self.buffer[0]; continue
                size=self.buffer[2]+5
                if len(self.buffer)<size: break
                raw=bytes(self.buffer[:size]); del self.buffer[:size]
                assert raw[-1]==0x7f and (sum(raw[1:-1])&255)==255, 'Invalid TCP reply framing/checksum'
                self.replies.append((raw[1],raw[3:-2]))
                if len(self.replies)>64: raise AssertionError('Unexpected TCP reply flood')
        raise AssertionError(f'Missing expected reply for command {command:02X}')

def machine_idle(status):
    machine=status['machine']
    return (machine['tracking'] is False and machine['pulse']=='idle' and
        machine['queued_controls']==0 and machine['control_ack_queued'] is False and
        machine['level_queued'] is False and machine['level_ack_pending'] is False and
        machine['queued_uart_replies']==0)

class ControlSession:
    def __init__(self, listener, local, original):
        self.listener=listener
        self.local=local
        self.old=original['settings']
        self.peer=None
        self.changed=False
        self.start_attempted=False
        self.stop_confirmed=False

    def save(self, host, ssid=None):
        current=api('/api/config')
        # Set this before POST: a lost HTTP response may still have saved NVS.
        self.changed=True
        return api('/api/config',{'revision':current['revision'],'wifi_ssid':self.old['wifi_ssid'] if ssid is None else ssid,
            'socket_server':host,'heart_rate_address':self.old['heart_rate_address']})

    def accept(self, seconds=12):
        self.listener.settimeout(seconds)
        conn,address=self.listener.accept()
        if address[0]!=AP:
            conn.close()
            raise AssertionError('TCP peer did not use the ESP32 AP address')
        self.peer=Peer(conn)
        return self.peer

    def ensure_stopped(self):
        if self.stop_confirmed and machine_idle(api('/api/status')): return
        current=api('/api/config')
        # Replace a test's deferred unreachable endpoint before STOP. This is
        # still the temporary test configuration, never the original config.
        # Once stopped, a lost ACK can then be recovered through a reconnect.
        if current['pending'] or current['active']['socket_server']!=self.local:
            self.save(self.local)
        deadline=time.monotonic()+35
        error=None
        while time.monotonic()<deadline:
            try:
                if self.peer is None: self.accept(min(5,deadline-time.monotonic()))
                self.peer.send(0x22,[0])
                self.peer.expect(0x22,[0,0xc8],min(10,deadline-time.monotonic()))
                self.stop_confirmed=True
                wait_for(lambda:machine_idle(api('/api/status')),max(.1,deadline-time.monotonic()))
                return
            except (OSError,AssertionError) as exc:
                error=exc
                if self.peer is not None: self.peer.close(); self.peer=None
        raise RuntimeError('Cleanup could not confirm STOP ACK and an idle controller; original settings were NOT restored') from error

    def restore(self):
        try:
            if not self.changed: return
            if self.start_attempted: self.ensure_stopped()
            if not machine_idle(api('/api/status')):
                raise RuntimeError('Controller is not idle; refusing to leave a deferred original configuration')
            def restored():
                config=api('/api/config')
                if config['pending']: return None
                for key in ('wifi_ssid','socket_server','heart_rate_address'):
                    if config['active'][key]!=self.old[key] or config['settings'][key]!=self.old[key]: return None
                if config['settings']['has_password']!=self.old['has_password']: return None
                return config
            # A lost POST response is ambiguous. Verify it, then retry only if
            # the saved/active configuration still has not returned to normal.
            error=None
            for attempt in range(3):
                try: self.save(self.old['socket_server'])
                except OSError as exc: error=exc
                try:
                    result=wait_for(restored,5)
                    break
                except AssertionError as exc: error=exc
            else:
                raise RuntimeError('STOP completed, but restoration of original settings could not be verified') from error
            print(f'PASS: original configuration restored; NVS revision={result["revision"]}',flush=True)
        finally:
            if self.peer is not None: self.peer.close(); self.peer=None

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--unwired-machine',action='store_true')
    parser.add_argument('--skip-scans',action='store_true')
    args=parser.parse_args()
    original=api('/api/config'); status=api('/api/status')
    print(f'INFO: initial NVS revision={original["revision"]}',flush=True)
    assert set(['ap','wifi','tcp','ble','machine','rfid','health']).issubset(status)
    assert re.fullmatch(r'RehabSetup_(?:[0-9A-F]{2}:){5}[0-9A-F]{2}',status['ap']['ssid'])
    assert status['ap']['ssid']!='RehabSetup_00:00:00:00:00:00', 'AP MAC must be read from hardware'
    assert status['ap']['ip']==AP
    assert 'wifi_password' not in json.dumps(original)
    for asset in ('/','/app.js','/style.css'):
        with opener.open(BASE+asset,timeout=5) as response: assert response.status==200 and len(response.read())>100
    # Common OS captive probe must lead to the local portal.
    with opener.open(BASE+'/generate_204',timeout=5) as response: assert response.geturl()==BASE+'/'
    query=struct.pack('!HHHHHH',0x1234,0x100,1,0,0,0)+b'\x07example\x04test\0'+struct.pack('!HH',1,1)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as dns:
        dns.settimeout(3); dns.sendto(query,(AP,53)); answer,_=dns.recvfrom(512)
        assert answer[:2]==b'\x12\x34' and answer[-4:]==socket.inet_aton(AP)
    print('PASS: AP MAC suffix, HTTP assets, API, credential redaction, captive redirect and DNS')
    if not args.skip_scans:
        for kind in ('wifi','ble'):
            api(f'/api/scan/{kind}',{})
            result=wait_for(lambda: (value if (value:=api(f'/api/scan/{kind}'))['state'] in ('done','error') else None),20)
            assert result['state']=='done', result.get('error')
            print(f'PASS: {kind} scan completed; {len(result["results"])} result(s), identities not logged')
    if not args.unwired_machine:
        print('TCP actuation tests skipped; use --unwired-machine only with disconnected control wires.')
        return
    assert original['pending'] is False, 'Finish existing pending settings before a control test'
    assert machine_idle(api('/api/status')), 'Control test requires an idle controller'
    assert api('/api/config')['revision']==original['revision'], 'Configuration changed during preflight'
    # Find our AP-side address without sending a datagram.
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as route:
        route.connect((AP,80)); local=route.getsockname()[0]
    assert local.startswith('192.168.4.') and local!=AP, 'PC route must use the ESP32 setup AP'
    with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); listener.bind((local,9999))
        session=ControlSession(listener,local,original)
        try:
            # No password is submitted or read. An unpredictable SSID exercises
            # one failed STA attempt while the setup AP/API must remain alive.
            absent_ssid='RehabAbsent_'+secrets.token_hex(8)
            session.save(session.old['socket_server'],absent_ssid)
            wait_for(lambda:(value:=api('/api/config'))['active']['wifi_ssid']==absent_ssid and not value['pending'])
            attempt_started=time.monotonic()
            def sta_failed():
                value=api('/api/status')
                assert value['ap']['ssid']==status['ap']['ssid'] and value['ap']['ip']==AP
                return (time.monotonic()-attempt_started>=15 and value['wifi']['ssid']==absent_ssid and
                    value['wifi']['state']!='connected' and bool(value['wifi']['last_error']))
            wait_for(sta_failed,25)
            session.save(session.old['socket_server'])
            wait_for(lambda:(value:=api('/api/config'))['active']['wifi_ssid']==session.old['wifi_ssid'] and not value['pending'])
            print('PASS: setup AP/API survived failed STA connection; original SSID restored, identities not logged',flush=True)
            listener.listen(2)
            session.save(local)
            peer=session.accept()
            for selector,key,field in ((0,'rfid','ready'),(2,'ble','subscribed')):
                expected=0xc8 if api('/api/status')[key][field] else 0xc9
                peer.send(0x20,[selector]); peer.expect(0x20,[selector,expected])
            # Fragment a control frame to exercise the device's real stream parser.
            start=frame(0x22,[1]); session.start_attempted=True
            peer.conn.sendall(start[:2]); time.sleep(.05); peer.conn.sendall(start[2:])
            peer.expect(0x22,[1,0xc8]); assert api('/api/status')['machine']['tracking'] is True
            peer.send(0x25,[0,10]); peer.expect(0x25,[0xc8])
            wait_for(lambda:api('/api/status')['machine']['sent_level']==10)
            result=session.save('192.0.2.123'); assert result['pending'] is True
            time.sleep(.5); pending=api('/api/config')
            assert pending['pending'] is True and pending['active']['socket_server']==local
            peer.send(0x22,[0]); peer.expect(0x22,[0,0xc8]); session.stop_confirmed=True
            wait_for(lambda:not api('/api/config')['pending'])
            assert api('/api/config')['active']['socket_server']=='192.0.2.123'
            assert machine_idle(api('/api/status'))
            print('PASS: TCP via AP, status, split START frame, resistance command, STOP ACK and deferred setting apply')
        finally:
            session.restore()

if __name__=='__main__': main()
